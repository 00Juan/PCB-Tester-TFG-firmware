#include <Arduino.h>
/**
 * Basic ADC reading example.
 * - connects to ADC
 * - reads value from channel
 * - converts value to analog voltage
 */

#include <SPI.h>
#include <Mcp320x.h>
#include "hardwareIOSetup.h"

#define SPI_CS1    	7 		   // SPI slave select 1
#define SPI_CS2    	6 		   // SPI slave select 2
#define ADC_VREF    3300     // 3.3V Vref
#define ADC_CLK     1600000  // SPI clock 1.6MHz


MCP3208 adc1(ADC_VREF, SPI_CS1);
MCP3208 adc2(ADC_VREF, SPI_CS2);

void setup() {

  // configure PIN mode
  pinMode(SPI_CS1, OUTPUT);
  pinMode(SPI_CS2, OUTPUT);

  // set initial PIN state
  digitalWrite(SPI_CS1, HIGH);
  digitalWrite(SPI_CS2, HIGH);

  // initialize serial
  Serial.begin(115200);

  // initialize SPI interface for MCP3208
  SPISettings settings(ADC_CLK, MSBFIRST, SPI_MODE0);
  SPI.begin();
  SPI.beginTransaction(settings);
}

void loop() {
  readAndShowAll16Voltages();
  delay(2000);
}