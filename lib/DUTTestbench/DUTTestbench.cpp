#include "DUTTestbench.h"
#include <math.h>   // fabsf, sqrtf

// ============================================================================
// Constructor / begin
// ============================================================================

DUTTestRunner::DUTTestRunner()
    : chCount_(0), testCount_(0) {
    for (uint8_t i = 0; i < DUT_MAX_CHANNELS; i++) ch_[i] = nullptr;
}

void DUTTestRunner::begin(LVLPChannel* channels[], uint8_t count) {
    chCount_ = (count < DUT_MAX_CHANNELS) ? count : DUT_MAX_CHANNELS;
    for (uint8_t i = 0; i < chCount_; i++) {
        ch_[i] = channels[i];
    }
    testCount_ = 0;
}

// ============================================================================
// Test management
// ============================================================================

bool DUTTestRunner::addTest(const TestCase& tc) {
    if (testCount_ >= DUT_MAX_TESTS) return false;
    tests_[testCount_++] = tc;
    return true;
}

void DUTTestRunner::clearTests() {
    testCount_ = 0;
}

// ============================================================================
// Run all / run one
// ============================================================================

void DUTTestRunner::runAll(U8G2* display) {
    display_ = display;
    Serial.println(F("\n========================================"));
    Serial.println(F("  DUT Testbench — Starting test run"));
    Serial.printf ("  Total tests: %u\n", testCount_);
    Serial.println(F("========================================\n"));

    for (uint8_t i = 0; i < testCount_; i++) {
        if (display) {
            display->clearBuffer();
            display->setFont(u8g2_font_5x8_mr);
            
            char header[24];
            snprintf(header, sizeof(header), "Running %u/%u", i + 1, testCount_);
            display->drawStr(0, 12, header);
            
            display->drawStr(0, 26, "Test:");
            
            char line1[22] = {0};
            char line2[22] = {0};
            const char* tname = tests_[i].name;
            strncpy(line1, tname, 21);
            if (strlen(tname) > 21) {
                strncpy(line2, tname + 21, 21);
            }
            
            display->drawStr(0, 40, line1);
            if (line2[0] != '\0') {
                display->drawStr(0, 50, line2);
            }
            display->sendBuffer();
        }

        results_[i] = runOne(i); // safetyDisconnectAll + dutSetup handled inside
        delay(50);               // brief inter-test pause
    }

    printReport();
}

TestResult DUTTestRunner::runOne(uint8_t index) {
    if (index >= testCount_) {
        TestResult err;
        err.testName      = "INVALID INDEX";
        err.outcome       = OUTCOME_ERROR;
        err.measuredValue = 0.0f;
        err.expectedValue = 0.0f;
        err.elapsedMs     = 0;
        snprintf(err.details, sizeof(err.details), "Index %u out of range (%u tests)", index, testCount_);
        return err;
    }

    const TestCase& tc = tests_[index];
    Serial.printf("[%u/%u] Running: %s\n", index + 1, testCount_, tc.name);

    // --- Lifecycle: clean slate -> DUT sim setup -> run test ---
    safetyDisconnectAll();
    if (tc.dutSetup) {
        tc.dutSetup();
        delay(20); // allow DUT sim channels to settle
    }

    TestResult result;
    switch (tc.type) {
        case TEST_VOLTAGE_THRESHOLD:        result = runVoltageThresholdTest(tc);        break;
        case TEST_VOLTAGE_ACCURACY:         result = runVoltageAccuracyTest(tc);         break;
        case TEST_VOLTAGE_RIPPLE:           result = runVoltageRippleTest(tc);           break;
        case TEST_CURRENT_CONSUMPTION:      result = runCurrentConsumptionTest(tc);      break;
        case TEST_CURRENT_INRUSH:           result = runCurrentInrushTest(tc);           break;
        case TEST_POWER_SEQUENCE:           result = runPowerSequenceTest(tc);           break;
        case TEST_PWM_INTEGRITY:            result = runPwmIntegrityTest(tc);            break;
        case TEST_SHORT_CIRCUIT_PROTECTION: result = runShortCircuitProtectionTest(tc);  break;
        case TEST_LOAD_REGULATION:          result = runLoadRegulationTest(tc);          break;
        case TEST_CROSS_CHANNEL_ISOLATION:  result = runCrossChannelIsolationTest(tc);   break;
        case TEST_STATIC_VOLTAGE:           result = runStaticVoltageTest(tc);           break;
        default:
            result.testName      = tc.name;
            result.outcome       = OUTCOME_ERROR;
            result.measuredValue = 0.0f;
            result.expectedValue = 0.0f;
            result.elapsedMs     = 0;
            snprintf(result.details, sizeof(result.details), "Unknown TestType %u", (uint8_t)tc.type);
            break;
    }

    Serial.printf("  -> %s  (%.4f / %.4f)  %u ms  %s\n\n",
                  outcomeStr(result.outcome),
                  result.measuredValue,
                  result.expectedValue,
                  result.elapsedMs,
                  result.details);

    return result;
}

// ============================================================================
// ===========================  TEST IMPLEMENTATIONS  =========================
// ============================================================================

// ----------------------------------------------------------------------------
// 1. VOLTAGE THRESHOLD
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runVoltageThresholdTest(const TestCase& tc) {
    const VoltageThresholdParams& p = tc.voltageThreshold;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.thresholdVoltage;
    r.measuredValue = 0.0f;

    // Validate masks
    if (p.senseChannelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Sense channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    // Apply drive channels
    applyChannelMask(p.driveChannelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);

    // Poll sense channels until all cross threshold
    uint32_t t0      = millis();
    bool     allOk   = false;

    while (!allOk && (millis() - t0) < p.timeoutMs) {
        updateRealtimeDisplay(tc.name, millis() - t0, p.timeoutMs);
        allOk = true;
        for (uint8_t i = 0; i < chCount_; i++) {
            if (!(p.senseChannelMask & (1 << i))) continue;
            float v = ch_[i]->readVoltage();
            if (v < p.thresholdVoltage) {
                allOk = false;
                break;
            }
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = (float)r.elapsedMs;   // Report latency as primary value

    if (allOk) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Threshold %.3f V reached in %u ms", p.thresholdVoltage, r.elapsedMs);
    } else {
        r.outcome = OUTCOME_TIMEOUT;
        // Report the worst (lowest) voltage seen across sense channels at timeout
        float worst = 9999.0f;
        for (uint8_t i = 0; i < chCount_; i++) {
            if (!(p.senseChannelMask & (1 << i))) continue;
            float v = ch_[i]->readVoltage();
            if (v < worst) worst = v;
        }
        r.measuredValue = worst;
        snprintf(r.details, sizeof(r.details),
                 "Timeout after %u ms. Worst sense channel: %.3f V (threshold: %.3f V)",
                 r.elapsedMs, worst, p.thresholdVoltage);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 2. VOLTAGE ACCURACY
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runVoltageAccuracyTest(const TestCase& tc) {
    const VoltageAccuracyParams& p = tc.voltageAccuracy;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.targetVoltage;
    r.measuredValue = 0.0f;

    if (p.channelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t t0 = millis();
    applyChannelMask(p.channelMask, MODE_VOLTAGE_SOURCE, p.targetVoltage);
    waitAndDisplay(p.settleMs, tc.name);

    uint8_t ns = (p.numSamples == 0 || p.numSamples > DUT_MAX_SAMPLES) ? 8 : p.numSamples;

    float worstError = 0.0f;
    uint8_t worstCh  = 0;
    float   worstMeas = 0.0f;

    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.channelMask & (1 << i))) continue;
        float meas = sampleAverageVoltage(i, ns);
        float err  = fabsf(meas - p.targetVoltage);
        if (err > worstError) {
            worstError = err;
            worstCh    = i;
            worstMeas  = meas;
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = worstMeas;    // Report the actual channel voltage
    r.expectedValue = p.targetVoltage;

    if (worstError <= p.toleranceVolts) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Voltage %.4f V (target %.4f V) err %.4f V <= tol %.4f V on CH%u",
                 worstMeas, p.targetVoltage, worstError, p.toleranceVolts, worstCh + 1);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "Voltage %.4f V (target %.4f V) err %.4f V > tol %.4f V on CH%u",
                 worstMeas, p.targetVoltage, worstError, p.toleranceVolts, worstCh + 1);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 3. VOLTAGE RIPPLE / NOISE
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runVoltageRippleTest(const TestCase& tc) {
    const VoltageRippleParams& p = tc.voltageRipple;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.maxRippleVolts;
    r.measuredValue = 0.0f;

    if (p.channelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint8_t ns = (p.numSamples == 0 || p.numSamples > DUT_MAX_SAMPLES) ? DUT_MAX_SAMPLES : p.numSamples;
    uint32_t intervalMs = p.windowMs / ns;
    if (intervalMs == 0) intervalMs = 1;

    applyChannelMask(p.channelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);
    waitAndDisplay(100, tc.name); // initial settle

    uint32_t t0 = millis();

    float worstPtP = 0.0f;
    uint8_t worstCh = 0;

    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.channelMask & (1 << i))) continue;

        float vmin = 1e9f, vmax = -1e9f;
        
        for (uint8_t s = 0; s < ns; s++) {
            float v = ch_[i]->readVoltage();
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
            delay(intervalMs);
        }

        float ptP = vmax - vmin;
        if (ptP > worstPtP) {
            worstPtP = ptP;
            worstCh  = i;
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = worstPtP;

    if (worstPtP <= p.maxRippleVolts) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Worst ripple %.4f V on CH%u (limit %.4f V)",
                 worstPtP, worstCh + 1, p.maxRippleVolts);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "Ripple %.4f V > limit %.4f V on CH%u",
                 worstPtP, p.maxRippleVolts, worstCh + 1);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 4. CURRENT CONSUMPTION
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runCurrentConsumptionTest(const TestCase& tc) {
    const CurrentConsumptionParams& p = tc.currentConsumption;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.maxCurrentA;
    r.measuredValue = 0.0f;

    if (p.channelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t t0 = millis();
    applyChannelMask(p.channelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);
    waitAndDisplay(p.settleMs, tc.name);

    uint8_t ns = (p.numSamples == 0 || p.numSamples > DUT_MAX_SAMPLES) ? 8 : p.numSamples;

    float worstHigh = 0.0f;
    float worstLow  = 1e9f;
    uint8_t worstCh = 0;
    float   worstMeas = 0.0f;

    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.channelMask & (1 << i))) continue;
        float meas = sampleAverageCurrent(i, ns);
        if (meas > worstHigh) { worstHigh = meas; worstMeas = meas; worstCh = i; }
        if (meas < worstLow)  { worstLow  = meas; }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = worstMeas;

    bool inRange = (worstHigh <= p.maxCurrentA) && (worstLow >= p.minCurrentA);

    if (inRange) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Current range [%.4f, %.4f] A within [%.4f, %.4f] A",
                 worstLow, worstHigh, p.minCurrentA, p.maxCurrentA);
    } else {
        r.outcome = OUTCOME_FAIL;
        if (worstHigh > p.maxCurrentA) {
            snprintf(r.details, sizeof(r.details),
                     "Overcurrent: CH%u drew %.5f A (max %.5f A)",
                     worstCh + 1, worstHigh, p.maxCurrentA);
        } else {
            snprintf(r.details, sizeof(r.details),
                     "Undercurrent: min %.5f A < limit %.5f A",
                     worstLow, p.minCurrentA);
        }
    }

    return r;
}

// ----------------------------------------------------------------------------
// 5. CURRENT INRUSH
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runCurrentInrushTest(const TestCase& tc) {
    const CurrentInrushParams& p = tc.currentInrush;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.maxInrushA;
    r.measuredValue = 0.0f;

    if (p.channelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t sampleInterval = (p.sampleIntervalMs < 1) ? 1 : p.sampleIntervalMs;

    // Enable channels – capture current profile from t=0
    applyChannelMask(p.channelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);

    float    peakCurrent  = 0.0f;
    uint32_t peakTimeMs   = 0;
    uint32_t settleTimeMs = 0;
    bool     hasSettled   = false;
    float    steadyState  = 0.0f;

    // First rough steady-state estimate (after full window, if we can)
    // We do a single pass collecting samples
    float    samples[DUT_MAX_SAMPLES];
    uint8_t  sampleCount = 0;
    uint32_t t0          = millis();

    // Use the first channel in the mask as representative
    uint8_t repCh = 0;
    for (uint8_t i = 0; i < chCount_; i++) {
        if (p.channelMask & (1 << i)) { repCh = i; break; }
    }

    while ((millis() - t0) < p.windowMs && sampleCount < DUT_MAX_SAMPLES) {
        updateRealtimeDisplay(tc.name, millis() - t0, p.windowMs);
        float c = ch_[repCh]->readCurrent();
        samples[sampleCount] = c;
        if (fabsf(c) > fabsf(peakCurrent)) {
            peakCurrent = c;
            peakTimeMs  = (uint32_t)(millis() - t0);
        }
        sampleCount++;
        delay(sampleInterval);
    }

    r.elapsedMs = (uint32_t)(millis() - t0);

    // Estimate steady state as average of last quarter of samples
    uint8_t ssStart = (sampleCount * 3) / 4;
    float   ssSum   = 0.0f;
    for (uint8_t i = ssStart; i < sampleCount; i++) ssSum += samples[i];
    steadyState = (sampleCount > ssStart) ? (ssSum / (sampleCount - ssStart)) : peakCurrent;

    // Find settle time (first sample within settleThreshold of steady state)
    for (uint8_t i = 0; i < sampleCount; i++) {
        if (fabsf(samples[i] - steadyState) <= p.settleThresholdA) {
            settleTimeMs = i * sampleInterval;
            hasSettled   = true;
            break;
        }
    }

    r.measuredValue = peakCurrent;

    if (fabsf(peakCurrent) <= p.maxInrushA) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Peak %.4f A at %u ms. Steady %.4f A. Settle %s (%u ms).",
                 peakCurrent, peakTimeMs, steadyState,
                 hasSettled ? "OK" : "not within window", settleTimeMs);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "Inrush %.4f A > max %.4f A at %u ms. Steady %.4f A.",
                 peakCurrent, p.maxInrushA, peakTimeMs, steadyState);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 6. POWER SEQUENCING
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runPowerSequenceTest(const TestCase& tc) {
    const PowerSequenceParams& p = tc.powerSequence;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = (float)p.stepCount;
    r.measuredValue = 0.0f;

    uint8_t stepsPassed = 0;
    char    failDetail[48] = "";
    uint32_t t0 = millis();

    for (uint8_t s = 0; s < p.stepCount && s < 8; s++) {
        applyChannelMask(p.stepChannelMask[s], MODE_VOLTAGE_SOURCE, p.stepTargetVoltage[s]);
        waitAndDisplay(p.stepDelayMs[s], tc.name);

        // Verify all channels in this step's mask are within tolerance
        bool stepOk = true;
        for (uint8_t i = 0; i < chCount_; i++) {
            if (!(p.stepChannelMask[s] & (1 << i))) continue;
            float v = sampleAverageVoltage(i, 4);
            if (fabsf(v - p.stepTargetVoltage[s]) > p.stepToleranceVolts) {
                snprintf(failDetail, sizeof(failDetail),
                         "Step %u CH%u: %.3f V (exp %.3f V)",
                         s + 1, i + 1, v, p.stepTargetVoltage[s]);
                stepOk = false;
                break;
            }
        }

        if (stepOk) {
            stepsPassed++;
        } else {
            break; // Stop on first failed step (sequencing failure)
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = (float)stepsPassed;

    if (stepsPassed == p.stepCount) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "All %u steps passed", p.stepCount);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "%u/%u steps passed. %s", stepsPassed, p.stepCount, failDetail);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 7. PWM INTEGRITY
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runPwmIntegrityTest(const TestCase& tc) {
    const PwmIntegrityParams& p = tc.pwmIntegrity;

    TestResult r;
    r.testName = tc.name;

    // Expected DC level = driveVoltage * (dutyCycle / 255)
    float expectedDC = p.driveVoltage * ((float)p.dutyCycle / 255.0f);
    r.expectedValue  = expectedDC;
    r.measuredValue  = 0.0f;

    if (p.driveChannelMask == 0 || p.senseChannelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Drive or sense channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t t0 = millis();

    // Set drive channels to PWM mode and apply duty cycle
    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.driveChannelMask & (1 << i))) continue;
        ch_[i]->resetStatus();
        ch_[i]->setOutputVoltage(p.driveVoltage);
        ch_[i]->setMode(MODE_PWM_GENERATOR);
        ch_[i]->setPwm(p.dutyCycle, p.frequency);
    }

    waitAndDisplay(p.settleMs, tc.name);

    // Measure average DC on sense channels
    float worstError = 0.0f;
    float worstMeas  = 0.0f;
    uint8_t worstCh  = 0;

    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.senseChannelMask & (1 << i))) continue;
        float meas = sampleAverageVoltage(i, 16);
        float err  = fabsf(meas - expectedDC);
        if (err > worstError) {
            worstError = err;
            worstMeas  = meas;
            worstCh    = i;
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = worstMeas;

    if (worstError <= p.toleranceVolts) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "DC %.4f V (exp %.4f V) err %.4f V on CH%u",
                 worstMeas, expectedDC, worstError, worstCh + 1);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "DC %.4f V (exp %.4f V) err %.4f V > tol %.4f V on CH%u",
                 worstMeas, expectedDC, worstError, p.toleranceVolts, worstCh + 1);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 8. SHORT-CIRCUIT PROTECTION
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runShortCircuitProtectionTest(const TestCase& tc) {
    const ShortCircuitProtectionParams& p = tc.shortCircuit;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = (float)p.timeoutMs;
    r.measuredValue = 0.0f;

    if (p.channelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    // Apply reduced current limit to trigger protection quickly
    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.channelMask & (1 << i))) continue;
        ch_[i]->resetStatus();
        ch_[i]->setLimits(p.driveVoltage + 1.0f, p.shortCurrentLimitA);
    }

    applyChannelMask(p.channelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);

    uint32_t t0       = millis();
    bool     faultFired = false;
    uint32_t faultMs    = 0;

    while ((millis() - t0) < p.timeoutMs) {
        updateRealtimeDisplay(tc.name, millis() - t0, p.timeoutMs);
        // Drive update loop to let channels detect over-current
        for (uint8_t i = 0; i < chCount_; i++) {
            if (!(p.channelMask & (1 << i))) continue;
            ch_[i]->update();
            if (ch_[i]->getStatus() == STATUS_FAIL_OVERCURRENT) {
                faultFired = true;
                faultMs    = (uint32_t)(millis() - t0);
                break;
            }
        }
        if (faultFired) break;
        delay(1);
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = faultFired ? (float)faultMs : 0.0f;

    // Reset channels after test
    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.channelMask & (1 << i))) continue;
        ch_[i]->resetStatus();
    }

    if (faultFired) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "OC fault fired in %u ms (limit %.3f A)",
                 faultMs, p.shortCurrentLimitA);
    } else {
        r.outcome = OUTCOME_TIMEOUT;
        snprintf(r.details, sizeof(r.details),
                 "No OC fault within %u ms (limit %.3f A)",
                 p.timeoutMs, p.shortCurrentLimitA);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 9. LOAD REGULATION
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runLoadRegulationTest(const TestCase& tc) {
    const LoadRegulationParams& p = tc.loadRegulation;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.maxDropVolts;
    r.measuredValue = 0.0f;

    if (p.driveChannelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Drive channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t t0 = millis();

    // --- Phase 1: No-load — drive channels only, load channels at HIGH_Z ---
    applyChannelMask(p.driveChannelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);
    waitAndDisplay(p.settleMs, tc.name);

    float   vNoLoad  = 0.0f;
    uint8_t nDrive   = 0;
    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.driveChannelMask & (1 << i))) continue;
        vNoLoad += sampleAverageVoltage(i, 8);
        nDrive++;
    }
    if (nDrive > 0) vNoLoad /= nDrive;

    // --- Phase 2: Loaded — bring in load channels at simulated sink voltage ---
    // Physics: with equal shunts (R) on each channel, to sink I_load from the
    // drive channel, the load channel must drive:
    //   V_load = V_drive - 2 * I_load * R_shunt
    // This creates a current of I_load through each channel's own shunt.
    if (p.loadChannelMask != 0) {
        const float SHUNT_R = 10.5f;
        float simVoltage = p.driveVoltage - 2.0f * p.loadCurrentA * SHUNT_R;
        if (simVoltage < 0.0f) simVoltage = 0.0f;
        applyChannelMask(p.loadChannelMask, MODE_VOLTAGE_SOURCE, simVoltage);
        delay(p.settleMs);
    }

    // Measure V_loaded on drive channels (load channels excluded)
    float   vLoaded = 0.0f;
    uint8_t nLoad   = 0;
    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.driveChannelMask & (1 << i))) continue;
        vLoaded += sampleAverageVoltage(i, 8);
        nLoad++;
    }
    if (nLoad > 0) vLoaded /= nLoad;

    float drop = vNoLoad - vLoaded;

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = drop;

    if (drop <= p.maxDropVolts) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Drop %.4f V (no-load %.4f V, loaded %.4f V) <= max %.4f V",
                 drop, vNoLoad, vLoaded, p.maxDropVolts);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "Drop %.4f V (no-load %.4f V, loaded %.4f V) > max %.4f V",
                 drop, vNoLoad, vLoaded, p.maxDropVolts);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 10. CROSS-CHANNEL ISOLATION
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runCrossChannelIsolationTest(const TestCase& tc) {
    const CrossChannelIsolationParams& p = tc.crossChannelIsolation;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.maxCouplingVolts;
    r.measuredValue = 0.0f;

    if (p.driveChannelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Drive channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t t0 = millis();
    applyChannelMask(p.driveChannelMask, MODE_VOLTAGE_SOURCE, p.driveVoltage);
    waitAndDisplay(p.settleMs, tc.name);

    // If senseChannelMask == 0, check every channel NOT in driveChannelMask.
    // Otherwise check exactly the channels in senseChannelMask.
    uint8_t effectiveSense = p.senseChannelMask
                             ? p.senseChannelMask
                             : (uint8_t)(~p.driveChannelMask);

    float   worstCoupling = 0.0f;
    uint8_t worstCh       = 0;

    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(effectiveSense & (1 << i))) continue;
        float v = sampleAverageVoltage(i, 8);
        if (fabsf(v) > worstCoupling) {
            worstCoupling = fabsf(v);
            worstCh       = i;
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = worstCoupling;

    if (worstCoupling <= p.maxCouplingVolts) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Worst coupling %.4f V on CH%u (limit %.4f V)",
                 worstCoupling, worstCh + 1, p.maxCouplingVolts);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "Coupling %.4f V on CH%u > limit %.4f V",
                 worstCoupling, worstCh + 1, p.maxCouplingVolts);
    }

    return r;
}

// ----------------------------------------------------------------------------
// 11. STATIC VOLTAGE
// ----------------------------------------------------------------------------
TestResult DUTTestRunner::runStaticVoltageTest(const TestCase& tc) {
    const StaticVoltageParams& p = tc.staticVoltage;

    TestResult r;
    r.testName      = tc.name;
    r.expectedValue = p.expectedVoltage;
    r.measuredValue = 0.0f;

    if (p.senseChannelMask == 0) {
        r.outcome = OUTCOME_ERROR;
        snprintf(r.details, sizeof(r.details), "Sense channel mask is 0");
        r.elapsedMs = 0;
        return r;
    }

    uint32_t t0 = millis();
    waitAndDisplay(p.settleMs, tc.name);

    float worstError = 0.0f;
    uint8_t worstCh  = 0;
    float   worstMeas = 0.0f;

    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(p.senseChannelMask & (1 << i))) continue;
        float meas = sampleAverageVoltage(i, 8);
        float err  = fabsf(meas - p.expectedVoltage);
        if (err > worstError) {
            worstError = err;
            worstCh    = i;
            worstMeas  = meas;
        }
    }

    r.elapsedMs     = (uint32_t)(millis() - t0);
    r.measuredValue = worstMeas;
    r.expectedValue = p.expectedVoltage;

    if (worstError <= p.toleranceVolts) {
        r.outcome = OUTCOME_PASS;
        snprintf(r.details, sizeof(r.details),
                 "Voltage %.4f V (target %.4f V) err %.4f V <= tol %.4f V on CH%u",
                 worstMeas, p.expectedVoltage, worstError, p.toleranceVolts, worstCh + 1);
    } else {
        r.outcome = OUTCOME_FAIL;
        snprintf(r.details, sizeof(r.details),
                 "Voltage %.4f V (target %.4f V) err %.4f V > tol %.4f V on CH%u",
                 worstMeas, p.expectedVoltage, worstError, p.toleranceVolts, worstCh + 1);
    }

    return r;
}

// ============================================================================
// ================================  REPORTING  ================================
// ============================================================================

void DUTTestRunner::printReport() const {
    Serial.println(F("\n========================================"));
    Serial.println(F("           TEST REPORT"));
    Serial.println(F("========================================"));

    uint8_t passed = 0, failed = 0, timeouts = 0, errors = 0;

    for (uint8_t i = 0; i < testCount_; i++) {
        const TestResult& r = results_[i];
        const char* outcome = outcomeStr(r.outcome);

        Serial.printf("[%2u] %-40s  %s\n", i + 1, r.testName, outcome);
        Serial.printf("       Measured: %.5f  Expected: %.5f  Time: %u ms\n",
                      r.measuredValue, r.expectedValue, r.elapsedMs);
        Serial.printf("       %s\n", r.details);

        switch (r.outcome) {
            case OUTCOME_PASS:    passed++;   break;
            case OUTCOME_FAIL:    failed++;   break;
            case OUTCOME_TIMEOUT: timeouts++; break;
            default:              errors++;   break;
        }
    }

    Serial.println(F("----------------------------------------"));
    Serial.printf("  PASS: %u  FAIL: %u  TIMEOUT: %u  ERROR: %u\n",
                  passed, failed, timeouts, errors);
    Serial.printf("  TOTAL: %u/%u passed\n", passed, testCount_);
    Serial.println(F("========================================\n"));
}

void DUTTestRunner::printSummary() const {
    uint8_t passed = 0;
    for (uint8_t i = 0; i < testCount_; i++) {
        if (results_[i].outcome == OUTCOME_PASS) passed++;
    }

    Serial.printf("=== Summary: %u/%u PASS ===\n", passed, testCount_);
    for (uint8_t i = 0; i < testCount_; i++) {
        Serial.printf("  [%s] %s\n", outcomeStr(results_[i].outcome), results_[i].testName);
    }
}

void DUTTestRunner::displaySummary(U8G2* display) const {
    if (!display) return;

    uint8_t passed = 0;
    for (uint8_t i = 0; i < testCount_; i++) {
        if (results_[i].outcome == OUTCOME_PASS) passed++;
    }

    display->clearBuffer();
    display->setFont(u8g2_font_5x8_mr);

    char header[24];
    snprintf(header, sizeof(header), "PASS %u/%u", passed, testCount_);
    display->drawStr(0, 8, header);

    // Render up to 6 rows of test results (limited by OLED height)
    uint8_t maxRows = 6;
    uint8_t startIdx = (testCount_ > maxRows) ? (testCount_ - maxRows) : 0;

    for (uint8_t i = startIdx; i < testCount_; i++) {
        uint8_t row = i - startIdx + 2; // rows 2..7
        const TestResult& r = results_[i];

        char line[22];
        const char* symbol = (r.outcome == OUTCOME_PASS)    ? "[P]" :
                             (r.outcome == OUTCOME_FAIL)    ? "[F]" :
                             (r.outcome == OUTCOME_TIMEOUT) ? "[T]" : "[E]";

        // Truncate test name to fit: 22 - 4 (symbol) - 1 (space) = 17 chars
        snprintf(line, sizeof(line), "%s %.16s", symbol, r.testName);
        display->drawStr(0, row * 8, line);
    }

    display->sendBuffer();
}

// ============================================================================
// ================================  HELPERS  ==================================
// ============================================================================

void DUTTestRunner::safetyDisconnectAll() {
    for (uint8_t i = 0; i < chCount_; i++) {
        if (ch_[i]) {
            ch_[i]->setMode(MODE_HIGH_IMPEDANCE);
            ch_[i]->setOutputVoltage(0.0f);
        }
    }
}

void DUTTestRunner::applyChannelMask(uint8_t mask, LVLPMode mode, float voltage) {
    for (uint8_t i = 0; i < chCount_; i++) {
        if (!(mask & (1 << i))) continue;
        if (!ch_[i]) continue;
        ch_[i]->resetStatus();
        ch_[i]->setOutputVoltage(voltage);
        ch_[i]->setMode(mode);
    }
}

float DUTTestRunner::sampleAverageVoltage(uint8_t chIdx, uint8_t numSamples) {
    if (!ch_[chIdx] || numSamples == 0) return 0.0f;
    float sum = 0.0f;
    for (uint8_t s = 0; s < numSamples; s++) {
        sum += ch_[chIdx]->readVoltage();
        if (s < numSamples - 1) delay(2);
    }
    return sum / numSamples;
}

float DUTTestRunner::sampleAverageCurrent(uint8_t chIdx, uint8_t numSamples) {
    if (!ch_[chIdx] || numSamples == 0) return 0.0f;
    float sum = 0.0f;
    for (uint8_t s = 0; s < numSamples; s++) {
        sum += ch_[chIdx]->readCurrent();
        if (s < numSamples - 1) delay(2);
    }
    return sum / numSamples;
}

void DUTTestRunner::updateRealtimeDisplay(const char* testName, uint32_t elapsedMs, uint32_t totalMs) {
    if (!display_) return;

    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate < 100 && elapsedMs != totalMs) return;
    lastUpdate = millis();

    display_->clearBuffer();
    display_->setFont(u8g2_font_5x8_mr);

    char line[24];
    strncpy(line, testName, 21);
    line[21] = '\0';
    display_->drawStr(0, 8, line);

    if (totalMs > 0) {
        snprintf(line, sizeof(line), "Wait: %lu/%lu ms", elapsedMs, totalMs);
    } else {
        snprintf(line, sizeof(line), "Time: %lu ms", elapsedMs);
    }
    display_->drawStr(0, 18, line);

    for (uint8_t i = 0; i < 4 && i < chCount_; i++) {
        if (ch_[i]) {
            float v = ch_[i]->readVoltage();
            snprintf(line, sizeof(line), "CH%u:%4.1fV", i + 1, v);
            uint8_t x = (i % 2 == 0) ? 0 : 64;
            uint8_t y = 32 + (i / 2) * 12;
            display_->drawStr(x, y, line);
        }
    }
    display_->sendBuffer();
}

void DUTTestRunner::waitAndDisplay(uint32_t waitMs, const char* testName) {
    uint32_t start = millis();
    while (millis() - start < waitMs) {
        updateRealtimeDisplay(testName, millis() - start, waitMs);
        delay(10);
    }
    updateRealtimeDisplay(testName, waitMs, waitMs);
}

const char* DUTTestRunner::outcomeStr(TestOutcome o) {
    switch (o) {
        case OUTCOME_PASS:    return "PASS";
        case OUTCOME_FAIL:    return "FAIL";
        case OUTCOME_TIMEOUT: return "TIMEOUT";
        case OUTCOME_SKIP:    return "SKIP";
        default:              return "ERROR";
    }
}
