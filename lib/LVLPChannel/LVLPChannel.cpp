#include "LVLPChannel.h"

// DAC supply voltage (defined in hardwareIOSetup.h as const float dacSupplyVoltage=4.957;)
const float DAC_SUPPLY_VOLTAGE = 4.967;
const float vespVoltage = 3.269;


LVLPChannel::LVLPChannel(uint8_t chIndex, 
                         MCP3208* adcPtr, MCP3208::Channel adcCh,
                         Adafruit_MCP4728* dacPtr, MCP4728_channel_t dacCh,
                         ShiftRegister74HC595<2>* srPtr, uint8_t srP,
                         int8_t pwmP, const ChannelCalibrationData& calDataRef) 
  : channelIndex(chIndex), adc(adcPtr), adcChannel(adcCh),
    dac(dacPtr), dacChannel(dacCh), sr(srPtr), srPin(srP), pwmPin(pwmP),
    calData(calDataRef), currentMode(MODE_HIGH_IMPEDANCE), targetVoltage(0), targetCurrent(0), currentDacVoltage(0) {
        // Map the struct values to the old legacy variables to keep existing functions working
        calData.K1=calDataRef.K1;
        calData.K2=calDataRef.K2;
        calData.offset=calDataRef.offset;
        calData.mADC=calDataRef.mADC;
        calData.bADC=calDataRef.bADC;
}

void LVLPChannel::init() {
    disconnect();
    
    // Set DAC to 0
    dac->setChannelValue(dacChannel, calculateDacValue(0.0));
    
    if (pwmPin >= 0) {
        pinMode(pwmPin, OUTPUT);
        digitalWrite(pwmPin, LOW);
    }
}

uint16_t LVLPChannel::calculateDacValue(float volts) {
    // Formula derived from op-amp test: Vout = 3.385*opampGain - Vdac*(opampGain-1)
    // Solving for Vdac: Vdac = (3.385*opampGain - Vout) / (opampGain - 1)
    float dacVoltage = (volts-calData.K1*vespVoltage-calData.offset)/calData.K2;
    Serial.println(dacVoltage);
    
    // Clamp the reference to 0.0 - DAC_SUPPLY_VOLTAGE
    if (dacVoltage < 0.0) dacVoltage = 0.0;
    if (dacVoltage > DAC_SUPPLY_VOLTAGE) dacVoltage = DAC_SUPPLY_VOLTAGE;

    dacValueAttribute=dacVoltage*calData.mDAC+calData.bDAC;
    dacVoltageAttribute=dacVoltage;
    
    return dacValueAttribute;
}



float LVLPChannel::calculateExpectedOutputVoltage(uint16_t dacValue) {
    // float dacVoltage = (dacValue * DAC_SUPPLY_VOLTAGE) / 4096.0;
    // return (vespVoltage+ opampGain*(vespVoltage-dacVoltage));
    return targetVoltage;

}

bool LVLPChannel::setMode(LVLPMode mode) {
    if (mode == MODE_PWM_GENERATOR && pwmPin < 0) {
        // Channel doesn't support PWM
        return false;
    }
    
    currentMode = mode;
    
    if (mode == MODE_HIGH_IMPEDANCE) {
        disconnect();
        // Set PWM pin LOW when disconnected
        if (pwmPin >= 0) {
            digitalWrite(pwmPin, LOW);
        }
    } else {
        // For mode PWM, we do not interfere with standard operations, 
        // the external controller (ESP32) takes over the GPAOM1.
        connect();
        
        // For channels 1 to 4 (which have pwmPin >= 0),
        // the PWM pin must be set HIGH to operate the op-amp properly as a source.
        if (pwmPin >= 0 && mode != MODE_PWM_GENERATOR) {
            digitalWrite(pwmPin, HIGH);
        }
    }
    
    return true;
}

void LVLPChannel::disconnect() {
    sr->set(srPin, LOW);
}

void LVLPChannel::connect() {
    sr->set(srPin, HIGH);
}

void LVLPChannel::setOutputVoltage(float voltage) {
    targetVoltage = voltage;
    currentDacVoltage = voltage;
    dac->setChannelValue(dacChannel, calculateDacValue(currentDacVoltage));
}

void LVLPChannel::setDACOutput(uint16_t value) {
    dac->setChannelValue(dacChannel, value);
    //dac->setChannelValue(dacChannel, 0);
}

void LVLPChannel::setOutputCurrent(float current) {
    targetCurrent = current;
    // Current regulation is handled interactively in the update loop 
    // to step currentDacVoltage up/down.
}

bool LVLPChannel::setPwm(uint8_t dutycycle, uint32_t frequency) {
    if (pwmPin < 0 || currentMode != MODE_PWM_GENERATOR) return false;
    
    analogWriteFrequency(frequency);
    analogWrite(pwmPin, dutycycle);
    return true;
}

float LVLPChannel::readVoltage() {
    uint16_t raw = adc->read(adcChannel);
    float val = calData.mADC *raw+calData.bADC;
    return val;
}

uint16_t LVLPChannel::readMCP3208Value() {
    uint16_t raw = adc->read(adcChannel);
    return raw;
}

float LVLPChannel::readCurrent() {
    float voutActual = readVoltage();
    // V_before_shunt is roughly what we command the DAC to generate
    float vBeforeShunt = calculateExpectedOutputVoltage(calculateDacValue(currentDacVoltage));
    
    // Calculate current according to ohm's law
    return (vBeforeShunt - voutActual) / SHUNT_RESISTANCE;
}

void LVLPChannel::update() {
    if (currentMode == MODE_CURRENT_SOURCE || currentMode == MODE_RESISTIVE_LOAD) {
        float actualCurrent = readCurrent();
        float error = targetCurrent - actualCurrent;
        
        // Simple incremental step depending on error margin
        // E.g. ~10mV step adjustment
        float step = 0.01; 
        
        if (abs(error) > 0.005) { // 5mA deadband
            if (actualCurrent < targetCurrent) {
                currentDacVoltage += step;
            } else {
                currentDacVoltage -= step;
            }
            
            // Saturation limits (assume 15V rail as theoretical maximum)
            if (currentDacVoltage > 15.0) currentDacVoltage = 15.0;
            if (currentDacVoltage < 0.0) currentDacVoltage = 0.0;
            
            dac->setChannelValue(dacChannel, calculateDacValue(currentDacVoltage));
        }
    }
}
