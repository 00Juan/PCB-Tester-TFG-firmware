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
    uint8_t pwmResolutionBits; // LEDC resolution (1-14 bits, default 8)
    
    LVLPMode channelMode;
    CRGB* led;

    LVLPStatus channelStatus;
    float maxVoltageLimit;
    float maxCurrentLimit;
    bool connected = false;      // SSR state, tracked by connect()/disconnect()
    uint16_t pwmDuty = 0;        // last values accepted by setPwm()
    uint32_t pwmFreq = 0;
    void checkLimits();
    
    float userTargetVoltage;
    float targetCurrent;
    float loopTargetVoltage; 

    // PID variables
    float pidKp;
    float pidKi;
    float pidKd;
    float pidIntegral;
    float pidPrevError;
    uint32_t pidLastTime;

    float channelVoltageOut;
    float channelCurrentOut;


    float opampGain;
    float adcGain;
    float adcOffset;

    //Calibration data
    ChannelCalibrationData calData;

    const float SHUNT_RESISTANCE = 10.5;

    // Over-current must persist for at least this long before the channel trips.
    // This rides through the brief inrush/settling transient produced when the
    // output voltage steps into a (capacitive) DUT, without masking a real fault.
    // Expressed as a TIME so it is independent of the channel update rate.
    // The tester's own output settles in <1 ms, so any over-current lasting
    // longer than this window is real. Trip latency is at most this value plus
    // one update period.
    //
    // Static so a single window applies to every channel, and a plain variable
    // (not constexpr) so it can be tuned at runtime — see
    // setOverCurrentDebounceMs(). Default 2 ms; defined in the .cpp.
    static uint32_t overCurrentDebounceMs;

    // Debounce state for the over-current trip (see checkLimits()).
    bool overCurrentActive = false;   // an over-current episode is in progress
    uint32_t overCurrentSinceMs = 0;  // millis() when the episode started


    
    uint16_t calculateDacValue(float outputVolts);
    

public:
    LVLPChannel(uint8_t chIndex, 
                MCP3208* adcPtr, MCP3208::Channel adcCh,
                Adafruit_MCP4728* dacPtr, MCP4728_channel_t dacCh,
                ShiftRegister74HC595<2>* srPtr, uint8_t srP,
                int8_t pwmP, const ChannelCalibrationData& calDataRef,CRGB* ledPtr);

    void setLimits(float maxVoltage, float maxCurrent);

    // Over-current debounce window (ms), shared by every channel. Tunable at
    // runtime (e.g. from the GUI); see checkLimits(). Default 2 ms.
    static void setOverCurrentDebounceMs(uint32_t ms) { overCurrentDebounceMs = ms; }
    static uint32_t getOverCurrentDebounceMs() { return overCurrentDebounceMs; }

    LVLPStatus getStatus() const { return channelStatus; }
    void resetStatus();
    void init();
    bool setMode(LVLPMode mode);
    void setOutputVoltage(float voltage);
    void setDACOutput(uint16_t value);
    void setOutputCurrent(float current);
    bool setPwm(uint16_t dutycycle, uint32_t frequency);
    void setPwmResolution(uint8_t bits);
    uint8_t getPwmResolution() const { return pwmResolutionBits; }
    uint16_t getPwmMaxDuty()   const { return (1u << pwmResolutionBits) - 1u; }
    void setCalibrationData(const ChannelCalibrationData& newCalData);
    
    void setPIDTunings(float kp, float ki, float kd);
    void resetPID();
    
    float readVoltage();
    uint16_t readMCP3208Value();
    float readCurrent();
    float readDACCalculatedVoltage();
    void disconnect(); // Disconnects SSR (High Z or emergency shutdown)
    void connect();    // Connects SSR
    
    void update();     // To be called in loop for current regulation

    // Refresh cached voltage/current and re-evaluate the safety limits WITHOUT
    // running the regulation loop (no DAC writes). Lets the blocking testbench
    // keep over-current / over-voltage protection live during a campaign run
    // without disturbing channel outputs. Returns true if the channel is in a
    // fail state after the check.
    bool monitorFault();

    float calculateExpectedOutputVoltage(uint16_t dacValue);

    void printDebugInfo() const;

    uint16_t dacValueAttribute=0;
    float dacVoltageAttribute=0;

    LVLPMode getMode() const { return channelMode; }

    // State snapshot getters (values cached by update() — no bus traffic)
    uint8_t  getIndex() const { return channelIndex; }
    float    getTargetVoltage() const { return userTargetVoltage; }
    float    getTargetCurrent() const { return targetCurrent; }
    float    getLastVoltage() const { return channelVoltageOut; }
    float    getLastCurrent() const { return channelCurrentOut; }
    float    getMaxVoltageLimit() const { return maxVoltageLimit; }
    float    getMaxCurrentLimit() const { return maxCurrentLimit; }
    float    getShuntResistance() const { return SHUNT_RESISTANCE; }
    bool     isConnected() const { return connected; }
    bool     hasPwm() const { return pwmPin >= 0; }
    uint16_t getPwmDuty() const { return pwmDuty; }
    uint32_t getPwmFreq() const { return pwmFreq; }
    const ChannelCalibrationData& getCalibrationData() const { return calData; }
};

#endif
