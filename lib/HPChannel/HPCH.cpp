#include "HPCH.h"

// ============================================================================
// Constructor
// ============================================================================

HPCH::HPCH(uint8_t chIndex,
           MCP3208* adcPtr,
           MCP3208::Channel adcChIn,
           MCP3208::Channel adcChOut,
           MCP3208::Channel adcChCurr,
           ShiftRegister74HC595<2>* srPtr,
           uint8_t srP,
           const HPCHCalibrationData& calDataRef,
           CRGB* ledPtr)
    : channelIndex(chIndex),
      adc(adcPtr),
      adcChVIn(adcChIn), adcChVOut(adcChOut), adcChCurrent(adcChCurr),
      sr(srPtr), srPin(srP), led(ledPtr),
      channelStatus(HPCH_STATUS_NORMAL),
      maxVoltageLimit(15.0f), maxCurrentLimit(10.0f),
      calData(calDataRef),
      acs725_zero_adc(2048), // default mid-point for 12-bit ADC
      channelVIn(0.0f), channelVOut(0.0f), channelCurrentOut(0.0f)
{
    calPointCollected[0] = false;
    calPointCollected[1] = false;
    calPointVIn[0]  = {0, 0.0f};
    calPointVIn[1]  = {0, 0.0f};
    calPointVOut[0] = {0, 0.0f};
    calPointVOut[1] = {0, 0.0f};
}

// ============================================================================
// Lifecycle
// ============================================================================

void HPCH::init() {
    disconnect();

    if (led) {
        *led = CRGB::Green;
        FastLED.show();
    }
}

void HPCH::update() {
    channelVIn      = readVIn();
    channelVOut     = readVOut();
    channelCurrentOut = readCurrent();

    checkLimits();
}

// ============================================================================
// Connection control
// ============================================================================

void HPCH::connect() {
    if (channelStatus == HPCH_STATUS_NORMAL) {
        sr->set(srPin, HIGH); // HIGH turns ON the PMOS switch
        connected = true;
        if (led) {
            *led = CRGB::Green;
            FastLED.show();
        }
    }
}

void HPCH::disconnect() {
    sr->set(srPin, LOW); // LOW turns OFF the PMOS switch
    connected = false;
}

// ============================================================================
// Limits & status
// ============================================================================

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

void HPCH::checkLimits() {
    if (channelStatus != HPCH_STATUS_NORMAL) return;

    if (channelVOut > maxVoltageLimit) {
        disconnect();
        channelStatus = HPCH_STATUS_FAIL_OVERVOLTAGE;
        if (led) {
            *led = CRGB::Blue;
            FastLED.show();
        }
    } else if (fabsf(channelCurrentOut) > maxCurrentLimit) {
        disconnect();
        channelStatus = HPCH_STATUS_FAIL_OVERCURRENT;
        if (led) {
            *led = CRGB::Red;
            FastLED.show();
        }
    }
}

// ============================================================================
// Private helpers
// ============================================================================

uint16_t HPCH::sampleADCAverage(MCP3208::Channel ch, uint8_t numSamples) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < numSamples; i++) {
        sum += adc->read(ch);
        delay(1);
    }
    return static_cast<uint16_t>(sum / numSamples);
}

void HPCH::fitLinearFromTwoPoints(const ADCCalPoint& p0, const ADCCalPoint& p1,
                                  float& outM, float& outB) {
    float dx = static_cast<float>(p1.raw) - static_cast<float>(p0.raw);
    if (fabsf(dx) < 1.0f) {
        // Degenerate: both points have the same raw value — keep existing coefficients
        return;
    }
    outM = (p1.voltage - p0.voltage) / dx;
    outB = p0.voltage - outM * static_cast<float>(p0.raw);
}

// ============================================================================
// Calibration — ADC two-point linear fit
// ============================================================================

bool HPCH::calibrateADCPoint(uint8_t pointIndex, float knownVIn, float knownVOut) {
    if (pointIndex > 1) {
        Serial.printf("HPCH%u calibrateADCPoint: invalid pointIndex %u (must be 0 or 1)\n",
                      channelIndex, pointIndex);
        return false;
    }

    Serial.printf("HPCH%u ADC cal point %u — averaging 64 samples...\n",
                  channelIndex, pointIndex);

    uint16_t rawVIn  = sampleADCAverage(adcChVIn,  64);
    uint16_t rawVOut = sampleADCAverage(adcChVOut, 64);

    calPointVIn[pointIndex]  = {rawVIn,  knownVIn};
    calPointVOut[pointIndex] = {rawVOut, knownVOut};
    calPointCollected[pointIndex] = true;

    Serial.printf("  VIn  raw=%4u  known=%.4f V\n", rawVIn,  knownVIn);
    Serial.printf("  VOut raw=%4u  known=%.4f V\n", rawVOut, knownVOut);

    if (calPointCollected[0] && calPointCollected[1]) {
        // Both points collected — compute linear coefficients
        fitLinearFromTwoPoints(calPointVIn[0],  calPointVIn[1],
                               calData.mADC_VIn,  calData.bADC_VIn);
        fitLinearFromTwoPoints(calPointVOut[0], calPointVOut[1],
                               calData.mADC_VOut, calData.bADC_VOut);

        Serial.printf("HPCH%u ADC calibration complete:\n", channelIndex);
        Serial.printf("  VIn  : m=%.8f  b=%.6f\n", calData.mADC_VIn,  calData.bADC_VIn);
        Serial.printf("  VOut : m=%.8f  b=%.6f\n", calData.mADC_VOut, calData.bADC_VOut);
        return true;
    }

    Serial.printf("HPCH%u ADC point %u stored. Collect point %u to complete calibration.\n",
                  channelIndex, pointIndex, pointIndex == 0 ? 1 : 0);
    return false;
}

// ============================================================================
// Calibration — ACS725 zero-current offset
// ============================================================================

void HPCH::calibrateACS725ZeroOffset() {
    Serial.printf("HPCH%u ACS725 zero-offset calibration — disconnecting channel...\n",
                  channelIndex);
    disconnect();
    delay(50); // Allow any transients to settle

    Serial.printf("HPCH%u averaging 200 samples for zero offset...\n", channelIndex);

    uint32_t sum = 0;
    for (int i = 0; i < 200; i++) {
        sum += adc->read(adcChCurrent);
        delay(1);
    }
    acs725_zero_adc = static_cast<uint16_t>(sum / 200);

    float zeroVoltage = acs725_zero_adc * (calData.adc_vref / 4095.0f);

    Serial.printf("HPCH%u ACS725 zero offset: raw=%u  (%.4f V)\n",
                  channelIndex, acs725_zero_adc, zeroVoltage);
}

// ============================================================================
// Calibration — ACS725 sensitivity
// ============================================================================

bool HPCH::calibrateACS725Sensitivity(float knownCurrentAmps) {
    if (knownCurrentAmps <= 0.0f) {
        Serial.printf("HPCH%u calibrateACS725Sensitivity: knownCurrentAmps must be > 0\n",
                      channelIndex);
        return false;
    }

    Serial.printf("HPCH%u ACS725 sensitivity calibration at %.4f A — averaging 200 samples...\n",
                  channelIndex, knownCurrentAmps);

    uint32_t sum = 0;
    for (int i = 0; i < 200; i++) {
        sum += adc->read(adcChCurrent);
        delay(1);
    }
    uint16_t rawNow = static_cast<uint16_t>(sum / 200);

    float vNow  = rawNow         * (calData.adc_vref / 4095.0f);
    float vZero = acs725_zero_adc * (calData.adc_vref / 4095.0f);
    float vDiff = vNow - vZero;

    Serial.printf("  raw_now=%u  V_now=%.4f V  V_zero=%.4f V  V_diff=%.4f V\n",
                  rawNow, vNow, vZero, vDiff);

    // Require at least 10 mV difference to avoid noise-dominated calibration
    if (fabsf(vDiff) < 0.010f) {
        Serial.printf("HPCH%u Error: voltage difference (%.4f V) too small — "
                      "is the channel carrying current?\n",
                      channelIndex, vDiff);
        return false;
    }

    calData.acs725_sensitivity = vDiff / knownCurrentAmps;

    Serial.printf("HPCH%u ACS725 sensitivity: %.6f V/A\n",
                  channelIndex, calData.acs725_sensitivity);
    return true;
}

// ============================================================================
// Calibration data management
// ============================================================================

void HPCH::setCalibrationData(const HPCHCalibrationData& newCalData) {
    calData = newCalData;
}

void HPCH::printCalibrationBlockForPaste() const {
    Serial.println("\n=== HPCHCalibrationData block for paste into hardwareIOSetup.h ===");
    Serial.println("HPCHCalibrationData hpCalData[2] = {");
    Serial.printf("    // mADC_VIn, bADC_VIn, mADC_VOut, bADC_VOut, acs725_sensitivity, adc_vref\n");
    // Print line for this channel (channelIndex is 1-based)
    Serial.printf("    { %.9ff, %.9ff, %.9ff, %.9ff, %.9ff, %.9ff }, // HPCH%u\n",
                  calData.mADC_VIn,
                  calData.bADC_VIn,
                  calData.mADC_VOut,
                  calData.bADC_VOut,
                  calData.acs725_sensitivity,
                  calData.adc_vref,
                  channelIndex);
    Serial.println("}; // (complete the array for the other channel)");
    Serial.printf("// ACS725 zero ADC baseline for HPCH%u: %u\n",
                  channelIndex, acs725_zero_adc);
    Serial.println("=== End block ===\n");
}

// ============================================================================
// Sensor reads
// ============================================================================

float HPCH::readVIn() {
    uint16_t raw = adc->read(adcChVIn);
    // V_real = mADC_VIn * raw + bADC_VIn
    return calData.mADC_VIn * static_cast<float>(raw) + calData.bADC_VIn;
}

float HPCH::readVOut() {
    uint16_t raw = adc->read(adcChVOut);
    return calData.mADC_VOut * static_cast<float>(raw) + calData.bADC_VOut;
}

float HPCH::readCurrent() {
    uint16_t raw = adc->read(adcChCurrent);

    float vNow  = static_cast<float>(raw)          * (calData.adc_vref / 4095.0f);
    float vZero = static_cast<float>(acs725_zero_adc) * (calData.adc_vref / 4095.0f);
    float vDiff = vNow - vZero;

    // I = V_diff / Sensitivity
    return vDiff / calData.acs725_sensitivity;
}

// ============================================================================
// Debug
// ============================================================================

void HPCH::printDebugInfo() const {
    Serial.println("=== HPCH Debug Info ===");
    Serial.print  ("Channel Index  : "); Serial.println(channelIndex);
    Serial.print  ("Status         : ");
    switch (channelStatus) {
        case HPCH_STATUS_NORMAL:           Serial.println("NORMAL");           break;
        case HPCH_STATUS_FAIL_OVERCURRENT: Serial.println("FAIL_OVERCURRENT"); break;
        case HPCH_STATUS_FAIL_OVERVOLTAGE: Serial.println("FAIL_OVERVOLTAGE"); break;
        default:                           Serial.println("FAIL_OTHER");       break;
    }
    Serial.printf("V_In           : %.4f V\n", channelVIn);
    Serial.printf("V_Out          : %.4f V\n", channelVOut);
    Serial.printf("Current        : %.5f A\n", channelCurrentOut);
    Serial.printf("Max V Limit    : %.2f V\n", maxVoltageLimit);
    Serial.printf("Max I Limit    : %.2f A\n", maxCurrentLimit);
    Serial.println("--- Calibration ---");
    Serial.printf("mADC_VIn       : %.9f\n", calData.mADC_VIn);
    Serial.printf("bADC_VIn       : %.6f\n", calData.bADC_VIn);
    Serial.printf("mADC_VOut      : %.9f\n", calData.mADC_VOut);
    Serial.printf("bADC_VOut      : %.6f\n", calData.bADC_VOut);
    Serial.printf("ACS Sensitivity: %.6f V/A\n", calData.acs725_sensitivity);
    Serial.printf("ADC Vref       : %.4f V\n",   calData.adc_vref);
    Serial.printf("ACS Zero ADC   : %u\n",        acs725_zero_adc);
    Serial.println("==============================");
}
