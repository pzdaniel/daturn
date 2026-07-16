#include <BleKeyboard.h>
#include <Adafruit_NeoPixel.h>
#ifdef __AVR__
 #include <avr/power.h> // Required for 16 MHz Adafruit Trinket
#endif
#include <TFT_eSPI.h>
#include <SPI.h>

#define RIGHT_TURN      35 //4
#define LEFT_TURN       0 //5
#define RGBW_PIN        22

#define DEBUG_MODE      0
int INITIAL = 1;



// RGBW Pixel
int numPixels = 1;
int pixelFormat = NEO_RGBW + NEO_KHZ800;
Adafruit_NeoPixel *pixels;

// BLE-Keyboard
BleKeyboard bleKeyboard("DaTurn", "DPommranz", 100);

// TFT-Display
TFT_eSPI tft = TFT_eSPI(135, 240); // Invoke custom library


void setup() {
  bleKeyboard.begin();
  Serial.begin(115200);
  Serial.println("Starting BLE work!");
    
  pinMode(LEFT_TURN, INPUT_PULLUP);
  pinMode(RIGHT_TURN, INPUT_PULLUP);


  #if defined(__AVR_ATtiny85__) && (F_CPU == 16000000)
    clock_prescale_set(clock_div_1);
  #endif
  pixels = new Adafruit_NeoPixel(numPixels, RGBW_PIN, pixelFormat);
  pixels->begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
  pixels->clear(); // Set all pixel colors to 'off'

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
}

void loop() {

  static int debug = 0, counter_right = 0, counter_left = 0;
  char button_right = digitalRead(RIGHT_TURN) == LOW;
  char button_left = digitalRead(LEFT_TURN) == LOW;

  while (!bleKeyboard.isConnected()) {
    bleKeyboard.press(KEY_RIGHT_ARROW);
    tft.setTextColor(TFT_BLUE, TFT_BLACK);
    tft.drawCentreString("Connecting",120,48,4);
    pixels->setPixelColor(0, pixels->Color(0, 0, 255, 0));
    pixels->show();
    delay(500);
    pixels->clear(); // Set all pixel colors to 'off'
    pixels->show();
    delay(500);
    //i = ( i > 10 ) ? 0 : i++; 
  }
  
  if (INITIAL) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("Connected",120,48,4);
    INITIAL = 0;
  }
  
   

  if(button_right) {
    counter_right++;
    pixels->setPixelColor(0, pixels->Color(0, 10, 0, 0));
  } else if(button_left) {
    counter_left++;
    pixels->setPixelColor(0, pixels->Color(10, 0, 0, 0));
  } else {
    pixels->setPixelColor(0, pixels->Color(0, 0, 100, 0));
  }

  pixels->show();  // Send the updated pixel colors to the hardware.
  

  if (bleKeyboard.isConnected() && !button_right && counter_right > 1) {
    if(DEBUG_MODE) Serial.print("Right Turn: ");
    if(DEBUG_MODE) Serial.println(counter_right);
    bleKeyboard.press(KEY_RIGHT_ARROW);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("Right arrow sent",120,48,4);
    INITIAL = 1;
    pixels->setPixelColor(0, pixels->Color(0, 100, 0, 0));
    pixels->show();   // Send the updated pixel colors to the hardware.
    delay (100);
    bleKeyboard.releaseAll();
    pixels->clear(); // Set all pixel colors to 'off'

    counter_right = 0;
  }
  if (bleKeyboard.isConnected() && !button_left && counter_left > 1) {
    if(DEBUG_MODE) Serial.print("Left Turn: ");
    if(DEBUG_MODE) Serial.println(counter_left);
    bleKeyboard.press(KEY_LEFT_ARROW);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("Left arrow sent",120,48,4);
    INITIAL = 1;
    pixels->setPixelColor(0, pixels->Color(100, 0, 0, 0));
    pixels->show();   // Send the updated pixel colors to the hardware.
    delay (100);
    bleKeyboard.releaseAll();
    pixels->clear(); // Set all pixel colors to 'off'
    
    counter_left = 0;
  }

  if(++debug % 20 == 0 && DEBUG_MODE ){
    Serial.println("sec");
    delay(50);
  }
}
