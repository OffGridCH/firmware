#include <Adafruit_NeoPixel.h>

#define PIN_STRIP_1 45
#define PIN_STRIP_2 46

#define NUM_LEDS 4 

Adafruit_NeoPixel strip1(NUM_LEDS, PIN_STRIP_1, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip2(NUM_LEDS, PIN_STRIP_2, NEO_GRB + NEO_KHZ800);

void setup() {
  strip1.begin();
  strip2.begin();
  
  strip1.setBrightness(50);
  strip2.setBrightness(50);
  
  strip1.show(); 
  strip2.show();
}

void loop() {
 
  for(int i = 0; i < NUM_LEDS; i++) {
    strip1.setPixelColor(i, strip1.Color(0, 0, 0));
    strip2.setPixelColor(i, strip2.Color(255, 255, 255));
  }
  strip1.show(); 
  strip2.show(); 
  
  delay(250); 
 
  for(int i = 0; i < NUM_LEDS; i++) {
    strip1.setPixelColor(i, strip1.Color(255, 255, 255));
    strip2.setPixelColor(i, strip2.Color(0, 0, 0));
  }
  strip1.show();
  strip2.show();
  
  delay(250); 
}
