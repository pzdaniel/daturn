// TFT_eSPI-Konfiguration für das Waveshare ESP32-S3-Touch-LCD-1.47
//
// Für die Arduino IDE: den kompletten Inhalt der Datei
//   Arduino/libraries/TFT_eSPI/User_Setup.h
// durch den Inhalt dieser Datei ersetzen.
// (Der CI-Build setzt dieselben Werte automatisch als Compiler-Flags.)

#define ST7789_DRIVER
#define TFT_WIDTH   172
#define TFT_HEIGHT  320

#define TFT_MOSI    45
#define TFT_SCLK    40
#define TFT_CS      42
#define TFT_DC      41
#define TFT_RST     39
#define TFT_BL      48
#define TFT_BACKLIGHT_ON HIGH

#define TFT_RGB_ORDER TFT_BGR
#define USE_HSPI_PORT              // ohne dieses Flag bleibt das Display schwarz!

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

#define SPI_FREQUENCY 8000000      // konservativ; 27 MHz kann man ausprobieren
