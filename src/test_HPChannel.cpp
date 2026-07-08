#include <Arduino.h>
#include "hardwareIOSetup.h"

// Note: Ensure your platformio.ini has src_filter configured to build this file and ignore main.cpp if you want to run this test

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("Starting HPCH Test...");

    // Initialize core hardware
    Wire.begin(pinI2cSda, pinI2cScl);
    SPI.begin(pinSpiClk, pinSpiMiso, pinSpiMosi);

    // Initialize MCP3208
    initializeMCP3208();

    // Initialize Shift Register
    sr.setAllLow();

    // Initialize LEDs
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear();
    FastLED.show();

    // Initialize HP Channels
    for (int i = 0; i < 2; i++) {
        hpChannels[i].init();
        Serial.print("Calibrating ACS725 for HPCH ");
        Serial.println(i + 1);
        hpChannels[i].calibrateACS725();
        
        // Let's connect channel 1 for testing, and keep channel 2 disconnected
        if (i == 0) {
            hpChannels[i].connect();
        }
    }
    
    Serial.println("Setup complete.");
}

void loop() {
    for (int i = 0; i < 2; i++) {
        hpChannels[i].update();
        hpChannels[i].printDebugInfo();
    }
    
    // Toggle Channel 1 connection state every 5 seconds
    static uint32_t lastToggle = 0;
    if (millis() - lastToggle > 5000) {
        lastToggle = millis();
        if (hpChannels[0].getStatus() == HPCH_STATUS_NORMAL) {
            static bool isConnected = true;
            isConnected = !isConnected;
            if (isConnected) {
                hpChannels[0].connect();
                Serial.println("HPCH 1 CONNECTED");
            } else {
                hpChannels[0].disconnect();
                Serial.println("HPCH 1 DISCONNECTED");
            }
        }
    }

    delay(1000); // Wait 1 second between prints
}
