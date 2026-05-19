// /*********
//   Rui Santos
//   Complete project details at https://randomnerdtutorials.com  
// *********/
#include <Arduino.h>
#include "hardwareIOSetup.h"
#include <ShiftRegister74HC595.h>
#include <SPI.h>
#include <Mcp320x.h>
#include <Adafruit_MCP4728.h>
#include <Wire.h>

//SSR
ShiftRegister74HC595<2> sr(35, 38, 37);

//ADC
#define SPI_CS1    	7 		   // SPI slave select
#define SPI_CS2    	6 		   // SPI slave select
#define ADC_VREF    3395     // 3.3V Vref
#define ADC_CLK     1600000  // SPI clock 1.6MHz
MCP3208 adc1(ADC_VREF, SPI_CS1);
MCP3208 adc2(ADC_VREF, SPI_CS2);
const float adc_gain= 6.11915;

const MCP3208::Channel mcp3208_channels[8] = {
  MCP3208::Channel::SINGLE_0, MCP3208::Channel::SINGLE_1,
  MCP3208::Channel::SINGLE_2, MCP3208::Channel::SINGLE_3,
  MCP3208::Channel::SINGLE_4, MCP3208::Channel::SINGLE_5,
  MCP3208::Channel::SINGLE_6, MCP3208::Channel::SINGLE_7
};

/**
 * @brief Converts the analog voltage (mV) from an ACS725LLCTR-10AB-T sensor to Current (A).
 * 
 * Sensor characteristics (at Vcc = 3.3V):
 * - ACS725-10AB is bidirectional (-10A to +10A)
 * - Zero current voltage offset (V_IOUT(Q)): ~ Vcc / 2 = 1.65V (1650 mV)
 * - Sensitivity for 10AB: 264 mV/A 
 * 
 * @param millivolts The analog voltage read from the MCP3208 (using adc.toAnalog(raw))
 * @return float Current value in Amperes
 */
float convertMvToCurrentACS725(uint16_t millivolts) {
    const float V_OFFSET = 2050.0; // 1.65V in mV
    //const float SENSITIVITY = 132.0; 
    const float SENSITIVITY = 162.96; 
    // Calculate current: I = (V_read - V_offset) / Sensitivity
    return (millivolts - V_OFFSET) / SENSITIVITY;
}

void readAndShowAll16Voltages() {
uint32_t raw=0;
float val;
  /*
  Serial.println("\n=== Reading All 16 Channels ===");
  
  Serial.println("CHANNELS 1-8 (CS1=7):");
  for (int i = 0; i < 8; i++) {
    uint16_t raw = adc1.read(mcp3208_channels[i]);
    float val = float(adc1.toAnalog(raw))*adc_gain/1000;
    Serial.printf("  CH%d: %2f V (raw: %4d)\n", i+1, val, raw);
  }
*/

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


Serial.println("SUPPLY");

raw = adc2.read( MCP3208::Channel::SINGLE_7);
val = float(adc2.toAnalog(raw))*adc_gain/1000;
float voltSupply=val;
Serial.printf("Supply: %2f V (raw: %4d)\n", val, raw);
  raw=0;
 for(int i=0;i<100;i++)
 {
  raw += adc2.read(MCP3208::Channel::SINGLE_6);
  delay(1);
 }
val = convertMvToCurrentACS725(raw/100);
Serial.printf(" Isupply: %2f\n", val);
Serial.printf(" POWER: %2f\n", voltSupply*val);



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






//DAC
Adafruit_MCP4728 mcp1,mcp2;

 
void setup() {

 Serial.begin(115200);

 //SSR
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe,LOW);
    delay(1000);
    sr.setAllHigh(); // set all pins HIGH

  //ADC
  pinMode(SPI_CS1, OUTPUT);
  pinMode(SPI_CS2, OUTPUT);
  digitalWrite(SPI_CS1, HIGH);
  digitalWrite(SPI_CS2, HIGH);

  // initialize SPI interface for MCP3208
  SPISettings settings(ADC_CLK, MSBFIRST, SPI_MODE0);
  SPI.begin();
  SPI.beginTransaction(settings);

  //DAC
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
  mcp1.setChannelValue(MCP4728_CHANNEL_A, 1000);
      mcp1.setChannelValue(MCP4728_CHANNEL_B, 1000);
      mcp1.setChannelValue(MCP4728_CHANNEL_C, 1000);
      mcp1.setChannelValue(MCP4728_CHANNEL_D, 1000);
      mcp2.setChannelValue(MCP4728_CHANNEL_A, 1000);
      mcp2.setChannelValue(MCP4728_CHANNEL_B, 1000);
      mcp2.setChannelValue(MCP4728_CHANNEL_C, 1000);
      mcp2.setChannelValue(MCP4728_CHANNEL_D, 1000);


      //CHANNEL CONTROL
      pinMode(pinCtrl1,OUTPUT);
      pinMode(pinCtrl2,OUTPUT);
      pinMode(pinCtrl3,OUTPUT);
      pinMode(pinCtrl4,OUTPUT);

      digitalWrite(pinCtrl1,HIGH);
      digitalWrite(pinCtrl2,HIGH);
      digitalWrite(pinCtrl3,HIGH);
      digitalWrite(pinCtrl4,HIGH);


}
 
void loop() {
readAndShowAll16Voltages();
}



