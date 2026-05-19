#ifndef HARDWARE_IO_SETUP_H
#define HARDWARE_IO_SETUP_H

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "SoftI2cMaster.h"

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
const uint8_t pinSrSer = 35;      // Serial data input
const uint8_t pinSrOe = 36;       // Output enable
const uint8_t pinSrRclk = 37;     // Register clock
const uint8_t pinSrSrclk = 38;    // Shift register clock



/**
 * @brief Initialize all configured hardware pins
 */
void initializeHardwarePins() {
    
}

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
            Serial.printf("Device found at address: 0x%02X (decimal: %3d)\n", address, address);
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
 * @param newAddressCode New address code (0-7). Final address will be 0x60 + code
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
    i2c.ldacwrite(0B01100001 | (oldId << 2), pinLedsLdac); // modified command for LDAC pin latch
    i2c.write(0B01100010 | (newId << 2));
    i2c.write(0B01100011 | (newId << 2));
    i2c.stop();
    
    Serial.println("EEPROM write command sent. Waiting for write to complete...");
    delay(100);  // wait for EEPROM writing 
    
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
        Serial.printf("✓ Success! MCP4728 now responds at address 0x%02X\n", newAddress);
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
    Serial.printf("MOSI Pin: %d, MISO Pin: %d, CLK Pin: %d\n", pinSpiMosi, pinSpiMiso, pinSpiClk);
    Serial.printf("Clock Speed: %d Hz\n", clockSpeed);
    
    // Initialize SPI with specified pins
    SPI.begin(pinSpiClk, pinSpiMiso, pinSpiMosi);
    
    Serial.println("SPI Bus initialized successfully!");
    return true;
}

/**
 * @brief Scan the SPI bus for available devices (chip select pins)
 * 
 * Attempts to communicate with each chip select pin to detect connected devices.
 * Tests both ADC chip select pins (ADC1 and ADC2) with enhanced diagnostics.
 * 
 * @return Number of devices found on the SPI bus
 */
uint8_t scanSPIDevices() {
    Serial.println("\n=== SPI Device Scan ===");
    Serial.println("Scanning for SPI devices with detailed diagnostics...\n");
    
    uint8_t deviceCount = 0;
    
    // Array of CS pins to scan
    const uint8_t csPins[] = {pinSpiCsAdc1, pinSpiCsAdc2};
    const char* csNames[] = {"ADC1", "ADC2"};
    const uint8_t numCsPins = sizeof(csPins) / sizeof(csPins[0]);
    
    for (uint8_t i = 0; i < numCsPins; i++) {
        uint8_t csPin = csPins[i];
        
        Serial.printf("\n--- Testing CS Pin %d (%s) ---\n", csPin, csNames[i]);
        
        // Initialize CS pin as output if not already
        pinMode(csPin, OUTPUT);
        digitalWrite(csPin, HIGH);
        delay(10);
        
        // Perform multiple reads to verify device presence
        uint8_t readBytes[4] = {0};
        bool devicePresent = true;
        
        // Read 4 bytes to get more information
        digitalWrite(csPin, LOW);
        delayMicroseconds(10);
        
        for (uint8_t j = 0; j < 4; j++) {
            readBytes[j] = SPI.transfer(0x00);
            delayMicroseconds(5);
        }
        
        digitalWrite(csPin, HIGH);
        delayMicroseconds(10);
        
        // Perform second read to check consistency
        delay(5);
        digitalWrite(csPin, LOW);
        delayMicroseconds(10);
        
        uint8_t readBytes2[4] = {0};
        for (uint8_t j = 0; j < 4; j++) {
            readBytes2[j] = SPI.transfer(0x00);
            delayMicroseconds(5);
        }
        
        digitalWrite(csPin, HIGH);
        delayMicroseconds(10);
        
        // Display read data
        Serial.printf("First Read:  [0x%02X] [0x%02X] [0x%02X] [0x%02X]\n", 
                      readBytes[0], readBytes[1], readBytes[2], readBytes[3]);
        Serial.printf("Second Read: [0x%02X] [0x%02X] [0x%02X] [0x%02X]\n", 
                      readBytes2[0], readBytes2[1], readBytes2[2], readBytes2[3]);
        
        // Check if device is responding (not all 0xFF or 0x00)
        bool allFF = (readBytes[0] == 0xFF && readBytes[1] == 0xFF && 
                      readBytes[2] == 0xFF && readBytes[3] == 0xFF);
        bool all00 = (readBytes[0] == 0x00 && readBytes[1] == 0x00 && 
                      readBytes[2] == 0x00 && readBytes[3] == 0x00);
        
        if (!allFF && !all00) {
            devicePresent = true;
            Serial.printf("Status: ✓ Device detected and responding\n");
        } else if (allFF) {
            Serial.printf("Status: - No device or pull-ups active\n");
            devicePresent = false;
        } else if (all00) {
            Serial.printf("Status: ⚠ All zeros - device may not be responding\n");
            devicePresent = false;
        }
        
        // Check response consistency
        bool consistent = (readBytes[0] == readBytes2[0] && 
                          readBytes[1] == readBytes2[1] && 
                          readBytes[2] == readBytes2[2] && 
                          readBytes[3] == readBytes2[3]);
        
        if (consistent) {
            Serial.printf("Consistency: ✓ Consistent reads\n");
        } else {
            Serial.printf("Consistency: ⚠ Inconsistent reads - possible data transmission issue\n");
        }
        
        // Analyze signal pattern
        uint8_t differentBits = 0;
        for (uint8_t j = 0; j < 4; j++) {
            differentBits += __builtin_popcount(readBytes[j]);
        }
        
        Serial.printf("Signal Pattern: %d bits set (out of 32)\n", differentBits);
        
        if (devicePresent && consistent) {
            deviceCount++;
            Serial.printf("Result: ✓ Device found and confirmed at CS Pin %d (%s)\n", csPin, csNames[i]);
        } else {
            Serial.printf("Result: ✗ Device communication uncertain\n");
        }
    }
    
    Serial.println("\n--- Scan Summary ---");
    if (deviceCount == 0) {
        Serial.println("No reliable SPI devices found!");
    } else {
        Serial.printf("Total SPI devices found: %d\n", deviceCount);
    }
    
    Serial.println("=== Scan Complete ===\n");
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
        delay(100);  // Small delay to allow bus to stabilize
        return scanSPIDevices();
    }
    return 0;
}





#endif // HARDWARE_IO_SETUP_H
