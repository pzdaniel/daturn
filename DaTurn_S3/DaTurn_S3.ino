/*
 * DaTurn-S3 – BLE-Fußschalter + Behringer-XR18-Fernsteuerung mit Display
 * Für Waveshare ESP32-S3-Touch-LCD-1.47 (1,47"-Display 172x320, JD9853-Controller)
 *
 * Pedal-Belegung (von links nach rechts), Pedale gegen GND an den Pinheader:
 *   1 (GPIO 4): Umschalter Mixer-Kanal A <-> B   (OSC an XR18 über WLAN)
 *   2 (GPIO 5): Pfeil links                      (BLE-Tastatur)
 *   3 (GPIO 6): Pfeil rechts                     (BLE-Tastatur)
 *   4 (GPIO 7): Mute/Unmute des gewählten Kanals (OSC an XR18 über WLAN)
 *
 * Das Display zeigt den aktiven Kanal (A/B), Mute- und Verbindungsstatus.
 * Optionale externe RGBW-Status-LED (z. B. die aus dem XIAO-Aufbau) an GPIO 11 –
 * das Board hat keine eigene RGB-LED; ohne LED läuft alles normal weiter.
 *
 * LCD-Verdrahtung der Touch-Variante (JD9853, ST7789-kompatibles Init):
 *   SCK=38, MOSI=39, CS=21, DC=45, RST=40, Backlight=46
 *   (GPIO 41/42/47/48 gehören dem Touch-Controller AXS5106L)
 *
 * Aufwecken aus dem Deep Sleep: nur über Pedal 1 (ganz links) –
 * der esp32-Core 2.x kann beim S3 nur einen einzelnen EXT0-Wakeup-Pin.
 *
 * Benötigte Bibliotheken (Details siehe README.md):
 *   - ESP32-BLE-Keyboard (T-vK), esp32-Core 2.0.17 (Core 3.x bootloopt, siehe README)
 *   - Adafruit NeoPixel
 *   - GFX Library for Arduino (Arduino_GFX, wie im Waveshare-Demo)
 */

#include <BleKeyboard.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

#define DEBUG_MODE      0

// ---------- Werkseinstellungen (per Weboberfläche änderbar, in NVS gespeichert) ----------
#define DEF_WIFI_SSID   "XR18-19-1B-07"  // SSID des XR18 (Access-Point-Modus) bzw. des Band-Routers
#define DEF_WIFI_PASS   ""               // XR18-AP ist ab Werk offen; im Client-Modus: WLAN-Passwort
#define DEF_XR18_IP     "192.168.1.1"    // im AP-Modus hat das XR18 immer 192.168.1.1
#define DEF_CH_A        1                // Mixer-Kanal A (1..16), z. B. Instrument 1
#define DEF_CH_B        2                // Mixer-Kanal B (1..16), z. B. Instrument 2

#define XR18_PORT       10024            // OSC-Port der X-AIR-Serie (X32 nutzt 10023)
#define XREMOTE_MS      8000             // /xremote hält ~10 s – rechtzeitig erneuern

// ---------- Setup-Access-Point (Fallback / Ersteinrichtung) ----------
#define AP_SSID         "DaTurn-Setup"
#define AP_PASS         "daturn4444"
#define STA_TIMEOUT_MS  30000            // WLAN nach 30 s nicht da -> Setup-AP zusätzlich starten
// Merk-IP des Setup-AP. Achtung: 4.4.4.4 ist eigentlich eine öffentliche Adresse –
// falls die Seite am Handy nicht lädt, mobile Daten kurz ausschalten.
const IPAddress AP_IP(4, 4, 4, 4);
const IPAddress AP_MASK(255, 255, 255, 0);

// ---------- Pins (Waveshare ESP32-S3-Touch-LCD-1.47) ----------
#define RGBW_PIN        11                      // optionale externe RGBW-LED (DIN), frei am Header
#define NUM_PIXELS      1
#define PIN_LCD_BL      46                      // Display-Hintergrundbeleuchtung

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

// GPIO 4-7 liegen frei am Pinheader und sind RTC-fähig (Deep-Sleep-Wakeup über Pedal 1).
Pedal pedals[] = {
  { 4, ACTION_CH_SWITCH,   0,               "ChSwitch",  0,  0, 0, 10, false, false, 0 },  // ganz links
  { 5, ACTION_KEY,         KEY_LEFT_ARROW,  "Left",     10,  0, 0,  0, false, false, 0 },
  { 6, ACTION_KEY,         KEY_RIGHT_ARROW, "Right",     0, 10, 0,  0, false, false, 0 },
  { 7, ACTION_MUTE_TOGGLE, 0,               "Mute",     10,  0, 10, 0, false, false, 0 },  // ganz rechts
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
// Display-Anbindung exakt wie im Waveshare-Arduino-Demo (Arduino_GFX):
// JD9853-Panel läuft mit dem ST7789-Treiber, Offsets 34/0 wegen 172 px Breite
Arduino_DataBus *bus = new Arduino_ESP32SPI(45 /* DC */, 21 /* CS */, 38 /* SCK */, 39 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 40 /* RST */, 1 /* Rotation: Querformat */, false /* IPS */,
                                      172, 320, 34, 0, 34, 0);
WiFiUDP udp;
WebServer server(80);
Preferences prefs;

RTC_DATA_ATTR uint8_t selectedCh = 0;     // 0 = Kanal A, 1 = Kanal B; überlebt den Deep Sleep
bool chMuted[2] = { true, true };         // lokales Abbild von /ch/xx/mix/on (wird vom Mixer synchronisiert)

uint32_t currentColor   = 0xFFFFFFFF;     // ungültig → erzwingt erstes LED-Update
uint32_t flashUntilMs   = 0;
uint8_t  flashR, flashG, flashB, flashW;
uint32_t lastActivityMs = 0;
uint32_t lastXremoteMs  = 0;
bool     apActive       = false;

uint8_t mixerChannel(uint8_t idx) { return idx == 0 ? cfg.ch1 : cfg.ch2; }

// ---------- Konfiguration (NVS) ----------

void loadConfig() {
  prefs.begin("daturn", true);
  cfg.ssid   = prefs.getString("ssid", DEF_WIFI_SSID);
  cfg.pass   = prefs.getString("pass", DEF_WIFI_PASS);
  cfg.xr18Ip = prefs.getString("ip",   DEF_XR18_IP);
  cfg.ch1    = prefs.getUChar("ch1", DEF_CH_A);
  cfg.ch2    = prefs.getUChar("ch2", DEF_CH_B);
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

// ---------- Display ----------

// RGB565-Farben
#define COL_BLACK  0x0000
#define COL_RED    0xF800
#define COL_GREEN  0x07E0
#define COL_YELLOW 0xFFE0
#define COL_WHITE  0xFFFF
#define COL_GREY   0x7BEF

struct UiState {
  bool    ble;
  bool    wifi;
  bool    ap;
  uint8_t ch;
  bool    mutedA;
  bool    mutedB;
};
UiState drawnUi = { false, false, false, 255, true, true };  // ch=255 erzwingt ersten Draw

void drawText(int16_t x, int16_t y, uint8_t size, uint16_t color, const String &s) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void updateDisplay() {
  UiState ui;
  ui.ble    = bleKeyboard.isConnected();
  ui.wifi   = (WiFi.status() == WL_CONNECTED);
  ui.ap     = apActive;
  ui.ch     = selectedCh;
  ui.mutedA = chMuted[0];
  ui.mutedB = chMuted[1];
  if (memcmp(&ui, &drawnUi, sizeof(ui)) == 0) return;   // nur bei Änderung neu zeichnen
  drawnUi = ui;

  const bool muted = (ui.ch == 0) ? ui.mutedA : ui.mutedB;

  gfx->fillScreen(COL_BLACK);

  // Kopfzeile: Verbindungsstatus (Querformat 320x172)
  drawText(8,   8, 2, ui.ble  ? COL_GREEN : COL_RED, "BLE");
  drawText(56,  8, 2, ui.wifi ? COL_GREEN : COL_RED, "WLAN");
  if (ui.ap) drawText(128, 8, 2, COL_YELLOW, "SETUP-AP");
  drawText(240, 8, 2, COL_GREY, "DaTurn");

  // Großer Kanalbuchstabe links, Details rechts
  drawText(36, 44, 12, muted ? COL_RED : COL_GREEN, ui.ch == 0 ? "A" : "B");
  drawText(150, 64, 3, COL_WHITE, "Kanal " + String(mixerChannel(ui.ch)));
  drawText(150, 100, 3, muted ? COL_RED : COL_GREEN, muted ? "STUMM" : "AN");

  // Fußzeile
  if (ui.ap)        drawText(8, 152, 2, COL_GREY, "Setup: http://4.4.4.4");
  else if (ui.wifi) drawText(8, 152, 2, COL_GREY, "http://" + WiFi.localIP().toString());
  else              drawText(8, 152, 2, COL_GREY, "suche WLAN ...");
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
void sendChOn(uint8_t chIdx, bool on) {
  char addr[20];
  chOnAddress(addr, mixerChannel(chIdx));
  const int32_t v = on ? 1 : 0;
  oscSend(addr, &v);
  if (DEBUG_MODE) { Serial.print(addr); Serial.println(on ? " 1" : " 0"); }
}

// Antworten/Updates des Mixers einlesen und lokalen Mute-Zustand synchron halten
void oscPoll() {
  while (udp.parsePacket() > 0) {
    uint8_t buf[128];
    const int n = udp.read(buf, sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = 0;
    const char *addr = (const char *)buf;

    for (uint8_t i = 0; i < 2; i++) {
      char expect[20];
      chOnAddress(expect, mixerChannel(i));
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
      chOnAddress(addr, mixerChannel(i));
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
      sendChOn(selectedCh, false);             // verlassenen Kanal sicherheitshalber muten
      chMuted[selectedCh] = true;
      selectedCh ^= 1;
      // Blitz zeigt die neue Auswahl: Weiß = Kanal A, Gelb = Kanal B
      if (selectedCh == 0) flash(0, 0, 0, 100, now);
      else                 flash(100, 60, 0, 0, now);
      if (DEBUG_MODE) { Serial.print("Selected channel "); Serial.println(selectedCh == 0 ? "A" : "B"); }
      break;

    case ACTION_MUTE_TOGGLE: {
      if (WiFi.status() != WL_CONNECTED) return;
      const bool muteNow = !chMuted[selectedCh];
      sendChOn(selectedCh, !muteNow);
      chMuted[selectedCh] = muteNow;
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
  h += String("<tr><td>Gew&auml;hlt</td><td>Kanal ") + (selectedCh == 0 ? "A" : "B") +
       " (Mixer-Kanal " + String(mixerChannel(selectedCh)) + ")</td></tr>";
  h += String("<tr><td>Mute</td><td>A: ") + (chMuted[0] ? "stumm" : "an") +
       " &middot; B: " + (chMuted[1] ? "stumm" : "an") + "</td></tr>";
  h += F("</table></div>");

  h += F("<form method='POST' action='/save'><div class='card'>"
         "<h2 style='margin-top:0'>WLAN / Mixer</h2>");
  h += "<label>WLAN-Name (SSID)</label><input name='ssid' value='" + htmlEscape(cfg.ssid) + "'>";
  h += "<label>WLAN-Passwort (leer = offen)</label><input name='pass' value='" + htmlEscape(cfg.pass) + "'>";
  h += "<label>XR18-IP-Adresse</label><input name='ip' value='" + htmlEscape(cfg.xr18Ip) + "'>";
  h += F("</div><div class='card'><h2 style='margin-top:0'>Kan&auml;le</h2>");
  h += "<label>Mixer-Kanal A</label><input name='ch1' type='number' min='1' max='16' value='" + String(cfg.ch1) + "'>";
  h += "<label>Mixer-Kanal B</label><input name='ch2' type='number' min='1' max='16' value='" + String(cfg.ch2) + "'>";
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
  delay(100);                                  // DHCP-Server erst nach softAP() umkonfigurieren
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  apActive = true;
  if (DEBUG_MODE) { Serial.print("Setup-AP: "); Serial.println(WiFi.softAPIP()); }
}

// ---------- Deep Sleep ----------

void goToSleep() {
  if (DEBUG_MODE) Serial.println("Idle – entering deep sleep");
  setLed(0, 0, 0);
  digitalWrite(PIN_LCD_BL, LOW);               // Display-Beleuchtung aus
  bleKeyboard.end();
  WiFi.disconnect(true);

  // Beim S3 mit Core 2.x weckt EXT0 genau einen Pin: Pedal 1 (ganz links)
  const gpio_num_t wakePin = (gpio_num_t)pedals[0].pin;
  rtc_gpio_pullup_en(wakePin);
  rtc_gpio_pulldown_dis(wakePin);
  esp_sleep_enable_ext0_wakeup(wakePin, 0);
  esp_deep_sleep_start();                      // Aufwachen = Neustart ab setup()
}

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  if (DEBUG_MODE) Serial.println("DaTurn-S3 (Waveshare Touch-LCD-1.47) starting");

  loadConfig();

  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    pinMode(pedals[i].pin, INPUT_PULLUP);
  }
  pinMode(PIN_LCD_BL, OUTPUT);
  delay(10);
  // Pedal 4 (ganz rechts) beim Einschalten gehalten -> Setup-AP sofort starten
  const bool forceAp = (digitalRead(pedals[PEDAL_COUNT - 1].pin) == LOW);

  pixels.begin();
  pixels.clear();
  pixels.show();

  gfx->begin();
  gfx->fillScreen(COL_BLACK);
  digitalWrite(PIN_LCD_BL, HIGH);              // Hintergrundbeleuchtung an

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
  updateDisplay();

#if IDLE_SLEEP_MS > 0
  // Nicht einschlafen, solange jemand auf dem Setup-AP hängt
  if ((now - lastActivityMs) >= IDLE_SLEEP_MS &&
      (!apActive || WiFi.softAPgetStationNum() == 0)) {
    goToSleep();
  }
#endif

  delay(2);
}
