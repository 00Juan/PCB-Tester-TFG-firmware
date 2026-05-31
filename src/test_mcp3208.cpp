#include <Arduino.h>
/**
 * MCP3208 Testing Suite
 * - Detection mode: Scans SPI bus to verify MCP3208 presence
 * - Reading mode: Reads and displays ADC values from channels
 */

#include <SPI.h>
#include <Mcp320x.h>
#include "hardwareIOSetup.h"



// Test mode selection
enum TestMode {
  MODE_DETECT = 0,   // Scan SPI bus for MCP3208 presence
  MODE_READ = 1      // Read ADC values from both MCP3208s
};

// Global variables
const MCP3208::Channel mcp3208_channels[8] = {
  MCP3208::Channel::SINGLE_0, MCP3208::Channel::SINGLE_1,
  MCP3208::Channel::SINGLE_2, MCP3208::Channel::SINGLE_3,
  MCP3208::Channel::SINGLE_4, MCP3208::Channel::SINGLE_5,
  MCP3208::Channel::SINGLE_6, MCP3208::Channel::SINGLE_7
};

const float adc_gain= 6.11915;

void readAndShowAll16Voltages();




void setup() {

  Serial.begin(115200);
  initializeMCP3208();
  // initialize serial
  
  // Wait for Serial port to be ready
  while (!Serial) {
    delay(100);
  }

}

void loop() {
  
    // Reading mode: read ADC values
    Serial.println("\n--- ADC Reading ---");
    readAndShowAll16Voltages();
    delay(2000);
  
}



void readAndShowAll16Voltages() {
uint32_t raw=0;
float val;
  
  Serial.println("\n=== Reading All 16 Channels ===");
  
  Serial.println("CHANNELS 1-8 (CS1=7):");
  for (int i = 0; i < 8; i++) {
    uint16_t raw = adc1.read(mcp3208_channels[i]);
    float val = float(adc1.toAnalog(raw))*adc_gain/1000;
    Serial.printf("  CH%d: %2f V (raw: %4d)\n", i+1, val, raw);
  }


// Serial.println("CHANNELS 9-10 (CS1=7):");

// Serial.println("VCH9:");
// for (int i = 0; i < 2; i++) {
//     raw = adc2.read(mcp3208_channels[i]);
//     val = float(adc2.toAnalog(raw))*adc_gain/1000;
//     Serial.printf("  CH%d: %2f V (raw: %4d)\n", i+1, val, raw);
//   }  
// Serial.println("ICH9:");
// raw = adc2.read(MCP3208::Channel::SINGLE_2);
//  Serial.printf(" ICH9 raw: %4d mV\n", raw);

//  raw=0;
//  for(int i=0;i<100;i++)
//  {
//   raw += adc2.read(MCP3208::Channel::SINGLE_2);
//   delay(1);
//  }

// val = convertMvToCurrentACS725(raw/100);
// Serial.printf(" ICH9: %2f\n", val);


// Serial.println("VCH10:");
// raw = adc2.read( MCP3208::Channel::SINGLE_3);
//  val = float(adc2.toAnalog(raw))*adc_gain/1000;
// Serial.printf("  CH%d: %2f V (raw: %4d)\n", 10, val, raw);

// Serial.println("ICH10:");
// raw = adc2.read(MCP3208::Channel::SINGLE_4);
//  Serial.printf(" ICH10 raw: %4d mV\n", raw);

//   raw=0;
//  for(int i=0;i<100;i++)
//  {
//   raw += adc2.read(MCP3208::Channel::SINGLE_4);
//   delay(1);
//  }
// val = convertMvToCurrentACS725(raw/100);
// Serial.printf(" ICH10: %2f\n", val);


// Serial.println("SUPPLY");

// raw = adc2.read( MCP3208::Channel::SINGLE_7);
// val = float(adc2.toAnalog(raw))*adc_gain/1000;
// float voltSupply=val;
// Serial.printf("Supply: %2f V (raw: %4d)\n", val, raw);
//   raw=0;
//  for(int i=0;i<100;i++)
//  {
//   raw += adc2.read(MCP3208::Channel::SINGLE_6);
//   delay(1);
//  }
// val = convertMvToCurrentACS725(raw/100);
// Serial.printf(" Isupply: %2f\n", val);
// Serial.printf(" POWER: %2f\n", voltSupply*val);



delay(200);


// Serial.println("CHANNEL 11 HV (CS1=7):");
// uint16_t raw = adc2.read( MCP3208::Channel::SINGLE_5);
// float val = float(adc2.toAnalog(raw))*305.29/1000;
// Serial.printf("  CH%d: %2f V (raw: %4d)\n", 11, val, raw);
// delay(100);


  // Serial.println("ADC2 (CS2=6):");
  // for (int i = 0; i < 8; i++) {
  //   uint16_t raw = adc2.read(mcp3208_channels[i]);
  //   float val = float(adc2.toAnalog(raw))*adc_gain/1000;
  //   Serial.printf("  CH%d: %2f V (raw: %4d)\n", i + 8 +1, val, raw);
  // }
  Serial.println("===============================");
}