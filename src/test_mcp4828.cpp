#include <Arduino.h>
// Basic demo for configuring the MCP4728 4-Channel 12-bit I2C DAC
#include <Adafruit_MCP4728.h>
#include <Wire.h>

Adafruit_MCP4728 mcp1,mcp2;

void setup(void) {
  Serial.begin(115200);
  while (!Serial)
    delay(10); // will pause Zero, Leonardo, etc until serial console opens

  Serial.println("Adafruit MCP4728 test!");

  // Try to initialize!
  if (!mcp1.begin(0x61)) {
    Serial.println("Failed to find MCP4728 1 chip");
    while (1) {
      delay(10);
    }
  }

   // Try to initialize!
  if (!mcp2.begin(0x60)) {
    Serial.println("Failed to find MCP4728 2 chip");
    while (1) {
      delay(10);
    }
  }

      mcp1.setChannelValue(MCP4728_CHANNEL_A, 4095);
      mcp1.setChannelValue(MCP4728_CHANNEL_B, 2048);
      mcp1.setChannelValue(MCP4728_CHANNEL_C, 1024);
      mcp1.setChannelValue(MCP4728_CHANNEL_D, 0);
      mcp2.setChannelValue(MCP4728_CHANNEL_A, 3095);
      mcp2.setChannelValue(MCP4728_CHANNEL_B, 3048);
      mcp2.setChannelValue(MCP4728_CHANNEL_C, 3024);
      mcp2.setChannelValue(MCP4728_CHANNEL_D, 3000);
}

void loop() { 
    Serial.println("OK");
    delay(1000); 
  Serial.println(  mcp1.getChannelValue(MCP4728_CHANNEL_A));
  Serial.println(  mcp1.getChannelValue(MCP4728_CHANNEL_B));
  Serial.println(  mcp1.getChannelValue(MCP4728_CHANNEL_C));
  Serial.println(  mcp1.getChannelValue(MCP4728_CHANNEL_D));
  Serial.println(  mcp2.getChannelValue(MCP4728_CHANNEL_A));
  Serial.println(  mcp2.getChannelValue(MCP4728_CHANNEL_B));
  Serial.println(  mcp2.getChannelValue(MCP4728_CHANNEL_C));
  Serial.println(  mcp2.getChannelValue(MCP4728_CHANNEL_D));

  
  }