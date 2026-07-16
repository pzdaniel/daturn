# DaTurn – BLE-Fußschalter + XR18-Fernsteuerung auf Seeed Studio XIAO ESP32C3

Fußschalter mit **4 Pedalen** (AirTurn-Umbau), portiert vom TTGO T-Display (siehe `legacy/`)
auf den Seeed Studio XIAO ESP32C3. Zwei Pedale blättern per BLE-Tastatur Noten um, die beiden
äußeren Pedale steuern ein **Behringer XR18** per OSC über WLAN (Kanal-Umschaltung und Mute
für zwei Bässe).

## Pedal-Belegung (von links nach rechts)

| Pedal | XIAO-Pin | GPIO | Funktion | Weg |
|---|---|---|---|---|
| 1 (ganz links) | D0 | 2 | Umschalter Bass 1 ↔ Bass 2 | OSC/WLAN |
| 2 | D1 | 3 | Pfeil links (zurückblättern) | BLE-Tastatur |
| 3 | D2 | 4 | Pfeil rechts (vorblättern) | BLE-Tastatur |
| 4 (ganz rechts) | D3 | 5 | Mute/Unmute des gewählten Kanals | OSC/WLAN |

Pedale jeweils gegen **GND** schalten (interne Pull-ups aktiv, keine externen Widerstände nötig).
RGBW-LED (DIN) an **D10** (GPIO 10).

**Logik der Bass-Umschaltung:** Pedal 1 wechselt den „gewählten“ Kanal (`BASS1_CH`/`BASS2_CH`
im Sketch, Standard: Kanal 01 und 02) und **mutet dabei automatisch den verlassenen Kanal** –
so können nie beide Bässe gleichzeitig offen sein. Pedal 4 toggelt Mute des gewählten Kanals.
Typischer Ablauf beim Basswechsel: Pedal 4 (mute) → Bass wechseln → Pedal 1 (Kanal umschalten)
→ Pedal 4 (unmute). Der Mute-Zustand wird laufend mit dem Mixer synchronisiert – auch wenn
zwischendurch am Tablet gemutet wird, stimmt der Toggle.

## LED-Anzeige

Die Grundlogik ist **unverändert vom Original** übernommen:

| Zustand | LED |
|---|---|
| BLE nicht verbunden | Blau (255) blinkend im 500-ms-Takt |
| Verbunden, Leerlauf | Blau (100) dauerhaft |
| Pedal gehalten | gedimmte Pedalfarbe (Helligkeit 10) |
| Pedal ausgelöst (beim Loslassen) | helle Pedalfarbe (100) für 100 ms |
| Pfeil links / rechts | Rot / Grün (wie bisher) |

Neu hinzugekommen (nur für die zwei neuen Funktionen):

| Aktion | Blitzfarbe |
|---|---|
| Umschalten auf Bass 1 | Weiß (W-Kanal) |
| Umschalten auf Bass 2 | Gelb |
| Kanal gemutet | Violett |
| Kanal wieder an | Türkis |

## Behringer XR18 – das Wichtigste zur Fernsteuerung

- Das **X AIR XR18** ist ein 18-Kanal-Digitalmixer (16 Midas-Preamps, 18×18-USB-Interface)
  mit eingebautem **Tri-Mode-WLAN-Router**: Access Point, WiFi-Client oder Ethernet/LAN.
- **Access-Point-Modus** (REMOTE-Schalter auf „ACCESS POINT“): Das Mischpult spannt ein
  offenes 2,4-GHz-WLAN auf (SSID z. B. `XR18-19-1B-07`), hat dann **immer die IP
  192.168.1.1** und vergibt an bis zu 4 Clients Adressen ab 192.168.1.101. Der ESP32-C3
  funkt ebenfalls nur 2,4 GHz – passt also.
- **WiFi-Client-/LAN-Modus:** Mixer hängt im Band-Router-Netz; dann im Sketch SSID/Passwort
  des Routers und die dortige Mixer-IP eintragen.
- Gesteuert wird das XR18 über das offene **OSC-Protokoll auf UDP-Port 10024**
  (X32/M32 nutzen 10023). Darüber laufen auch die offiziellen Apps (X AIR Edit für
  PC/Mac/Linux, X AIR für iOS/Android). Es können mehrere Clients gleichzeitig verbunden
  sein – Tablet-Mischen und Fußschalter stören sich nicht.
- Relevante OSC-Befehle:
  - `/ch/01/mix/on` mit Int-Argument: `1` = Kanal an, `0` = **gemutet** (Kanäle 01–16)
  - Message ohne Argument = Abfrage; der Mixer antwortet mit dem aktuellen Wert
  - `/xremote`: abonniert Parameter-Updates für ~10 s, muss danach erneuert werden
    (der Sketch schickt es alle 8 s und hält so den Mute-Zustand synchron)
- Die vollständige (inoffizielle) Protokoll-Doku stammt von Patrick-Gilles Maillot
  („Unofficial X32/X-AIR OSC Remote Protocol“); Behringer liefert dazu eine
  `Parameters.txt` mit allen OSC-Pfaden.

Der Sketch implementiert OSC minimal selbst (UDP-Pakete von Hand gebaut) – es ist also
**keine zusätzliche OSC-Bibliothek** nötig.

## Konfiguration im Sketch

```cpp
#define WIFI_SSID  "XR18-19-1B-07"  // SSID des XR18-Access-Points (steht auf dem Gerät/im Setup)
#define WIFI_PASS  ""               // AP-Modus ab Werk offen
#define XR18_IP    "192.168.1.1"    // AP-Modus: immer 192.168.1.1
#define BASS1_CH   1                // Mixer-Kanal Bass 1
#define BASS2_CH   2                // Mixer-Kanal Bass 2
```

BLE (Umblättern) und WLAN (Mixer) laufen gleichzeitig – der ESP32-C3 teilt sich ein Funkmodul
für beides (Koexistenz-Modus), was für Tastendrücke und kleine UDP-Pakete problemlos reicht.
Drückt man ein Mixer-Pedal, solange das WLAN (noch) nicht verbunden ist, passiert nichts –
erkennbar am ausbleibenden LED-Blitz. Gleiches gilt wie bisher für die Blätter-Pedale ohne
BLE-Verbindung.

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

## Weitere Hinweise

- **Deep Sleep:** nach 30 min ohne Pedaldruck (konfigurierbar über `IDLE_SLEEP_MS`).
  Jedes Pedal weckt das Gerät; der erste Druck nach dem Aufwachen dient nur dem
  Aufwecken/Neuverbinden. Die Kanal-Auswahl (Bass 1/2) überlebt den Deep Sleep
  (RTC-Speicher), der Mute-Zustand wird nach dem Aufwachen frisch vom Mixer abgefragt.
- **Strapping-Pin:** GPIO 2 (D0, Umschalt-Pedal) beim Einschalten/Reset nicht gedrückt
  halten, sonst startet der Chip u. U. nicht. GPIO 2–5 wurden gewählt, weil beim C3 nur
  GPIO 0–5 Deep-Sleep-Wakeup können.
- **Auslösen beim Loslassen** (wie im Original) – dadurch bleibt auch die LED-Abfolge
  „gedimmt beim Halten → heller Blitz beim Loslassen“ exakt wie gewohnt.

## Ideen für später

- Batteriestand melden: Spannungsteiler an einen ADC-Pin (der XIAO C3 hat keinen
  eingebauten) und `bleKeyboard.setBatteryLevel()` zyklisch aktualisieren.
- Langdruck-Funktionen (z. B. langer Tritt auf Pedal 1 = beide Kanäle muten).
- Statt Kanal-Mute die Fader fahren (`/ch/xx/mix/fader`, Float 0–1) für weiche Übergänge.
