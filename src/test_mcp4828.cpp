#include <Arduino.h>
// Basic demo for configuring the MCP4728 4-Channel 12-bit I2C DAC
#include <Adafruit_MCP4728.h>
#include <Wire.h>
#include "hardwareIOSetup.h"



void setup(void) {
  Serial.begin(115200);
  while (!Serial)
    delay(10); // will pause Zero, Leonardo, etc until serial console opens

  Serial.println("Adafruit MCP4728 test!");

  // Try to initialize!
  
  initializeMCP4728();

      DAC1.setChannelValue(MCP4728_CHANNEL_A, 4095);
      DAC1.setChannelValue(MCP4728_CHANNEL_B, 2048);
      DAC1.setChannelValue(MCP4728_CHANNEL_C, 1024);
      DAC1.setChannelValue(MCP4728_CHANNEL_D, 0);
      DAC2.setChannelValue(MCP4728_CHANNEL_A, 3095);
      DAC2.setChannelValue(MCP4728_CHANNEL_B, 3048);
      DAC2.setChannelValue(MCP4728_CHANNEL_C, 3024);
      DAC2.setChannelValue(MCP4728_CHANNEL_D, 3000);
}

void loop() { 
    Serial.println("OK");
    delay(1000); 
  Serial.println(  DAC1.getChannelValue(MCP4728_CHANNEL_A));
  Serial.println(  DAC1.getChannelValue(MCP4728_CHANNEL_B));
  Serial.println(  DAC1.getChannelValue(MCP4728_CHANNEL_C));
  Serial.println(  DAC1.getChannelValue(MCP4728_CHANNEL_D));
  Serial.println(  DAC2.getChannelValue(MCP4728_CHANNEL_A));
  Serial.println(  DAC2.getChannelValue(MCP4728_CHANNEL_B));
  Serial.println(  DAC2.getChannelValue(MCP4728_CHANNEL_C));
  Serial.println(  DAC2.getChannelValue(MCP4728_CHANNEL_D));

  
  }