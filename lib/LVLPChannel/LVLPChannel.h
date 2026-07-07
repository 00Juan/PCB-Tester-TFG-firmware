#ifndef LVLP_CHANNEL_H
#define LVLP_CHANNEL_H

#include <Arduino.h>
#include <Mcp320x.h>
#include <Adafruit_MCP4728.h>
#include <ShiftRegister74HC595.h>
#include <FastLED.h>


enum LVLPMode {
    MODE_HIGH_IMPEDANCE,
    MODE_VOLTAGE_SOURCE,
    MODE_CURRENT_SOURCE,
    MODE_RESISTIVE_LOAD,
    MODE_PWM_GENERATOR
};

enum LVLPStatus {
    STATUS_NORMAL,
    STATUS_FAIL_OVERCURRENT,
    STATUS_FAIL_OVERVOLTAGE,
    STATUS_FAIL_OTHER
};

struct ChannelCalibrationData {
    float K1;
    float K2;
    float offset;
    float mADC;
    float bADC;
    float mDAC;
    float bDAC;

};

class LVLPChannel {
private:
    uint8_t channelIndex; // 1 to 8 (for user-facing info)
    
    MCP3208* adc;
    MCP3208::Channel adcChannel;
    
    Adafruit_MCP4728* dac;
    MCP4728_channel_t dacChannel;
    
    ShiftRegister74HC595<2>* sr;
    uint8_t srPin; // 0 to 7
    
    int8_t pwmPin; // -1 if not assigned
    
    LVLPMode currentMode;
    CRGB* led;

    LVLPStatus currentStatus;
    float maxVoltageLimit;
    float maxCurrentLimit;
    void checkLimits();
    
    float targetVoltage;
    float targetCurrent;
    float currentDacVoltage; // Current voltage setting sent to DAC 
                             // (this corresponds to the desired op-amp output voltage before the shunt)
    
    float opampGain;
    float adcGain;
    float adcOffset;

    //Calibration data
    ChannelCalibrationData calData;

    const float SHUNT_RESISTANCE = 10.5;

    
    
    uint16_t calculateDacValue(float outputVolts);
    

public:
    LVLPChannel(uint8_t chIndex, 
                MCP3208* adcPtr, MCP3208::Channel adcCh,
                Adafruit_MCP4728* dacPtr, MCP4728_channel_t dacCh,
                ShiftRegister74HC595<2>* srPtr, uint8_t srP,
                int8_t pwmP, const ChannelCalibrationData& calDataRef,CRGB* ledPtr);

    void setLimits(float maxVoltage, float maxCurrent);
    LVLPStatus getStatus() const { return currentStatus; }
    void resetStatus();
    void init();
    bool setMode(LVLPMode mode);
    void setOutputVoltage(float voltage);
    void setDACOutput(uint16_t value);
    void setOutputCurrent(float current);
    bool setPwm(uint8_t dutycycle, uint32_t frequency);
    
    float readVoltage();
    uint16_t readMCP3208Value();
    float readCurrent();
    float readDACCalculatedVoltage();
    void disconnect(); // Disconnects SSR (High Z or emergency shutdown)
    void connect();    // Connects SSR
    
    void update();     // To be called in loop for current regulation

    float calculateExpectedOutputVoltage(uint16_t dacValue);

    uint16_t dacValueAttribute=0;
    float dacVoltageAttribute=0;
    
    LVLPMode getMode() const { return currentMode; }
};

#endif
