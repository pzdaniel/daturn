/*
 * DaTurn – BLE-Fußschalter + Behringer-XR18-Fernsteuerung (AirTurn-Umbau, 4 Pedale)
 * Für Seeed Studio XIAO ESP32C3
 *
 * Pedal-Belegung (von links nach rechts):
 *   1 (D0): Kanal-Umschalter Bass 1 <-> Bass 2   (OSC an XR18 über WLAN)
 *   2 (D1): Pfeil links                          (BLE-Tastatur)
 *   3 (D2): Pfeil rechts                         (BLE-Tastatur)
 *   4 (D3): Mute/Unmute des gewählten Kanals     (OSC an XR18 über WLAN)
 *
 * Konfiguration per Handy: http://daturn.local bzw. IP des ESP im XR18-WLAN.
 * Fallback-Access-Point "DaTurn-Setup" (Passwort: daturn123, Seite: http://192.168.4.1),
 * wenn das konfigurierte WLAN nicht erreichbar ist oder Pedal 4 beim Einschalten
 * gehalten wird.
 *
 * Benötigte Bibliotheken (Details siehe README.md):
 *   - ESP32-BLE-Keyboard (T-vK) mit aktiviertem USE_NIMBLE
 *   - NimBLE-Arduino 1.4.x
 *   - Adafruit NeoPixel
 */

#include <BleKeyboard.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#define DEBUG_MODE      0

// ---------- Werkseinstellungen (per Weboberfläche änderbar, in NVS gespeichert) ----------
#define DEF_WIFI_SSID   "XR18-19-1B-07"  // SSID des XR18 (Access-Point-Modus) bzw. des Band-Routers
#define DEF_WIFI_PASS   ""               // XR18-AP ist ab Werk offen; im Client-Modus: WLAN-Passwort
#define DEF_XR18_IP     "192.168.1.1"    // im AP-Modus hat das XR18 immer 192.168.1.1
#define DEF_BASS1_CH    1                // Mixer-Kanal Bass 1 (1..16)
#define DEF_BASS2_CH    2                // Mixer-Kanal Bass 2 (1..16)

#define XR18_PORT       10024            // OSC-Port der X-AIR-Serie (X32 nutzt 10023)
#define XREMOTE_MS      8000             // /xremote hält ~10 s – rechtzeitig erneuern

// ---------- Setup-Access-Point (Fallback / Ersteinrichtung) ----------
#define AP_SSID         "DaTurn-Setup"
#define AP_PASS         "daturn123"
#define STA_TIMEOUT_MS  30000            // WLAN nach 30 s nicht da -> Setup-AP zusätzlich starten

// ---------- Pins (GPIO-Nummern, XIAO-Beschriftung im Kommentar) ----------
#define RGBW_PIN        10                      // D10 – DIN der RGBW-LED
#define NUM_PIXELS      1

// ---------- Verhalten ----------
#define DEBOUNCE_MS     25                      // Entprellzeit der Pedale
#define FLASH_MS        100                     // heller LED-Blitz beim Auslösen (wie Original)
#define IDLE_SLEEP_MS   (30UL * 60UL * 1000UL)  // Deep Sleep nach 30 min ohne Pedaldruck (0 = nie)

enum PedalAction : uint8_t { ACTION_KEY, ACTION_CH_SWITCH, ACTION_MUTE_TOGGLE };

struct Pedal {
  uint8_t     pin;                        // GPIO
  PedalAction action;
  uint8_t     key;                        // nur bei ACTION_KEY
  const char *name;
  uint8_t     dimR, dimG, dimB, dimW;     // LED-Farbe solange gehalten (Original: Helligkeit 10)
  bool        pressed;                    // entprellter Zustand
  bool        lastReading;                // letzter Rohwert
  uint32_t    lastChangeMs;               // Zeitpunkt der letzten Rohwert-Änderung
};

// XIAO ESP32C3: D0=GPIO2, D1=GPIO3, D2=GPIO4, D3=GPIO5.
// Beim ESP32-C3 können nur GPIO0–5 aus dem Deep Sleep wecken – deshalb genau diese vier.
// Achtung: GPIO2 (D0) ist ein Strapping-Pin – dieses Pedal beim Einschalten nicht gedrückt halten.
Pedal pedals[] = {
  { 2, ACTION_CH_SWITCH,   0,               "ChSwitch",  0,  0, 0, 10, false, false, 0 },  // D0, ganz links
  { 3, ACTION_KEY,         KEY_LEFT_ARROW,  "Left",     10,  0, 0,  0, false, false, 0 },  // D1
  { 4, ACTION_KEY,         KEY_RIGHT_ARROW, "Right",     0, 10, 0,  0, false, false, 0 },  // D2
  { 5, ACTION_MUTE_TOGGLE, 0,               "Mute",     10,  0, 10, 0, false, false, 0 },  // D3, ganz rechts
};
const size_t PEDAL_COUNT = sizeof(pedals) / sizeof(pedals[0]);

struct Config {
  String  ssid;
  String  pass;
  String  xr18Ip;
  uint8_t ch1;
  uint8_t ch2;
} cfg;

BleKeyboard bleKeyboard("DaTurn", "DPommranz", 100);
Adafruit_NeoPixel pixels(NUM_PIXELS, RGBW_PIN, NEO_RGBW + NEO_KHZ800);
WiFiUDP udp;
WebServer server(80);
Preferences prefs;

RTC_DATA_ATTR uint8_t selectedBass = 0;   // 0 = Bass 1, 1 = Bass 2; überlebt den Deep Sleep
bool chMuted[2] = { true, true };         // lokales Abbild von /ch/xx/mix/on (wird vom Mixer synchronisiert)

uint32_t currentColor   = 0xFFFFFFFF;     // ungültig → erzwingt erstes LED-Update
uint32_t flashUntilMs   = 0;
uint8_t  flashR, flashG, flashB, flashW;
uint32_t lastActivityMs = 0;
uint32_t lastXremoteMs  = 0;
bool     apActive       = false;

uint8_t bassChannel(uint8_t idx) { return idx == 0 ? cfg.ch1 : cfg.ch2; }

// ---------- Konfiguration (NVS) ----------

void loadConfig() {
  prefs.begin("daturn", true);
  cfg.ssid   = prefs.getString("ssid", DEF_WIFI_SSID);
  cfg.pass   = prefs.getString("pass", DEF_WIFI_PASS);
  cfg.xr18Ip = prefs.getString("ip",   DEF_XR18_IP);
  cfg.ch1    = prefs.getUChar("ch1", DEF_BASS1_CH);
  cfg.ch2    = prefs.getUChar("ch2", DEF_BASS2_CH);
  prefs.end();
}

void saveConfig() {
  prefs.begin("daturn", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("ip",   cfg.xr18Ip);
  prefs.putUChar("ch1", cfg.ch1);
  prefs.putUChar("ch2", cfg.ch2);
  prefs.end();
}

// ---------- LED ----------

void setLed(uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) {
  const uint32_t c = pixels.Color(r, g, b, w);
  if (c == currentColor) return;          // nur bei Änderung wirklich senden
  currentColor = c;
  pixels.setPixelColor(0, c);
  pixels.show();
}

void flash(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint32_t now) {
  flashR = r; flashG = g; flashB = b; flashW = w;
  flashUntilMs = now + FLASH_MS;
}

// LED-Logik wie im Original (TTGO-Version):
//   - heller Blitz (Helligkeit 100) für 100 ms beim Auslösen
//   - gedimmte Pedalfarbe (Helligkeit 10) solange ein Pedal gehalten wird
//   - Blau 255 im 500-ms-Takt blinkend, solange BLE nicht verbunden ist
//   - Blau 100 dauerhaft im verbundenen Leerlauf
void updateLed(uint32_t now, bool bleConnected) {
  if (now < flashUntilMs) {
    setLed(flashR, flashG, flashB, flashW);
    return;
  }
  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    if (pedals[i].pressed) {
      setLed(pedals[i].dimR, pedals[i].dimG, pedals[i].dimB, pedals[i].dimW);
      return;
    }
  }
  if (!bleConnected) {
    if ((now / 500) % 2 == 0) setLed(0, 0, 255);
    else                      setLed(0, 0, 0);
  } else {
    setLed(0, 0, 100);
  }
}

// ---------- OSC (minimal, ohne Zusatzbibliothek) ----------

size_t oscPad(uint8_t *buf, size_t len) {
  do { buf[len++] = 0; } while (len % 4);
  return len;
}

// value == nullptr → Abfrage (Message ohne Argument), sonst Int-Argument
void oscSend(const char *addr, const int32_t *value) {
  uint8_t buf[64];
  size_t len = strlen(addr);
  memcpy(buf, addr, len);
  len = oscPad(buf, len);
  buf[len++] = ',';
  if (value) buf[len++] = 'i';
  len = oscPad(buf, len);
  if (value) {
    buf[len++] = (*value >> 24) & 0xFF;
    buf[len++] = (*value >> 16) & 0xFF;
    buf[len++] = (*value >>  8) & 0xFF;
    buf[len++] =  *value        & 0xFF;
  }
  udp.beginPacket(cfg.xr18Ip.c_str(), XR18_PORT);
  udp.write(buf, len);
  udp.endPacket();
}

void chOnAddress(char *out, uint8_t ch) {
  sprintf(out, "/ch/%02u/mix/on", ch);
}

// /ch/xx/mix/on: 1 = Kanal an, 0 = gemutet
void sendChOn(uint8_t bassIdx, bool on) {
  char addr[20];
  chOnAddress(addr, bassChannel(bassIdx));
  const int32_t v = on ? 1 : 0;
  oscSend(addr, &v);
  if (DEBUG_MODE) { Serial.print(addr); Serial.println(on ? " 1" : " 0"); }
}

// Antworten/Updates des Mixers einlesen und lokalen Mute-Zustand synchron halten
// (falls z. B. am Tablet gemutet wird)
void oscPoll() {
  while (udp.parsePacket() > 0) {
    uint8_t buf[128];
    const int n = udp.read(buf, sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = 0;
    const char *addr = (const char *)buf;

    for (uint8_t i = 0; i < 2; i++) {
      char expect[20];
      chOnAddress(expect, bassChannel(i));
      if (strcmp(addr, expect) != 0) continue;

      size_t p = strlen(addr) + 1;
      while (p % 4) p++;                       // Padding des Adressteils überspringen
      if (p + 8 <= (size_t)n && buf[p] == ',' && buf[p + 1] == 'i') {
        const size_t v = p + 4;
        const int32_t on = ((int32_t)buf[v] << 24) | ((int32_t)buf[v + 1] << 16) |
                           ((int32_t)buf[v + 2] << 8) | buf[v + 3];
        chMuted[i] = (on == 0);
        if (DEBUG_MODE) { Serial.print(expect); Serial.print(" -> "); Serial.println(on); }
      }
    }
  }
}

void oscTick(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (now - lastXremoteMs >= XREMOTE_MS || lastXremoteMs == 0) {
    lastXremoteMs = now;
    oscSend("/xremote", nullptr);              // Updates abonnieren (hält ~10 s)
    for (uint8_t i = 0; i < 2; i++) {          // Zustände zusätzlich aktiv abfragen
      char addr[20];
      chOnAddress(addr, bassChannel(i));
      oscSend(addr, nullptr);
    }
  }
  oscPoll();
}

// ---------- Pedal-Aktionen ----------

void triggerPedal(Pedal &p, uint32_t now) {
  switch (p.action) {

    case ACTION_KEY:
      if (!bleKeyboard.isConnected()) return;
      bleKeyboard.write(p.key);
      // heller Blitz in der Pedalfarbe, wie im Original
      flash(p.dimR * 10, p.dimG * 10, p.dimB * 10, p.dimW * 10, now);
      if (DEBUG_MODE) { Serial.print(p.name); Serial.println(" arrow sent"); }
      break;

    case ACTION_CH_SWITCH:
      if (WiFi.status() != WL_CONNECTED) return;
      sendChOn(selectedBass, false);           // verlassenen Kanal sicherheitshalber muten
      chMuted[selectedBass] = true;
      selectedBass ^= 1;
      // Blitz zeigt die neue Auswahl: Weiß = Bass 1, Gelb = Bass 2
      if (selectedBass == 0) flash(0, 0, 0, 100, now);
      else                   flash(100, 60, 0, 0, now);
      if (DEBUG_MODE) { Serial.print("Selected bass "); Serial.println(selectedBass + 1); }
      break;

    case ACTION_MUTE_TOGGLE: {
      if (WiFi.status() != WL_CONNECTED) return;
      const bool muteNow = !chMuted[selectedBass];
      sendChOn(selectedBass, !muteNow);
      chMuted[selectedBass] = muteNow;
      // Blitz: Violett = gemutet, Türkis = wieder an
      if (muteNow) flash(100, 0, 100, 0, now);
      else         flash(0, 100, 100, 0, now);
      break;
    }
  }
}

// ---------- Weboberfläche ----------

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      default:   out += s[i];
    }
  }
  return out;
}

void handleRoot() {
  lastActivityMs = millis();

  String wifiState = (WiFi.status() == WL_CONNECTED)
    ? "verbunden mit " + htmlEscape(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ")"
    : "nicht verbunden";
  if (apActive) wifiState += " – Setup-AP aktiv";

  String h;
  h.reserve(3000);
  h += F("<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>DaTurn Setup</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
         "main{max-width:26rem;margin:0 auto;padding:1rem}"
         "h1{font-size:1.4rem}h2{font-size:1rem;margin-top:1.5rem}"
         ".card{background:#1d1d1f;border-radius:12px;padding:1rem;margin:.8rem 0}"
         "label{display:block;margin:.7rem 0 .2rem;font-size:.9rem;color:#aaa}"
         "input{width:100%;box-sizing:border-box;font-size:1.1rem;padding:.6rem;"
         "border-radius:8px;border:1px solid #444;background:#2a2a2d;color:#eee}"
         "button{width:100%;margin-top:1.2rem;font-size:1.1rem;padding:.8rem;"
         "border-radius:8px;border:0;background:#2563eb;color:#fff}"
         ".st td{padding:.15rem .5rem .15rem 0;font-size:.95rem}"
         "</style></head><body><main><h1>DaTurn</h1>");

  h += F("<div class='card'><h2 style='margin-top:0'>Status</h2><table class='st'>");
  h += "<tr><td>WLAN</td><td>" + wifiState + "</td></tr>";
  h += String("<tr><td>BLE</td><td>") + (bleKeyboard.isConnected() ? "verbunden" : "nicht verbunden") + "</td></tr>";
  h += "<tr><td>Gew&auml;hlt</td><td>Bass " + String(selectedBass + 1) +
       " (Kanal " + String(bassChannel(selectedBass)) + ")</td></tr>";
  h += String("<tr><td>Mute</td><td>Bass 1: ") + (chMuted[0] ? "stumm" : "an") +
       " &middot; Bass 2: " + (chMuted[1] ? "stumm" : "an") + "</td></tr>";
  h += F("</table></div>");

  h += F("<form method='POST' action='/save'><div class='card'>"
         "<h2 style='margin-top:0'>WLAN / Mixer</h2>");
  h += "<label>WLAN-Name (SSID)</label><input name='ssid' value='" + htmlEscape(cfg.ssid) + "'>";
  h += "<label>WLAN-Passwort (leer = offen)</label><input name='pass' value='" + htmlEscape(cfg.pass) + "'>";
  h += "<label>XR18-IP-Adresse</label><input name='ip' value='" + htmlEscape(cfg.xr18Ip) + "'>";
  h += F("</div><div class='card'><h2 style='margin-top:0'>Kan&auml;le</h2>");
  h += "<label>Mixer-Kanal Bass 1</label><input name='ch1' type='number' min='1' max='16' value='" + String(cfg.ch1) + "'>";
  h += "<label>Mixer-Kanal Bass 2</label><input name='ch2' type='number' min='1' max='16' value='" + String(cfg.ch2) + "'>";
  h += F("</div><button type='submit'>Speichern &amp; Neustart</button></form>"
         "</main></body></html>");

  server.send(200, "text/html", h);
}

void handleSave() {
  lastActivityMs = millis();

  cfg.ssid   = server.arg("ssid");
  cfg.pass   = server.arg("pass");
  cfg.xr18Ip = server.arg("ip");
  cfg.ch1    = constrain(server.arg("ch1").toInt(), 1, 16);
  cfg.ch2    = constrain(server.arg("ch2").toInt(), 1, 16);
  saveConfig();

  server.send(200, "text/html",
    F("<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'></head>"
      "<body style='font-family:system-ui;background:#111;color:#eee;text-align:center;padding-top:3rem'>"
      "<h1>Gespeichert</h1><p>DaTurn startet neu &ndash; danach ggf. neu verbinden.</p></body></html>"));
  delay(500);
  ESP.restart();
}

void startAp() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);                      // STA versucht parallel weiter zu verbinden
  WiFi.softAP(AP_SSID, AP_PASS);
  apActive = true;
  if (DEBUG_MODE) { Serial.print("Setup-AP: "); Serial.println(WiFi.softAPIP()); }
}

// ---------- Deep Sleep ----------

void goToSleep() {
  if (DEBUG_MODE) Serial.println("Idle – entering deep sleep");
  setLed(0, 0, 0);
  bleKeyboard.end();
  WiFi.disconnect(true);

  uint64_t mask = 0;
  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    mask |= 1ULL << pedals[i].pin;
    gpio_sleep_set_pull_mode((gpio_num_t)pedals[i].pin, GPIO_PULLUP_ONLY);
  }
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();                      // Aufwachen = Neustart ab setup()
}

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  if (DEBUG_MODE) Serial.println("DaTurn (XIAO ESP32C3) starting");

  loadConfig();

  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    pinMode(pedals[i].pin, INPUT_PULLUP);
  }
  delay(10);
  // Pedal 4 (ganz rechts) beim Einschalten gehalten -> Setup-AP sofort starten
  const bool forceAp = (digitalRead(pedals[PEDAL_COUNT - 1].pin) == LOW);

  pixels.begin();
  pixels.clear();
  pixels.show();

  bleKeyboard.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (cfg.ssid.length() > 0) {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());  // nicht blockierend
  }
  if (forceAp || cfg.ssid.length() == 0) startAp();
  udp.begin(XR18_PORT);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();
  MDNS.begin("daturn");                        // -> http://daturn.local
  MDNS.addService("http", "tcp", 80);

  lastActivityMs = millis();
}

void loop() {
  const uint32_t now = millis();

  // --- Pedale einlesen: entprellt, nicht blockierend ---
  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    Pedal &p = pedals[i];
    const bool reading = (digitalRead(p.pin) == LOW);

    if (reading != p.lastReading) {
      p.lastReading  = reading;
      p.lastChangeMs = now;
    }
    if ((now - p.lastChangeMs) >= DEBOUNCE_MS && reading != p.pressed) {
      p.pressed = reading;
      lastActivityMs = now;
      if (!p.pressed) triggerPedal(p, now);    // Auslösen beim Loslassen, wie im Original
    }
  }

  // Konfiguriertes WLAN dauerhaft nicht erreichbar -> Setup-AP zusätzlich anbieten
  if (!apActive && WiFi.status() != WL_CONNECTED && now >= STA_TIMEOUT_MS) startAp();

  server.handleClient();
  oscTick(now);
  updateLed(now, bleKeyboard.isConnected());

#if IDLE_SLEEP_MS > 0
  // Nicht einschlafen, solange jemand auf dem Setup-AP hängt
  if ((now - lastActivityMs) >= IDLE_SLEEP_MS &&
      (!apActive || WiFi.softAPgetStationNum() == 0)) {
    goToSleep();
  }
#endif

  delay(2);
}
