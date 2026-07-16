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
 * Benötigte Bibliotheken (Details siehe README.md):
 *   - ESP32-BLE-Keyboard (T-vK) mit aktiviertem USE_NIMBLE
 *   - NimBLE-Arduino 1.4.x
 *   - Adafruit NeoPixel
 */

#include <BleKeyboard.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#define DEBUG_MODE      0

// ---------- WLAN / XR18 ----------
#define WIFI_SSID       "XR18-19-1B-07"  // SSID des XR18 (Access-Point-Modus) bzw. des Band-Routers
#define WIFI_PASS       ""               // XR18-AP ist ab Werk offen; im Client-Modus: WLAN-Passwort
#define XR18_IP         "192.168.1.1"    // im AP-Modus hat das XR18 immer 192.168.1.1
#define XR18_PORT       10024            // OSC-Port der X-AIR-Serie (X32 nutzt 10023)
#define BASS1_CH        1                // Mixer-Kanal Bass 1 (1..16)
#define BASS2_CH        2                // Mixer-Kanal Bass 2 (1..16)
#define XREMOTE_MS      8000             // /xremote hält ~10 s – rechtzeitig erneuern

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

BleKeyboard bleKeyboard("DaTurn", "DPommranz", 100);
Adafruit_NeoPixel pixels(NUM_PIXELS, RGBW_PIN, NEO_RGBW + NEO_KHZ800);
WiFiUDP udp;

const uint8_t bassCh[2] = { BASS1_CH, BASS2_CH };
RTC_DATA_ATTR uint8_t selectedBass = 0;   // 0 = Bass 1, 1 = Bass 2; überlebt den Deep Sleep
bool chMuted[2] = { true, true };         // lokales Abbild von /ch/xx/mix/on (wird vom Mixer synchronisiert)

uint32_t currentColor   = 0xFFFFFFFF;     // ungültig → erzwingt erstes LED-Update
uint32_t flashUntilMs   = 0;
uint8_t  flashR, flashG, flashB, flashW;
uint32_t lastActivityMs = 0;
uint32_t lastXremoteMs  = 0;

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
  udp.beginPacket(XR18_IP, XR18_PORT);
  udp.write(buf, len);
  udp.endPacket();
}

void chOnAddress(char *out, uint8_t ch) {
  sprintf(out, "/ch/%02u/mix/on", ch);
}

// /ch/xx/mix/on: 1 = Kanal an, 0 = gemutet
void sendChOn(uint8_t bassIdx, bool on) {
  char addr[20];
  chOnAddress(addr, bassCh[bassIdx]);
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
      chOnAddress(expect, bassCh[i]);
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
      chOnAddress(addr, bassCh[i]);
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

  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    pinMode(pedals[i].pin, INPUT_PULLUP);
  }

  pixels.begin();
  pixels.clear();
  pixels.show();

  bleKeyboard.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);            // nicht blockierend, Status wird im Loop geprüft
  udp.begin(XR18_PORT);

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

  oscTick(now);
  updateLed(now, bleKeyboard.isConnected());

#if IDLE_SLEEP_MS > 0
  if ((now - lastActivityMs) >= IDLE_SLEEP_MS) goToSleep();
#endif

  delay(2);
}
