#include <Arduino.h>

#include "hardwareIOSetup.h"



void setup() {
  
  Serial.begin(115200);
  delay(1000);
  initializeWS2812B();
  Serial.printf("Testing %d LEDs on Pin %d\n", NUM_LEDS, LED_PIN);

  // Note: Since this pin is shared with the DAC LDAC in your hardware file, 
  // you must ensure the DAC address configuration isn't conflicting right now.
 
}

void loop() {
  Serial.println("Turning LEDs Red...");
  fill_solid(leds, NUM_LEDS, CRGB::Yellow4);
  FastLED.show();
  delay(1000);

//   Serial.println("Turning LEDs Green...");
//   fill_solid(leds, NUM_LEDS, CRGB::Green);
//   FastLED.show();
//   delay(1000);

//   Serial.println("Turning LEDs Blue...");
//   fill_solid(leds, NUM_LEDS, CRGB::Blue);
//   FastLED.show();
//   delay(1000);

//   Serial.println("Chasing sequence...");
//   FastLED.clear();
//   for(int i = 0; i < NUM_LEDS; i++) {
//     leds[i] = CRGB::White;
//     FastLED.show();
//     delay(100);
//     leds[i] = CRGB::Black;
//   }
//   delay(500);
}




