# DaTurn – BLE-Fußschalter (AirTurn-Umbau) auf Seeed Studio XIAO ESP32C3

BLE-Tastatur-Fußschalter mit **4 Pedalen** (Pfeiltasten links/rechts/oben/unten), z. B. zum
Umblättern von Noten auf dem Tablet. Portierung des ursprünglichen TTGO-T-Display-Sketches
(siehe `legacy/`) auf den Seeed Studio XIAO ESP32C3.

## Warum der alte Sketch nicht direkt läuft

| Thema | Alt (TTGO T-Display) | Neu (XIAO ESP32C3) |
|---|---|---|
| Taster-Pins | GPIO 35 / GPIO 0 | existieren auf dem C3 nicht → jetzt GPIO 2–5 (D0–D3) |
| NeoPixel-Pin | GPIO 22 | existiert nicht → jetzt GPIO 10 (D10) |
| TFT (135×240, `TFT_eSPI`) | onboard | XIAO hat kein Display → entfernt (RGBW-LED zeigt den Status) |
| Pedale | nur 2 | alle 4 AirTurn-Pedale |
| BLE-Stack | Bluedroid (Standard) | NimBLE (Pflicht/empfohlen auf dem C3, siehe unten) |

## Verkabelung

| Funktion | XIAO-Pin | GPIO | Taste |
|---|---|---|---|
| Pedal 1 | D0 | 2 | Pfeil links |
| Pedal 2 | D1 | 3 | Pfeil rechts |
| Pedal 3 | D2 | 4 | Pfeil hoch |
| Pedal 4 | D3 | 5 | Pfeil runter |
| RGBW-LED (DIN) | D10 | 10 | – |

Pedale jeweils gegen **GND** schalten (interne Pull-ups sind aktiv, keine externen Widerstände nötig).
Die Tastenbelegung lässt sich in der `pedals[]`-Tabelle im Sketch ändern (z. B. `KEY_PAGE_UP`/`KEY_PAGE_DOWN`).

**Wichtig:** GPIO 2 (D0) ist ein Strapping-Pin des ESP32-C3 – dieses Pedal beim
Einschalten/Reset nicht gedrückt halten, sonst startet der Chip u. U. nicht.
Bewusst wurden GPIO 2–5 gewählt, weil beim C3 **nur GPIO 0–5** aus dem Deep Sleep
wecken können.

## Bibliotheken / Arduino-IDE-Setup

1. **Board-Paket:** `esp32` von Espressif (Boardauswahl: *XIAO_ESP32C3*).
   Für den seriellen Monitor ggf. *USB CDC On Boot: Enabled* setzen.
2. **ESP32-BLE-Keyboard** (T-vK): als ZIP von GitHub installieren.
   Die Original-Bibliothek (Stand 0.3.2) kompiliert mit dem Bluedroid-Standardstack auf
   aktuellen esp32-Cores (3.x) nicht mehr – deshalb **NimBLE aktivieren**:
   in `BleKeyboard.h` die Zeile `#define USE_NIMBLE` einkommentieren.
3. **NimBLE-Arduino** in Version **1.4.x** installieren (2.x hat eine geänderte API und ist
   mit der alten BleKeyboard-Bibliothek nicht kompatibel).
4. **Adafruit NeoPixel** über den Bibliotheksverwalter.

Alternative: fertig auf NimBLE/C3 angepasste Forks wie
[ESP32C3-BLE-Keyboard](https://github.com/pr4u4t/ESP32C3-BLE-Keyboard) oder
[ESP32-NimBLE-Keyboard](https://github.com/wakwak-koba/ESP32-NimBLE-Keyboard) –
dann entfällt das manuelle `USE_NIMBLE`.

## Verhalten

- **Blau blinkend:** wartet auf BLE-Verbindung (Gerätename „DaTurn“).
- **Dezent grün:** verbunden, bereit.
- **Farbblitz:** Pedal ausgelöst (jede Taste hat ihre eigene Farbe).
- **Deep Sleep:** nach 30 min ohne Pedaldruck (konfigurierbar über `IDLE_SLEEP_MS`).
  Jedes Pedal weckt das Gerät wieder auf; der erste Druck nach dem Aufwachen dient nur
  dem Aufwecken/Neuverbinden.

## Verbesserungen gegenüber dem alten Sketch

- Auslösen **beim Herunterdrücken** statt beim Loslassen → spürbar geringere Latenz.
- Saubere `millis()`-Entprellung statt Schleifenzähler; keine blockierenden `delay()`s mehr.
- `bleKeyboard.write()` statt `press()` + 100 ms `delay()` + `releaseAll()`.
- Bugfix: Im alten „Connecting“-Loop wurde `press(KEY_RIGHT_ARROW)` ohne Verbindung
  aufgerufen und nie freigegeben.
- LED wird nur bei Farbwechsel aktualisiert (weniger Stromverbrauch/Bus-Traffic).
- Deep Sleep mit Pedal-Wakeup für Batteriebetrieb.

## Ideen für später

- Batteriestand melden: Spannungsteiler an einen ADC-Pin (der XIAO C3 hat keinen
  eingebauten) und `bleKeyboard.setBatteryLevel()` zyklisch aktualisieren.
- Langdruck-Funktionen (z. B. langer Tritt = andere Taste).
- Kleines I2C-OLED, falls wieder eine Statusanzeige gewünscht ist.
