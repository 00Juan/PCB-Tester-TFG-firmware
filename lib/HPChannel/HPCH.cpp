#include "HPCH.h"

HPCH::HPCH(uint8_t chIndex, 
           MCP3208* adcPtr, 
           MCP3208::Channel adcChIn, 
           MCP3208::Channel adcChOut, 
           MCP3208::Channel adcChCurr,
           ShiftRegister74HC595<2>* srPtr, 
           uint8_t srP,
           const HPCHCalibrationData& calDataRef, 
           CRGB* ledPtr)
    : channelIndex(chIndex), adc(adcPtr), 
      adcChVIn(adcChIn), adcChVOut(adcChOut), adcChCurrent(adcChCurr),
      sr(srPtr), srPin(srP), led(ledPtr), 
      channelStatus(HPCH_STATUS_NORMAL),
      maxVoltageLimit(15.0f), maxCurrentLimit(10.0f),
      calData(calDataRef), acs725_zero_adc(2048), // default mid-point for 12-bit
      channelVIn(0), channelVOut(0), channelCurrentOut(0)
{
}

void HPCH::init() {
    disconnect();
    
    if (led) {
        *led = CRGB::Green;
        FastLED.show();
    }
}

void HPCH::setLimits(float maxVoltage, float maxCurrent) {
    maxVoltageLimit = maxVoltage;
    maxCurrentLimit = maxCurrent;
}

void HPCH::resetStatus() {
    disconnect();
    channelStatus = HPCH_STATUS_NORMAL;
    if (led) {
        *led = CRGB::Green;
        FastLED.show();
    }
}

void HPCH::setCalibrationData(const HPCHCalibrationData& newCalData) {
    calData = newCalData;
}

void HPCH::calibrateACS725() {
    // Ensure the channel is open so 0A flows
    disconnect();
    delay(10); // Wait for transients to settle

    uint32_t sum = 0;
    const int numSamples = 100;
    
    for (int i = 0; i < numSamples; i++) {
        sum += adc->read(adcChCurrent);
        delay(1);
    }
    
    acs725_zero_adc = sum / numSamples;
}

void HPCH::connect() {
    if (channelStatus == HPCH_STATUS_NORMAL) {
        sr->set(srPin, HIGH); // Assuming HIGH turns ON the PMOS
        if (led) {
            *led = CRGB::Green;
            FastLED.show();
        }
    }
}

void HPCH::disconnect() {
    sr->set(srPin, LOW); // Assuming LOW turns OFF the PMOS
}

float HPCH::readVIn() {
    uint16_t raw = adc->read(adcChVIn);
    // V_ADC = raw * (adc_vref / 4095.0)
    // V_actual = m * V_ADC + b 
    // OR directly using mADC and bADC to convert raw to volts
    float val = calData.mADC_VIn * raw + calData.bADC_VIn;
    return val;
}

float HPCH::readVOut() {
    uint16_t raw = adc->read(adcChVOut);
    float val = calData.mADC_VOut * raw + calData.bADC_VOut;
    return val;
}

float HPCH::readCurrent() {
    uint16_t raw = adc->read(adcChCurrent);
    
    // Calculate difference in voltage from zero point
    float rawVoltage = raw * (calData.adc_vref / 4095.0f);
    float zeroVoltage = acs725_zero_adc * (calData.adc_vref / 4095.0f);
    
    float voltageDiff = rawVoltage - zeroVoltage;
    
    // Current = V_diff / Sensitivity
    float current = voltageDiff / calData.acs725_sensitivity;
    
    return current;
}

void HPCH::checkLimits() {
    if (channelStatus != HPCH_STATUS_NORMAL) return;

    if (channelVOut > maxVoltageLimit) {
        disconnect();
        channelStatus = HPCH_STATUS_FAIL_OVERVOLTAGE;
        if (led) {
            *led = CRGB::Blue;
            FastLED.show();
        }
    } else if (abs(channelCurrentOut) > maxCurrentLimit) {
        disconnect();
        channelStatus = HPCH_STATUS_FAIL_OVERCURRENT;
        if (led) {
            *led = CRGB::Red;
            FastLED.show();
        }
    }
}

void HPCH::update() {
    channelVIn = readVIn();
    channelVOut = readVOut();
    channelCurrentOut = readCurrent();
    
    checkLimits();
}

void HPCH::printDebugInfo() const {
    Serial.println("=== HPCH Debug Info ===");
    Serial.print("Channel Index: "); Serial.println(channelIndex);
    Serial.print("Channel Status: "); Serial.println(channelStatus);
    
    Serial.print("Current(A)= "); Serial.println(channelCurrentOut, 4);
    Serial.print("V_In(V)= "); Serial.println(channelVIn, 4);
    Serial.print("V_Out(V)= "); Serial.println(channelVOut, 4);
    
    Serial.print("Zero Current ADC: "); Serial.println(acs725_zero_adc);
    
    Serial.print("Max Voltage Limit: "); Serial.println(maxVoltageLimit, 4);
    Serial.print("Max Current Limit: "); Serial.println(maxCurrentLimit, 4);
    
    Serial.print("Cal mADC_VIn: "); Serial.println(calData.mADC_VIn, 6);
    Serial.print("Cal bADC_VIn: "); Serial.println(calData.bADC_VIn, 6);
    Serial.print("Cal mADC_VOut: "); Serial.println(calData.mADC_VOut, 6);
    Serial.print("Cal bADC_VOut: "); Serial.println(calData.bADC_VOut, 6);
    Serial.print("Cal ACS Sensitivity: "); Serial.println(calData.acs725_sensitivity, 6);
    Serial.print("Cal ADC Vref: "); Serial.println(calData.adc_vref, 6);
    Serial.println("==============================");
}
