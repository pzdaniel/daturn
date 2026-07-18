# DaTurn – BLE-Fußschalter + XR18-Fernsteuerung auf Seeed Studio XIAO ESP32C3

Fußschalter mit **4 Pedalen** (AirTurn-Umbau), portiert vom TTGO T-Display (siehe `legacy/`)
auf den Seeed Studio XIAO ESP32C3. Zwei Pedale blättern per BLE-Tastatur Noten um, die beiden
äußeren Pedale steuern ein **Behringer XR18** per OSC über WLAN: Umschalten zwischen zwei
Mixer-Kanälen und Mute/Unmute des gewählten Kanals – z. B. für zwei Instrumente, von denen
immer nur eines gespielt wird.

## Pedal-Belegung (von links nach rechts)

| Pedal | XIAO-Pin | GPIO | Funktion | Weg |
|---|---|---|---|---|
| 1 (ganz links) | D0 | 2 | Umschalter Mixer-Kanal A ↔ B | OSC/WLAN |
| 2 | D1 | 3 | Pfeil links (zurückblättern) | BLE-Tastatur |
| 3 | D2 | 4 | Pfeil rechts (vorblättern) | BLE-Tastatur |
| 4 (ganz rechts) | D3 | 5 | Mute/Unmute des gewählten Kanals | OSC/WLAN |

Pedale jeweils gegen **GND** schalten (interne Pull-ups aktiv, keine externen Widerstände nötig).
RGBW-LED (DIN) an **D10** (GPIO 10).

**Logik der Kanal-Umschaltung:** Pedal 1 wechselt den „gewählten“ Kanal (`DEF_CH_A`/`DEF_CH_B`
im Sketch bzw. per Weboberfläche, Standard: Kanal 01 und 02) und **mutet dabei automatisch den
verlassenen Kanal** – so können nie beide Kanäle gleichzeitig offen sein. Pedal 4 toggelt Mute
des gewählten Kanals. Typischer Ablauf beim Instrumentenwechsel: Pedal 4 (mute) → Instrument
wechseln → Pedal 1 (Kanal umschalten) → Pedal 4 (unmute). Der Mute-Zustand wird laufend mit dem
Mixer synchronisiert – auch wenn zwischendurch am Tablet gemutet wird, stimmt der Toggle.

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
| Umschalten auf Kanal A | Weiß (W-Kanal) |
| Umschalten auf Kanal B | Gelb |
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

## Konfiguration per Handy (Weboberfläche)

Der ESP32 hostet eine kleine mobile Konfig-Seite (WLAN-Zugangsdaten, XR18-IP, beide
Kanalnummern plus Status-Anzeige). Die Einstellungen landen im Flash (NVS) und
überleben Neustart und Deep Sleep; die `DEF_*`-Defines im Sketch sind nur noch die
Werkseinstellungen.

**Normalbetrieb (empfohlen):** Der ESP hängt als Client im XR18-WLAN. Handy einfach
im selben XR18-WLAN lassen und **http://daturn.local** öffnen (falls der Handy-Browser
mDNS nicht auflöst: die IP des ESP steht im seriellen Monitor bzw. in der Client-Liste
des Routers). So erreicht das Handy gleichzeitig die X-AIR-App **und** die Konfig-Seite –
kein Umschalten nötig.

**Setup-Access-Point (Ersteinrichtung/Fallback):** Wenn das konfigurierte WLAN nicht
erreichbar ist (30 s Timeout) oder **Pedal 4 beim Einschalten gehalten** wird, spannt
der ESP zusätzlich ein eigenes WLAN auf:

- SSID `DaTurn-Setup`, Passwort `daturn4444`, Seite: **http://4.4.4.4**

Nach dem Verbinden öffnet sich die Konfig-Seite automatisch als
**Netzwerkanmeldeseite** (Captive Portal, wie bei WLED). Falls nicht:
`http://4.4.4.4` manuell im Browser öffnen.

Hinweis zur Merk-IP `4.4.4.4`: Das ist eigentlich eine öffentliche Internet-Adresse.
Lädt die Seite am Handy nicht, kurz die **mobilen Daten ausschalten** – manche Handys
schicken die Anfrage sonst übers Mobilfunknetz ins echte Internet statt an den ESP.
Wer das vermeiden will, trägt im Sketch z. B. `192.168.4.4` als `AP_IP` ein.

Zum Konflikt mit dem XR18: Die beiden Funknetze stören sich nicht (verschiedene SSIDs;
läuft der ESP gleichzeitig als Client + AP, teilen sich beide sogar denselben Kanal).
Der praktische Haken ist das **Handy**: Es kann nur in einem WLAN gleichzeitig sein –
hängt es am `DaTurn-Setup`-AP, erreicht es das Mischpult (X-AIR-App) nicht. Deshalb ist
der AP nur Fallback und die Konfig-Seite läuft normalerweise im XR18-WLAN mit.
Solange ein Gerät mit dem Setup-AP verbunden ist, geht der ESP nicht in den Deep Sleep.

Werkseinstellungen im Sketch:

```cpp
#define DEF_WIFI_SSID  "XR18-19-1B-07"  // SSID des XR18-Access-Points (steht auf dem Gerät/im Setup)
#define DEF_WIFI_PASS  ""               // AP-Modus ab Werk offen
#define DEF_XR18_IP    "192.168.1.1"    // AP-Modus: immer 192.168.1.1
#define DEF_CH_A       1                // Mixer-Kanal A
#define DEF_CH_B       2                // Mixer-Kanal B
```

BLE (Umblättern) und WLAN (Mixer) laufen gleichzeitig – der ESP32-C3 teilt sich ein Funkmodul
für beides (Koexistenz-Modus), was für Tastendrücke und kleine UDP-Pakete problemlos reicht.
Drückt man ein Mixer-Pedal, solange das WLAN (noch) nicht verbunden ist, passiert nichts –
erkennbar am ausbleibenden LED-Blitz. Gleiches gilt wie bisher für die Blätter-Pedale ohne
BLE-Verbindung.

## Bibliotheken / Arduino-IDE-Setup

1. **Board-Paket:** `esp32` von Espressif, **Version 2.0.17** (im Boardverwalter über das
   Versions-Dropdown wählen!). Boardauswahl: *XIAO_ESP32C3*.
   Für den seriellen Monitor ggf. *USB CDC On Boot: Enabled* setzen.
2. **ESP32-BLE-Keyboard** (T-vK): als ZIP von GitHub installieren – unverändert lassen
   (`USE_NIMBLE` **nicht** aktivieren).
3. **Adafruit NeoPixel** über den Bibliotheksverwalter.

**Warum Core 2.0.17 und nicht 3.x?** Die ESP32-BLE-Keyboard-Bibliothek ist mit dem
esp32-Core 3.x nicht kompatibel: Mit dem Standard-Stack kompiliert sie dort nicht,
und mit NimBLE-Arduino 1.4.x kompiliert sie zwar, **crasht aber beim Start des
BT-Controllers in einer Boot-Schleife** (Guru Meditation Error, Sprung auf 0x0).
Core 2.0.17 + Standard-Stack (Bluedroid) ist die seit Jahren stabile Kombination.
Langfristige Alternative wäre ein Umstieg auf NimBLE-Arduino 2.x mit eigener
HID-Implementierung oder einem gepflegten Fork.

## Flashen (Schritt für Schritt)

1. Repo herunterladen; der Ordner `DaTurn/` mit `DaTurn.ino` muss so heißen bleiben.
2. Arduino IDE 2.x: Boardverwalter-URL
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json` eintragen,
   Paket **esp32 (Espressif Systems)** in Version **2.0.17** installieren (nicht 3.x,
   siehe oben), Board **XIAO_ESP32C3** wählen, *USB CDC On Boot: Enabled* und
   *Partition Scheme: Huge App (3MB No OTA)* setzen.
3. Bibliotheken wie oben beschrieben installieren (NeoPixel, ESP32-BLE-Keyboard als
   ZIP, unverändert).
4. XIAO per USB-C anschließen, Port wählen, Upload. Falls der Upload nicht startet:
   BOOT-Taste („B“) gedrückt halten, während man den XIAO einsteckt (Bootloader-Modus),
   nach dem Flashen Reset drücken.
5. Erster Test: LED blinkt blau → „DaTurn“ am Tablet koppeln. Danach Pedal 4 gedrückt
   einschalten → WLAN `DaTurn-Setup` → `http://4.4.4.4` → XR18-Daten eintragen.
   Fehlersuche: `DEBUG_MODE 1` setzen und seriellen Monitor mit 115200 Baud öffnen.

## Display-Variante: Waveshare ESP32-S3-Touch-LCD-1.47 (`DaTurn_S3/`)

Alternative Hardware mit 1,47″-Farbdisplay (172×320, ST7789): Das Display zeigt den
**aktiven Kanal groß als A/B** (grün = an, rot = stumm), Mixer-Kanalnummer, BLE-/WLAN-Status
und im Setup-Modus die Konfig-Adresse. Funktional identisch zur XIAO-Variante
(gleiche Pedale, LED-Logik, Weboberfläche, XR18-Steuerung); der Web-Installer
erkennt automatisch, welches Board angeschlossen ist.

| Anschluss | GPIO |
|---|---|
| Pedal 1 (ganz links, Kanal-Umschalter) | 4 |
| Pedal 2 (Pfeil links) | 5 |
| Pedal 3 (Pfeil rechts) | 6 |
| Pedal 4 (ganz rechts, Mute) | 7 |
| Externe RGBW-Status-LED (optional, DIN) | 11 |

Pedale wieder gegen **GND** (interne Pull-ups). Besonderheiten:

- Das Board hat **keine eigene RGB-LED** – als Status-LED kann optional die RGBW-LED
  aus dem XIAO-Aufbau an GPIO 11 weiterverwendet werden (gleiche Farblogik). Ohne LED
  läuft alles normal, das Display zeigt den Status ja ohnehin an.
- **Display:** Die Touch-Variante nutzt einen JD9853-Controller mit eigener Verdrahtung
  (SCK=38, MOSI=39, CS=21, DC=45, RST=40, Backlight=46; GPIO 41/42/47/48 gehören dem
  Touch-Chip). Angesteuert wird es wie im Waveshare-Demo über **Arduino_GFX** mit dem
  ST7789-Treiber (Offsets 34/0).
- **Aufwecken aus dem Deep Sleep nur über Pedal 1** (der esp32-Core 2.x kann beim S3
  nur einen einzelnen Wakeup-Pin). Display und LED gehen im Deep Sleep aus.
- **Akku:** 3,7-V-LiPo an den Akku-Anschluss des Boards (bzw. VBAT-Pin), Laden über USB-C
  übernimmt das Board. Akkustands-Anzeige per `setBatteryLevel()` ist vorbereitet,
  sobald der Mess-GPIO des Boards verifiziert ist (siehe Waveshare-Wiki).
- Arduino IDE: Board **ESP32S3 Dev Module** wählen (Flash Size 16MB, PSRAM „OPI PSRAM“,
  Partition Scheme „Huge App“, USB CDC On Boot Enabled), zusätzlich die Bibliothek
  **„GFX Library for Arduino“** (Arduino_GFX) in Version **1.4.9** über den
  Bibliotheksverwalter installieren (neuere Versionen brauchen den esp32-Core 3.x) –
  keine weitere Konfiguration nötig. Der CI-Build macht das automatisch.

## Weitere Hinweise

- **Deep Sleep:** nach 30 min ohne Pedaldruck (konfigurierbar über `IDLE_SLEEP_MS`).
  Jedes Pedal weckt das Gerät; der erste Druck nach dem Aufwachen dient nur dem
  Aufwecken/Neuverbinden. Die Kanal-Auswahl (A/B) überlebt den Deep Sleep
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
