#ifndef HPCH_H
#define HPCH_H

#include <Arduino.h>
#include <Mcp320x.h>
#include <ShiftRegister74HC595.h>
#include <FastLED.h>

enum HPCHStatus {
    HPCH_STATUS_NORMAL,
    HPCH_STATUS_FAIL_OVERCURRENT,
    HPCH_STATUS_FAIL_OVERVOLTAGE,
    HPCH_STATUS_FAIL_OTHER
};

struct HPCHCalibrationData {
    float mADC_VIn;
    float bADC_VIn;
    float mADC_VOut;
    float bADC_VOut;
    
    // Default 0.132 V/A for ACS725LLCTR-10AB-T
    float acs725_sensitivity; 
    
    // The ADC reference voltage, default typically 3.3V or 5.0V
    float adc_vref; 
};

class HPCH {
private:
    uint8_t channelIndex; // Identifier (1 to N)
    
    MCP3208* adc;
    MCP3208::Channel adcChVIn;
    MCP3208::Channel adcChVOut;
    MCP3208::Channel adcChCurrent;
    
    ShiftRegister74HC595<2>* sr;
    uint8_t srPin;
    
    CRGB* led;
    
    HPCHStatus channelStatus;
    
    float maxVoltageLimit;
    float maxCurrentLimit;
    
    HPCHCalibrationData calData;
    
    // Auto-calibrated zero current ADC value
    uint16_t acs725_zero_adc;

    // Internal voltages
    float channelVIn;
    float channelVOut;
    float channelCurrentOut;

    void checkLimits();

public:
    HPCH(uint8_t chIndex, 
         MCP3208* adcPtr, 
         MCP3208::Channel adcChIn, 
         MCP3208::Channel adcChOut, 
         MCP3208::Channel adcChCurr,
         ShiftRegister74HC595<2>* srPtr, 
         uint8_t srP,
         const HPCHCalibrationData& calDataRef, 
         CRGB* ledPtr);

    void init();
    
    void connect();    // Close the PMOS switch
    void disconnect(); // Open the PMOS switch
    
    void setLimits(float maxVoltage, float maxCurrent);
    HPCHStatus getStatus() const { return channelStatus; }
    void resetStatus();
    
    void setCalibrationData(const HPCHCalibrationData& newCalData);
    
    void calibrateACS725();
    
    void update(); // Must be called periodically to check limits and read sensors

    float readVIn();
    float readVOut();
    float readCurrent();
    
    void printDebugInfo() const;
};

#endif // HPCH_H
