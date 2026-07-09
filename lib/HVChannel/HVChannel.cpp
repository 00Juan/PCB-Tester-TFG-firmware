#include "HVChannel.h"

// ============================================================================
// Constructor
// ============================================================================

HVChannel::HVChannel(uint8_t chIndex,
                     MCP3208* adcPtr,
                     MCP3208::Channel adcCh,
                     ShiftRegister74HC595<2>* srPtr,
                     uint8_t srP,
                     const HVChannelCalibrationData& calDataRef,
                     CRGB* ledPtr)
    : channelIndex(chIndex),
      adc(adcPtr),
      adcChannel(adcCh),
      sr(srPtr),
      srPin(srP),
      led(ledPtr),
      channelStatus(HV_STATUS_NORMAL),
      maxVoltageLimit(500.0f),   // safe default — user should call setLimits()
      calData(calDataRef),
      channelVoltage(0.0f)
{}

// ============================================================================
// Lifecycle
// ============================================================================

void HVChannel::init() {
    disconnect();

    if (led) {
        *led = CRGB::Green;
        FastLED.show();
    }
}

void HVChannel::update() {
    channelVoltage = readVoltage();
    checkLimits();
}

// ============================================================================
// Connection control
// ============================================================================

void HVChannel::connect() {
    if (channelStatus == HV_STATUS_NORMAL) {
        sr->set(srPin, HIGH);
        if (led) {
            *led = CRGB::Green;
            FastLED.show();
        }
    }
}

void HVChannel::disconnect() {
    sr->set(srPin, LOW);
}

// ============================================================================
// Limits & status
// ============================================================================

void HVChannel::setLimits(float maxVoltage) {
    maxVoltageLimit = maxVoltage;
}

void HVChannel::resetStatus() {
    disconnect();
    channelStatus = HV_STATUS_NORMAL;
    if (led) {
        *led = CRGB::Green;
        FastLED.show();
    }
}

void HVChannel::checkLimits() {
    if (channelStatus != HV_STATUS_NORMAL) return;

    if (channelVoltage > maxVoltageLimit) {
        disconnect();
        channelStatus = HV_STATUS_FAIL_OVERVOLTAGE;
        if (led) {
            *led = CRGB::Red;
            FastLED.show();
        }
        Serial.printf("[HVChannel%u] OVERVOLTAGE: %.2f V > %.2f V — relay opened.\n",
                      channelIndex, channelVoltage, maxVoltageLimit);
    }
}

// ============================================================================
// Private helpers
// ============================================================================

uint16_t HVChannel::sampleADCAverage(uint8_t numSamples) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < numSamples; i++) {
        sum += adc->read(adcChannel);
        delay(1);
    }
    return static_cast<uint16_t>(sum / numSamples);
}

float HVChannel::pwlLookup(uint16_t raw) const {
    // Inside dead-zone → 0 V
    if (raw <= calData.deadZoneRaw || calData.numPoints < 2) {
        return 0.0f;
    }

    // Below first calibrated point → extrapolate / clamp
    if (raw <= calData.points[0].raw) {
        return calData.points[0].voltage;
    }

    // Above last calibrated point → clamp to last point value
    uint8_t n = calData.numPoints;
    if (raw >= calData.points[n - 1].raw) {
        return calData.points[n - 1].voltage;
    }

    // Binary search for the surrounding segment
    uint8_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        uint8_t mid = (lo + hi) / 2;
        if (raw >= calData.points[mid].raw) lo = mid;
        else                               hi = mid;
    }

    // Linear interpolation between points[lo] and points[hi]
    float rawLo = static_cast<float>(calData.points[lo].raw);
    float rawHi = static_cast<float>(calData.points[hi].raw);
    float dx     = rawHi - rawLo;

    if (fabsf(dx) < 1.0f) {
        // Degenerate segment — return lower voltage
        return calData.points[lo].voltage;
    }

    float t = (static_cast<float>(raw) - rawLo) / dx;
    return calData.points[lo].voltage + t * (calData.points[hi].voltage - calData.points[lo].voltage);
}

void HVChannel::sortCalibrationPoints() {
    // Insertion sort — the table is at most HV_CAL_MAX_POINTS long, so O(n²) is fine
    for (uint8_t i = 1; i < calData.numPoints; i++) {
        HVCalPoint key = calData.points[i];
        int8_t j = i - 1;
        while (j >= 0 && calData.points[j].raw > key.raw) {
            calData.points[j + 1] = calData.points[j];
            j--;
        }
        calData.points[j + 1] = key;
    }
}

// ============================================================================
// Sensor reads
// ============================================================================

float HVChannel::readVoltage() {
    uint16_t raw = adc->read(adcChannel);
    return pwlLookup(raw);
}

uint16_t HVChannel::readRaw() {
    return adc->read(adcChannel);
}

// ============================================================================
// Calibration
// ============================================================================

void HVChannel::calibrationReset() {
    calData.deadZoneRaw = 0;
    calData.numPoints   = 0;
    for (uint8_t i = 0; i < HV_CAL_MAX_POINTS; i++) {
        calData.points[i] = {0, 0.0f};
    }
    Serial.printf("[HVChannel%u] Calibration table cleared.\n", channelIndex);
}

void HVChannel::calibrateDeadZone(float knownVoltage) {
    Serial.printf("[HVChannel%u] Measuring dead-zone boundary — averaging 128 samples...\n",
                  channelIndex);
    uint16_t raw = sampleADCAverage(128);

    calData.deadZoneRaw = raw;

    Serial.printf("[HVChannel%u] Dead-zone threshold set: raw=%u (applied voltage=%.2f V)\n",
                  channelIndex, raw, knownVoltage);

    // Also record this as the first calibration point
    if (calData.numPoints < HV_CAL_MAX_POINTS) {
        calData.points[calData.numPoints++] = {raw, knownVoltage};
        Serial.printf("[HVChannel%u] Point #%u stored: raw=%u  voltage=%.4f V\n",
                      channelIndex, calData.numPoints, raw, knownVoltage);
    }
}

bool HVChannel::calibrateAddPoint(float knownVoltage, uint8_t numSamples) {
    if (calData.numPoints >= HV_CAL_MAX_POINTS) {
        Serial.printf("[HVChannel%u] Calibration table full (%u points max).\n",
                      channelIndex, HV_CAL_MAX_POINTS);
        return false;
    }

    Serial.printf("[HVChannel%u] Adding cal point at %.4f V — averaging %u samples...\n",
                  channelIndex, knownVoltage, numSamples);

    uint16_t raw = sampleADCAverage(numSamples);
    calData.points[calData.numPoints++] = {raw, knownVoltage};

    Serial.printf("[HVChannel%u] Point #%u stored: raw=%u  voltage=%.4f V\n",
                  channelIndex, calData.numPoints, raw, knownVoltage);
    return true;
}

bool HVChannel::calibrateFinish() {
    if (calData.numPoints < 2) {
        Serial.printf("[HVChannel%u] Calibration INCOMPLETE: only %u point(s) — need at least 2.\n",
                      channelIndex, calData.numPoints);
        return false;
    }

    sortCalibrationPoints();

    Serial.printf("[HVChannel%u] Calibration finalised with %u points:\n",
                  channelIndex, calData.numPoints);
    Serial.printf("  Dead-zone threshold : raw <= %u\n", calData.deadZoneRaw);
    for (uint8_t i = 0; i < calData.numPoints; i++) {
        Serial.printf("  [%2u] raw=%4u  V=%.4f V\n",
                      i, calData.points[i].raw, calData.points[i].voltage);
    }
    return true;
}

// ============================================================================
// Calibration data management
// ============================================================================

void HVChannel::setCalibrationData(const HVChannelCalibrationData& newCalData) {
    calData = newCalData;
}

void HVChannel::printCalibrationBlockForPaste() const {
    Serial.printf("\n=== HVChannelCalibrationData block for HVChannel%u ===\n", channelIndex);
    Serial.println("HVChannelCalibrationData hvCalData = {");
    Serial.printf("    /* deadZoneRaw */ %u,\n", calData.deadZoneRaw);
    Serial.println("    /* points[] */ {");
    for (uint8_t i = 0; i < HV_CAL_MAX_POINTS; i++) {
        if (i < calData.numPoints) {
            Serial.printf("        { %4u, %.9ff },  // [%u] %.4f V\n",
                          calData.points[i].raw,
                          calData.points[i].voltage,
                          i,
                          calData.points[i].voltage);
        } else {
            Serial.printf("        {    0, 0.0f },             // [%u] unused\n", i);
        }
    }
    Serial.println("    },");
    Serial.printf("    /* numPoints */ %u,\n", calData.numPoints);
    Serial.printf("    /* adc_vref  */ %.9ff\n", calData.adc_vref);
    Serial.println("};");
    Serial.println("=== End block ===\n");
}

// ============================================================================
// Debug
// ============================================================================

void HVChannel::printDebugInfo() const {
    Serial.println("=== HVChannel Debug Info ===");
    Serial.printf("Channel Index  : %u\n", channelIndex);
    Serial.print ("Status         : ");
    switch (channelStatus) {
        case HV_STATUS_NORMAL:           Serial.println("NORMAL");           break;
        case HV_STATUS_FAIL_OVERVOLTAGE: Serial.println("FAIL_OVERVOLTAGE"); break;
        default:                         Serial.println("FAIL_OTHER");       break;
    }
    Serial.printf("Voltage        : %.4f V\n",  channelVoltage);
    Serial.printf("Max V Limit    : %.2f V\n",  maxVoltageLimit);
    Serial.println("--- Calibration ---");
    Serial.printf("Dead-zone raw  : %u\n",       calData.deadZoneRaw);
    Serial.printf("Num points     : %u\n",       calData.numPoints);
    Serial.printf("ADC Vref       : %.4f V\n",   calData.adc_vref);
    for (uint8_t i = 0; i < calData.numPoints; i++) {
        Serial.printf("  [%2u] raw=%4u  V=%.4f V\n",
                      i, calData.points[i].raw, calData.points[i].voltage);
    }
    Serial.println("============================");
}
