#ifndef HARDWARE_IO_SETUP_H
#define HARDWARE_IO_SETUP_H

#include "SoftI2cMaster.h"
#include <Adafruit_MCP4728.h>
#include <Arduino.h>
#include <ESP32RotaryEncoder.h>
#include <FastLED.h>
#include <Mcp320x.h>
#include <SPI.h>
#include <ShiftRegister74HC595.h>
#include <U8g2lib.h>
#include <Wire.h>

// ============================================================================
// PIN DEFINITIONS - ESP32 GPIO Configuration
// ============================================================================

// ============================================================================
// GPIO PIN DEFINITIONS - ESP32-S3 BMS Signal List
// ============================================================================

// LED and DAC Control
const uint8_t pinLedsLdac = 1;

// Audio
const uint8_t pinBuzzer = 2;

// CAN Communication
const uint8_t pinCanRx = 4;
const uint8_t pinCanTx = 5;

// SPI ADC Chip Selects
const uint8_t pinSpiCsAdc2 = 6;
const uint8_t pinSpiCsAdc1 = 7;

// I2C Communication
const uint8_t pinI2cSda = 8;
const uint8_t pinI2cScl = 9;

// Control Signals
const uint8_t pinCtrl4 = 10;
const uint8_t pinCtrl1 = 14;
const uint8_t pinCtrl2 = 15;
const uint8_t pinCtrl3 = 16;

// SPI Bus (Main communication)
const uint8_t pinSpiMosi = 11;
const uint8_t pinSpiClk = 12;
const uint8_t pinSpiMiso = 13;

// Encoder Inputs
const uint8_t pinEncoderD1 = 17;
const uint8_t pinEncoderD2 = 18;
const uint8_t pinEncoderSw = 21;

// Shift Register Control
const uint8_t pinSrSer = 35;   // Serial data input
const uint8_t pinSrOe = 36;    // Output enable
const uint8_t pinSrRclk = 37;  // Register clock
const uint8_t pinSrSrclk = 38; // Shift register clock

// I2C addresses
const uint16_t i2cAddrMCP4728_1 = 0x81;
const uint16_t i2cAddrMCP4728_2 = 0x80;
const uint16_t i2cAddrSSD1309 = 0x3C;

// ============================================================================
// OBJECTS
// ============================================================================
ShiftRegister74HC595<2> sr(pinSrSer, pinSrSrclk, pinSrRclk);

RotaryEncoder rotaryEncoder(pinEncoderD1, pinEncoderD2, -1, -1);

#define ADC_VREF 3270   // 3.3V Vref
#define ADC_CLK 1600000 // SPI clock 1.6MHz
MCP3208 adc1(ADC_VREF, pinSpiCsAdc1);
MCP3208 adc2(ADC_VREF, pinSpiCsAdc2);

Adafruit_MCP4728 DAC1, DAC2;

U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// Configuration for WS2812B LEDs
#define NUM_LEDS 11
// We are using the pin dedicated for LED and DAC control
#define LED_PIN pinLedsLdac
#define BRIGHTNESS 20
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];

#include "LVLPChannel.h"

ChannelCalibrationData chCalData[8] = {
    // K1,   K2,  offset,   mAdc,    bAdc, mDAC, bDAC
    {4.922409058f, -3.926402569f, 0.217268860f, 0.005097771f, -0.030085115f,
     819.581665039f, -2.384184361f}, // CH1 //estaba a -1.503215434
    { 4.922409058f, -3.920775175f, 0.262085301f, 0.005094388f, -0.027798980f, 819.581665039f, -2.384184361f }, // CH2
    { 4.922409058f, -3.956279993f, 0.276542324f, 0.005100798f, -0.029126322f, 819.581665039f, -2.384184361f }, // CH3
    { 4.922409058f, -3.914973021f, 0.268908657f, 0.005089805f, -0.027109882f, 819.581665039f, -2.384184361f }, // CH4
    { 4.922409058f, -3.942269087f, 0.754644084f, 0.005104496f, -0.027869666f, 819.581665039f, -2.384184361f }, // CH5
    { 4.922409058f, -3.938249350f, 0.746614146f, 0.005100900f, -0.029309209f, 819.581665039f, -2.384184361f }, // CH6
    { 4.922409058f, -3.926738501f, 0.729560542f, 0.005166044f, -0.028693369f, 819.581665039f, -2.384184361f }, // CH7
    { 4.922409058f, -3.907069683f, 0.725888438f, 0.005104249f, -0.028883915f, 819.581665039f, -2.384184361f }, // CH8
};

// LVLP Channels configuration
LVLPChannel lvlpChannels[8] = {
    // CH1: DAC1_A, ADC1_0, SR_0, PWM Ctrl1 (pin 14)
    LVLPChannel(1, &adc1, MCP3208::Channel::SINGLE_0, &DAC1, MCP4728_CHANNEL_A,
                &sr, 0, pinCtrl1, chCalData[0], &leds[0]),
    // CH2: DAC1_B, ADC1_1, SR_1, PWM Ctrl2 (pin 15)
    LVLPChannel(2, &adc1, MCP3208::Channel::SINGLE_1, &DAC1, MCP4728_CHANNEL_B,
                &sr, 1, pinCtrl2, chCalData[1], &leds[1]),
    // CH3: DAC1_C, ADC1_2, SR_2, PWM Ctrl3 (pin 16)
    LVLPChannel(3, &adc1, MCP3208::Channel::SINGLE_2, &DAC1, MCP4728_CHANNEL_C,
                &sr, 2, pinCtrl3, chCalData[2], &leds[2]),
    // CH4: DAC1_D, ADC1_3, SR_3, PWM Ctrl4 (pin 10)
    LVLPChannel(4, &adc1, MCP3208::Channel::SINGLE_3, &DAC1, MCP4728_CHANNEL_D,
                &sr, 3, pinCtrl4, chCalData[3], &leds[3]),
    // CH5: DAC2_A, ADC1_4, SR_4, No PWM
    LVLPChannel(5, &adc1, MCP3208::Channel::SINGLE_4, &DAC2, MCP4728_CHANNEL_A,
                &sr, 4, -1, chCalData[4], &leds[4]),
    // CH6: DAC2_B, ADC1_5, SR_5, No PWM
    LVLPChannel(6, &adc1, MCP3208::Channel::SINGLE_5, &DAC2, MCP4728_CHANNEL_B,
                &sr, 5, -1, chCalData[5], &leds[5]),
    // CH7: DAC2_C, ADC1_6, SR_6, No PWM
    LVLPChannel(7, &adc1, MCP3208::Channel::SINGLE_6, &DAC2, MCP4728_CHANNEL_C,
                &sr, 6, -1, chCalData[6], &leds[6]),
    // CH8: DAC2_D, ADC1_7, SR_7, No PWM
    LVLPChannel(8, &adc1, MCP3208::Channel::SINGLE_7, &DAC2, MCP4728_CHANNEL_D, &sr, 7, -1, chCalData[7],&leds[7])
};

#include "HPCH.h"

// Default calibration data for HPCH (assuming 5k1/1k voltage divider -> 6.1 ratio)
HPCHCalibrationData hpCalData[2] = {
    // mADC_VIn, bADC_VIn, mADC_VOut, bADC_VOut, sensitivity, vref
    { 0.005075289f, 0.015266559f, 0.005192232f, 0.008624060f, 0.132556796f, 3.269999981f }, // HPCH1, // HPCH 1
    { 0.005094961f, 0.466436088f, 0.005202507f, 0.007445905f, 0.131758332f, 3.269999981f }  // HPCH 2
};



// HPCH Channels configuration
HPCH hpChannels[2] = {
    // HPCH1: ADC2_0 (VIn), ADC2_1 (VOut), ADC2_2 (Current), SR_8, LED_8
    HPCH(1, &adc2, MCP3208::Channel::SINGLE_0, MCP3208::Channel::SINGLE_1, MCP3208::Channel::SINGLE_2, &sr, 8, hpCalData[0], &leds[8]),
    
    // HPCH2: ADC2_3 (VIn), ADC2_4 (VOut), ADC2_5 (Current), SR_9, LED_9
    HPCH(2, &adc2, MCP3208::Channel::SINGLE_3, MCP3208::Channel::SINGLE_7, MCP3208::Channel::SINGLE_4, &sr, 9, hpCalData[1], &leds[9])
};

#include "HVChannel.h"

// ============================================================================
// HV Channel configuration
// NOTE: Adjust ADC channel, SR pin, and LED index to match your hardware.
//
// Default calibration is identity (no real points) — run the calibration
// routine (test_HVChannel environment) to populate real values and paste
// the output back here.
// ============================================================================
HVChannelCalibrationData hvCalData = {
    /* deadZoneRaw */ 100,
    /* points[] */ {
        {  100, 25.229999542f },  // [0] 25.2300 V
        {  118, 30.159999847f },  // [1] 30.1600 V
        {  176, 44.369998932f },  // [2] 44.3700 V
        {  243, 61.130001068f },  // [3] 61.1300 V
        {    0, 0.0f },             // [4] unused
        {    0, 0.0f },             // [5] unused
        {    0, 0.0f },             // [6] unused
        {    0, 0.0f },             // [7] unused
        {    0, 0.0f },             // [8] unused
        {    0, 0.0f },             // [9] unused
        {    0, 0.0f },             // [10] unused
        {    0, 0.0f },             // [11] unused
        {    0, 0.0f },             // [12] unused
        {    0, 0.0f },             // [13] unused
        {    0, 0.0f },             // [14] unused
        {    0, 0.0f },             // [15] unused
    },
     4,
    3.269999981f
};

// HVChannel instance
// HVCh1: ADC2_5 (voltage sense), SR_10, LED_10
// Adjust the ADC channel and SR/LED pin indices to match your PCB layout.
HVChannel hvChannels[1] = {
  HVChannel(1, &adc2, MCP3208::Channel::SINGLE_5, &sr, 10, hvCalData, &leds[10])
};


// ============================================================================
// CONFIGURATION
// ============================================================================

bool initializeEncoder(long minValue, long maxValue, bool circleValues) {
  // FLOATING tells the library to use INPUT_PULLUP internally for A/B pins
  rotaryEncoder.setEncoderType(EncoderType::FLOATING);
  rotaryEncoder.setBoundaries(minValue, maxValue, circleValues);
  // rotaryEncoder.onTurned( &knobCallback );

  rotaryEncoder.begin(true);
  // Setup the button pin manually with pull-up
  pinMode(pinEncoderSw, INPUT_PULLUP);
  return true;
}

bool initializeMCP3208() {
  // configure PIN mode
  pinMode(pinSpiCsAdc1, OUTPUT);
  pinMode(pinSpiCsAdc2, OUTPUT);

  // set initial PIN state
  digitalWrite(pinSpiCsAdc1, HIGH);
  digitalWrite(pinSpiCsAdc2, HIGH);

  SPISettings settings(ADC_CLK, MSBFIRST, SPI_MODE0);
  SPI.begin();
  SPI.beginTransaction(settings);

  Serial.println("\n=== MCP3208 ADC Scan ===");
  Serial.println("Scanning for MCP3208 devices on SPI bus...\n");

  uint8_t deviceCount = 0;

  // Array of CS pins to scan
  const uint8_t csPins[] = {pinSpiCsAdc1, pinSpiCsAdc2};
  const char *csNames[] = {"ADC1", "ADC2"};
  const uint8_t numCsPins = sizeof(csPins) / sizeof(csPins[0]);

  for (uint8_t i = 0; i < numCsPins; i++) {
    uint8_t csPin = csPins[i];

    Serial.printf("\n--- Testing MCP3208 at CS Pin %d (%s) ---\n", csPin,
                  csNames[i]);

    // Initialize CS pin as output
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    delay(10);

    // MCP3208 channel 0, single-ended read
    // Command byte: Start bit (1) + Single-ended (1) + Don't care (0) + Channel
    // selection (3 bits) For channel 0: 0x06 (binary: 0000 0110)
    uint8_t commandByte = 0x06; // Start + single-ended for channel 0

    // Perform MCP3208 read sequence
    // Transaction 1: Send read command for channel 0
    digitalWrite(csPin, LOW);
    delayMicroseconds(10);

    uint8_t nullByte = SPI.transfer(commandByte); // Send command, receive null
    uint8_t dataHigh =
        SPI.transfer(0x00); // Receive high byte (12-bit value MSB)
    uint8_t dataLow = SPI.transfer(0x00); // Receive low byte (12-bit value LSB)

    digitalWrite(csPin, HIGH);
    delayMicroseconds(10);

    // Combine the 12-bit value (dataHigh[1:0] + dataLow[7:0])
    uint16_t adcValue1 = ((dataHigh & 0x0F) << 8) | dataLow;

    // Perform second read for consistency verification
    delay(2);
    digitalWrite(csPin, LOW);
    delayMicroseconds(10);

    nullByte = SPI.transfer(commandByte);
    dataHigh = SPI.transfer(0x00);
    dataLow = SPI.transfer(0x00);

    digitalWrite(csPin, HIGH);
    delayMicroseconds(10);

    uint16_t adcValue2 = ((dataHigh & 0x0F) << 8) | dataLow;

    // Display MCP3208 read data
    Serial.printf("First Read:  ADC Value = 0x%03X (%4d / 4095)\n", adcValue1,
                  adcValue1);
    Serial.printf("Second Read: ADC Value = 0x%03X (%4d / 4095)\n", adcValue2,
                  adcValue2);

    // Check for valid MCP3208 response characteristics
    bool validRange1 = (adcValue1 <= 0xFFF); // 12-bit max
    bool validRange2 = (adcValue2 <= 0xFFF);

    Serial.printf("Valid Range: %s, %s\n", validRange1 ? "✓" : "✗",
                  validRange2 ? "✓" : "✗");

    // Check if values are in reasonable range for MCP3208
    // Typically connected to a reference, not floating high or low
    bool notFloating = !(adcValue1 == 0xFFF && adcValue2 == 0xFFF) &&
                       !(adcValue1 == 0x000 && adcValue2 == 0x000);

    if (notFloating) {
      Serial.printf("Status: ✓ Channel responding with analog data\n");
    } else if (adcValue1 == 0xFFF) {
      Serial.printf(
          "Status: ⚠ Channel floating high (no connection or Vref issue)\n");
    } else if (adcValue1 == 0x000) {
      Serial.printf("Status: ⚠ Channel stuck at zero\n");
    }

    // Determine if MCP3208 is detected
    bool mcp3208Detected = validRange1 && validRange2 && notFloating;

    if (mcp3208Detected) {
      deviceCount++;
      Serial.printf(
          "Result: ✓ MCP3208 detected and responding at CS Pin %d (%s)\n",
          csPin, csNames[i]);
    } else if (validRange1 && validRange2) {
      Serial.printf("Result: ⚠ Possible MCP3208 with floating input or "
                    "reference issue\n");
    } else {
      Serial.printf("Result: ✗ No MCP3208 detected or communication error\n");
    }
  }

  Serial.println("\n--- Scan Summary ---");
  if (deviceCount == 0) {
    Serial.println("No MCP3208 devices found!");
  } else {
    Serial.printf("Total MCP3208 devices found: %d\n", deviceCount);
  }

  Serial.println("=== MCP3208 Scan Complete ===\n");
  // return (deviceCount==2);
  return true;
}

bool initializeMCP4728() {
  bool ok = true;
  if (!DAC1.begin(0x61)) {
    Serial.println("Failed to find DAC 1 chip");
    ok = false;
  } else {
    Serial.println("DAC 1 OK");
  }

  if (!DAC2.begin(0x60)) {
    Serial.println("Failed to find DAC 2 chip");
    ok = false;
  } else {
    Serial.println("DAC 2 OK");
  }
  return ok;
}

bool initializeSSD1309() {
  bool ok = true;
  // Initialize the specific I2C pins using the Wire library before u8g2 starts
  Wire.begin(pinI2cSda, pinI2cScl);

  // Initialize the display
  if (!u8g2.begin()) {
    Serial.println("u8g2 initialization failed!");
    ok = false;
  } else {
    Serial.println("u8g2 initialized successfully.");
  }
  return ok;
}

bool initializeWS2812B() {
  Serial.println("\n=== WS2812B LED Test Booting ===");
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
      .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  return true;
}

/**
 * @brief Initialize all configured hardware pins
 */
void initializeHardwarePins() {}

// ============================================================================
// I2C BUS FUNCTIONS
// ============================================================================

/**
 * @brief Initialize the I2C bus with specified clock speed
 *
 * @param clockSpeed I2C clock frequency in Hz (default: 100000 Hz = 100 kHz)
 * @return true if initialization was successful, false otherwise
 */
bool initializeI2C(uint32_t clockSpeed = 100000) {
  Serial.println("\n=== Initializing I2C Bus ===");
  Serial.printf("SDA Pin: %d, SCL Pin: %d\n", pinI2cSda, pinI2cScl);
  Serial.printf("Clock Speed: %d Hz\n", clockSpeed);

  // Begin I2C communication with the specified pins and clock speed
  Wire.begin(pinI2cSda, pinI2cScl, clockSpeed);

  Serial.println("I2C Bus initialized successfully!");
  return true;
}

/**
 * @brief Scan the I2C bus for available devices and print their addresses
 *
 * Performs a scan of the I2C bus and prints the addresses of all detected
 * slave devices in both hexadecimal and decimal format.
 *
 * @return Number of devices found on the I2C bus
 */
uint8_t scanI2CDevices() {
  Serial.println("\n=== I2C Device Scan ===");
  Serial.println("Scanning for I2C devices...\n");

  uint8_t deviceCount = 0;

  // I2C addresses range from 0x08 to 0x77 (7-bit addressing)
  // 0x00-0x07 are reserved, 0x78-0x7F are reserved
  for (uint8_t address = 0x08; address < 0x78; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("Device found at address: 0x%02X (decimal: %3d)\n", address,
                    address);
      deviceCount++;
    }
  }

  if (deviceCount == 0) {
    Serial.println("No I2C devices found!");
  } else {
    Serial.printf("\nTotal devices found: %d\n", deviceCount);
  }

  Serial.println("=== Scan Complete ===\n");
  return deviceCount;
}

/**
 * @brief Change MCP4728 I2C address permanently (stored in EEPROM)
 *
 * The MCP4728 DAC can permanently change its I2C address by writing to EEPROM.
 * The address bits (A2, A1, A0) are configured via I2C commands and persist
 * across power cycles.
 *
 * @param currentAddress Current I2C address of the MCP4728 (default: 0x60)
 * @param newAddressCode New address code (0-7). Final address will be 0x60 +
 * code
 * @return true if address change was successful, false otherwise
 */
bool changeMCP4728Address(uint8_t currentAddress, uint8_t newAddressCode) {

  pinMode(pinLedsLdac, OUTPUT);
  digitalWrite(pinLedsLdac, LOW);
  delayMicroseconds(100);
  digitalWrite(pinLedsLdac, HIGH);
  delay(50);

  if (newAddressCode > 7) {
    Serial.println("Error: Address code must be 0-7");
    return false;
  }

  uint8_t oldId = currentAddress & 0x07;
  uint8_t newId = newAddressCode;
  uint8_t newAddress = 0x60 + newId;

  Serial.printf("\n=== MCP4728 Permanent I2C Address Change ===\n");
  Serial.printf("Current Address: 0x%02X\n", currentAddress);
  Serial.printf("New Address: 0x%02X (code: %d)\n", newAddress, newAddressCode);
  Serial.println("Writing address to EEPROM (permanent)...");

  SoftI2cMaster i2c;
  i2c.init(pinI2cScl, pinI2cSda);
  pinMode(pinLedsLdac, OUTPUT);

  digitalWrite(pinLedsLdac, HIGH);
  i2c.start(0B11000000 | (oldId << 1));
  i2c.ldacwrite(0B01100001 | (oldId << 2),
                pinLedsLdac); // modified command for LDAC pin latch
  i2c.write(0B01100010 | (newId << 2));
  i2c.write(0B01100011 | (newId << 2));
  i2c.stop();

  Serial.println("EEPROM write command sent. Waiting for write to complete...");
  delay(100); // wait for EEPROM writing

  // Read to verify
  digitalWrite(pinLedsLdac, HIGH);
  i2c.start(0B00000000);
  i2c.ldacwrite(0B00001100, pinLedsLdac); // modified command for LDAC pin latch
  i2c.restart(0B11000001);
  uint8_t address = i2c.read(true);
  i2c.stop();

  int scanedAddress = (address & 0B00001110) >> 1;
  Serial.printf("Scanned Address = %d\n", scanedAddress);

  // Re-initialize hardware I2C after using Software I2C
  Wire.begin(pinI2cSda, pinI2cScl, 100000);

  if (scanedAddress == newId) {
    Serial.printf("✓ Success! MCP4728 now responds at address 0x%02X\n",
                  newAddress);
    return true;
  } else {
    Serial.printf("✗ Failed! MCP4728 address is still %d\n", scanedAddress);
    return false;
  }
}

// ============================================================================
// SPI BUS FUNCTIONS
// ============================================================================

/**
 * @brief Initialize the SPI bus with specified clock speed
 *
 * @param clockSpeed SPI clock frequency in Hz (default: 1000000 Hz = 1 MHz)
 * @return true if initialization was successful, false otherwise
 */
bool initializeSPI(uint32_t clockSpeed = 1000000) {
  Serial.println("\n=== Initializing SPI Bus ===");
  Serial.printf("MOSI Pin: %d, MISO Pin: %d, CLK Pin: %d\n", pinSpiMosi,
                pinSpiMiso, pinSpiClk);
  Serial.printf("Clock Speed: %d Hz\n", clockSpeed);

  // Initialize SPI with specified pins
  SPI.begin(pinSpiClk, pinSpiMiso, pinSpiMosi);

  Serial.println("SPI Bus initialized successfully!");
  return true;
}

/**
 * @brief Scan the SPI bus for MCP3208 ADC devices
 *
 * Specialized function to detect MCP3208 8-channel ADCs on the SPI bus.
 * The MCP3208 is a 12-bit ADC that communicates via SPI.
 * Tests both ADC chip select pins (ADC1 and ADC2) with MCP3208-specific
 * diagnostics.
 *
 * MCP3208 Communication Protocol:
 * - Requires 3 bytes per transaction: command byte, data byte 1, data byte 2
 * - Command format (byte 1): 0x06 (start bit + single-ended) | (channel << 6)
 * - Returns 12-bit value from the selected channel
 *
 * @return Number of MCP3208 devices found on the SPI bus
 */
uint8_t scanSPIDevices() {
  Serial.println("\n=== MCP3208 ADC Scan ===");
  Serial.println("Scanning for MCP3208 devices on SPI bus...\n");

  uint8_t deviceCount = 0;

  // Array of CS pins to scan
  const uint8_t csPins[] = {pinSpiCsAdc1, pinSpiCsAdc2};
  const char *csNames[] = {"ADC1", "ADC2"};
  const uint8_t numCsPins = sizeof(csPins) / sizeof(csPins[0]);

  for (uint8_t i = 0; i < numCsPins; i++) {
    uint8_t csPin = csPins[i];

    Serial.printf("\n--- Testing MCP3208 at CS Pin %d (%s) ---\n", csPin,
                  csNames[i]);

    // Initialize CS pin as output
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    delay(10);

    // MCP3208 channel 0, single-ended read
    // Command byte: Start bit (1) + Single-ended (1) + Don't care (0) + Channel
    // selection (3 bits) For channel 0: 0x06 (binary: 0000 0110)
    uint8_t commandByte = 0x06; // Start + single-ended for channel 0

    // Perform MCP3208 read sequence
    // Transaction 1: Send read command for channel 0
    digitalWrite(csPin, LOW);
    delayMicroseconds(10);

    uint8_t nullByte = SPI.transfer(commandByte); // Send command, receive null
    uint8_t dataHigh =
        SPI.transfer(0x00); // Receive high byte (12-bit value MSB)
    uint8_t dataLow = SPI.transfer(0x00); // Receive low byte (12-bit value LSB)

    digitalWrite(csPin, HIGH);
    delayMicroseconds(10);

    // Combine the 12-bit value (dataHigh[1:0] + dataLow[7:0])
    uint16_t adcValue1 = ((dataHigh & 0x0F) << 8) | dataLow;

    // Perform second read for consistency verification
    delay(2);
    digitalWrite(csPin, LOW);
    delayMicroseconds(10);

    nullByte = SPI.transfer(commandByte);
    dataHigh = SPI.transfer(0x00);
    dataLow = SPI.transfer(0x00);

    digitalWrite(csPin, HIGH);
    delayMicroseconds(10);

    uint16_t adcValue2 = ((dataHigh & 0x0F) << 8) | dataLow;

    // Display MCP3208 read data
    Serial.printf("First Read:  ADC Value = 0x%03X (%4d / 4095)\n", adcValue1,
                  adcValue1);
    Serial.printf("Second Read: ADC Value = 0x%03X (%4d / 4095)\n", adcValue2,
                  adcValue2);

    // Check for valid MCP3208 response characteristics
    bool validRange1 = (adcValue1 <= 0xFFF); // 12-bit max
    bool validRange2 = (adcValue2 <= 0xFFF);

    // Check consistency between reads
    uint16_t valueDifference = (adcValue1 > adcValue2)
                                   ? (adcValue1 - adcValue2)
                                   : (adcValue2 - adcValue1);
    bool consistent = (valueDifference <= 5); // Allow small noise margin

    Serial.printf("Valid Range: %s, %s\n", validRange1 ? "✓" : "✗",
                  validRange2 ? "✓" : "✗");
    Serial.printf("Difference: %d counts\n", valueDifference);

    if (consistent) {
      Serial.printf("Consistency: ✓ Consistent reads (diff: %d)\n",
                    valueDifference);
    } else {
      Serial.printf("Consistency: ⚠ Inconsistent reads (diff: %d)\n",
                    valueDifference);
    }

    // Check if values are in reasonable range for MCP3208
    // Typically connected to a reference, not floating high or low
    bool notFloating = !(adcValue1 == 0xFFF && adcValue2 == 0xFFF) &&
                       !(adcValue1 == 0x000 && adcValue2 == 0x000);

    if (notFloating) {
      Serial.printf("Status: ✓ Channel responding with analog data\n");
    } else if (adcValue1 == 0xFFF) {
      Serial.printf(
          "Status: ⚠ Channel floating high (no connection or Vref issue)\n");
    } else if (adcValue1 == 0x000) {
      Serial.printf("Status: ⚠ Channel stuck at zero\n");
    }

    // Determine if MCP3208 is detected
    bool mcp3208Detected =
        validRange1 && validRange2 && consistent && notFloating;

    if (mcp3208Detected) {
      deviceCount++;
      Serial.printf(
          "Result: ✓ MCP3208 detected and responding at CS Pin %d (%s)\n",
          csPin, csNames[i]);
    } else if (consistent && validRange1 && validRange2) {
      Serial.printf("Result: ⚠ Possible MCP3208 with floating input or "
                    "reference issue\n");
    } else {
      Serial.printf("Result: ✗ No MCP3208 detected or communication error\n");
    }
  }

  Serial.println("\n--- Scan Summary ---");
  if (deviceCount == 0) {
    Serial.println("No MCP3208 devices found!");
  } else {
    Serial.printf("Total MCP3208 devices found: %d\n", deviceCount);
  }

  Serial.println("=== MCP3208 Scan Complete ===\n");
  return deviceCount;
}

/**
 * @brief Initialize SPI bus and scan for available devices
 *
 * Convenience function that combines SPI initialization and device scanning.
 *
 * @param clockSpeed SPI clock frequency in Hz (default: 1000000 Hz = 1 MHz)
 * @return Number of devices found on the SPI bus
 */
uint8_t initializeAndScanSPI(uint32_t clockSpeed = 1000000) {
  if (initializeSPI(clockSpeed)) {
    delay(100); // Small delay to allow bus to stabilize
    return scanSPIDevices();
  }
  return 0;
}

#endif // HARDWARE_IO_SETUP_H
