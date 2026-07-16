/*
 * DaTurn – BLE-Fußschalter (AirTurn-Umbau, 4 Pedale)
 * Portierung für Seeed Studio XIAO ESP32C3
 *
 * Benötigte Bibliotheken (Details siehe README.md):
 *   - ESP32-BLE-Keyboard (T-vK) mit aktiviertem USE_NIMBLE
 *   - NimBLE-Arduino 1.4.x
 *   - Adafruit NeoPixel
 */

#include <BleKeyboard.h>
#include <Adafruit_NeoPixel.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#define DEBUG_MODE      0

// ---------- Pins (GPIO-Nummern, XIAO-Beschriftung im Kommentar) ----------
#define RGBW_PIN        10                      // D10 – DIN der RGBW-LED
#define NUM_PIXELS      1

// ---------- Verhalten ----------
#define DEBOUNCE_MS     25                      // Entprellzeit der Pedale
#define FLASH_MS        150                     // LED-Blitz nach Tastendruck
#define IDLE_SLEEP_MS   (30UL * 60UL * 1000UL)  // Deep Sleep nach 30 min ohne Pedaldruck (0 = nie)

struct Pedal {
  uint8_t     pin;                              // GPIO
  uint8_t     key;                              // gesendete Taste
  const char *name;
  uint8_t     r, g, b;                          // LED-Farbe beim Auslösen
  bool        pressed;                          // entprellter Zustand
  bool        lastReading;                      // letzter Rohwert
  uint32_t    lastChangeMs;                     // Zeitpunkt der letzten Rohwert-Änderung
};

// XIAO ESP32C3: D0=GPIO2, D1=GPIO3, D2=GPIO4, D3=GPIO5.
// Beim ESP32-C3 können nur GPIO0–5 aus dem Deep Sleep wecken – deshalb genau diese vier.
// Achtung: GPIO2 (D0) ist ein Strapping-Pin – dieses Pedal beim Einschalten nicht gedrückt halten.
Pedal pedals[] = {
  { 2, KEY_LEFT_ARROW,  "Left",  100,   0,   0, false, false, 0 },  // D0
  { 3, KEY_RIGHT_ARROW, "Right",   0, 100,   0, false, false, 0 },  // D1
  { 4, KEY_UP_ARROW,    "Up",      0,  60,  60, false, false, 0 },  // D2
  { 5, KEY_DOWN_ARROW,  "Down",  100,  60,   0, false, false, 0 },  // D3
};
const size_t PEDAL_COUNT = sizeof(pedals) / sizeof(pedals[0]);

BleKeyboard bleKeyboard("DaTurn", "DPommranz", 100);
Adafruit_NeoPixel pixels(NUM_PIXELS, RGBW_PIN, NEO_RGBW + NEO_KHZ800);

uint32_t currentColor   = 0xFFFFFFFF;           // ungültig → erzwingt erstes LED-Update
uint32_t flashUntilMs   = 0;
uint32_t lastActivityMs = 0;
bool     wasConnected   = false;

void setLed(uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0) {
  const uint32_t c = pixels.Color(r, g, b, w);
  if (c == currentColor) return;                // nur bei Änderung wirklich senden
  currentColor = c;
  pixels.setPixelColor(0, c);
  pixels.show();
}

void goToSleep() {
  if (DEBUG_MODE) Serial.println("Idle – entering deep sleep");
  setLed(0, 0, 0);
  bleKeyboard.end();

  uint64_t mask = 0;
  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    mask |= 1ULL << pedals[i].pin;
    gpio_sleep_set_pull_mode((gpio_num_t)pedals[i].pin, GPIO_PULLUP_ONLY);
  }
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();                       // Aufwachen = Neustart ab setup()
}

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
  lastActivityMs = millis();
}

void loop() {
  const uint32_t now = millis();
  const bool connected = bleKeyboard.isConnected();

  // --- Pedale einlesen: entprellt, nicht blockierend, Auslösen beim Herunterdrücken ---
  for (size_t i = 0; i < PEDAL_COUNT; i++) {
    Pedal &p = pedals[i];
    const bool reading = (digitalRead(p.pin) == LOW);

    if (reading != p.lastReading) {
      p.lastReading  = reading;
      p.lastChangeMs = now;
    }
    if ((now - p.lastChangeMs) >= DEBOUNCE_MS && reading != p.pressed) {
      p.pressed = reading;
      if (p.pressed) {
        lastActivityMs = now;
        if (connected) {
          bleKeyboard.write(p.key);             // Press + Release in einem
          setLed(p.r, p.g, p.b);
          flashUntilMs = now + FLASH_MS;
          if (DEBUG_MODE) { Serial.print(p.name); Serial.println(" sent"); }
        }
      }
    }
  }

  // --- Status-LED ---
  if (now < flashUntilMs) {
    // Tastendruck-Blitz läuft noch – nichts überschreiben
  } else if (!connected) {
    if ((now / 500) % 2 == 0) setLed(0, 0, 50); // blau blinken: wartet auf Verbindung
    else                      setLed(0, 0, 0);
  } else {
    if (!wasConnected && DEBUG_MODE) Serial.println("BLE connected");
    setLed(0, 3, 0);                            // dezentes Grün: verbunden, bereit
  }
  wasConnected = connected;

#if IDLE_SLEEP_MS > 0
  if ((now - lastActivityMs) >= IDLE_SLEEP_MS) goToSleep();
#endif

  delay(2);
}
