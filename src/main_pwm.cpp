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
// ShiftRegister74HC595<2> sr(35, 38, 37);

//ADC
// #define SPI_CS1    	7 		   // SPI slave select
// #define SPI_CS2    	6 		   // SPI slave select
// #define ADC_VREF    3395     // 3.3V Vref
#define ADC_CLK     1600000  // SPI clock 1.6MHz
// MCP3208 adc1(ADC_VREF, SPI_CS1);
// MCP3208 adc2(ADC_VREF, SPI_CS2);
const float adc_gain= 6.11915;
const float opamp_gain= 4.92;
float a;

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

uint16_t convertVtoDigitalDAC(float volts) {
    float aux = (3.385*opamp_gain-volts)*1/(opamp_gain-1);
    a=aux;
    return(uint16_t(aux*4095.0/4.962));
}

//DAC
// Adafruit_MCP4728 DAC1,DAC2;

 
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
  initializeMCP4728();

  
      DAC1.setChannelValue(MCP4728_CHANNEL_A, convertVtoDigitalDAC(13));
      DAC1.setChannelValue(MCP4728_CHANNEL_B, convertVtoDigitalDAC(0));
      DAC1.setChannelValue(MCP4728_CHANNEL_C, convertVtoDigitalDAC(0));
      DAC1.setChannelValue(MCP4728_CHANNEL_D, convertVtoDigitalDAC(0));
      DAC2.setChannelValue(MCP4728_CHANNEL_A, convertVtoDigitalDAC(0));
      DAC2.setChannelValue(MCP4728_CHANNEL_B, convertVtoDigitalDAC(0));
      DAC2.setChannelValue(MCP4728_CHANNEL_C, convertVtoDigitalDAC(0));
      DAC2.setChannelValue(MCP4728_CHANNEL_D, convertVtoDigitalDAC(0));


      //CHANNEL CONTROL
      pinMode(pinCtrl1,OUTPUT);
      pinMode(pinCtrl2,OUTPUT);
      pinMode(pinCtrl3,OUTPUT);
      pinMode(pinCtrl4,OUTPUT);

      //digitalWrite(pinCtrl1,HIGH);
      digitalWrite(pinCtrl2,HIGH);
      digitalWrite(pinCtrl3,HIGH);
      digitalWrite(pinCtrl4,HIGH);

      analogWriteResolution(8);
      analogWriteFrequency(1000);



}
 
void loop() {
  Serial.println(a);
//readAndShowAll16Voltages();
 analogWrite(14, 128);
}



