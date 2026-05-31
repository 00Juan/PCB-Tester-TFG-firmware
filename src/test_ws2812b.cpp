#include <Arduino.h>
#include <FastLED.h>
#include "hardwareIOSetup.h"

// Configuration for WS2812B LEDs
#define NUM_LEDS      11
// We are using the pin dedicated for LED and DAC control
#define LED_PIN       pinLedsLdac
#define BRIGHTNESS    255
#define LED_TYPE      WS2812B
#define COLOR_ORDER   GRB

CRGB leds[NUM_LEDS];

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== WS2812B LED Test Booting ===");
  Serial.printf("Testing %d LEDs on Pin %d\n", NUM_LEDS, LED_PIN);

  // Note: Since this pin is shared with the DAC LDAC in your hardware file, 
  // you must ensure the DAC address configuration isn't conflicting right now.
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
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




