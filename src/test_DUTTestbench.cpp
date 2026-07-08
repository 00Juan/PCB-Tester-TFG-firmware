/**
 * @file test_DUTTestbench.cpp
 * @brief Generic DUT testbench entry point.
 *
 * HOW TO USE
 * ----------
 * 1. Below the "USER: Add test cases here" section, call runner.addTest(tc)
 *    once per test.  Fill in the relevant parameter struct for the test type
 *    you want (see DUTTestbench.h for all available types and their fields).
 *
 * 2. Select the correct PlatformIO environment:
 *       pio run -e test_DUTTestbench --target upload
 *
 * 3. Open the Serial Monitor at 115200 baud to see the live test log and the
 *    final report.  The OLED will show a compact pass/fail summary.
 *
 * CHANNEL MASK REFERENCE
 * ----------------------
 *   Bit 0 (0x01) = CH1     Bit 4 (0x10) = CH5
 *   Bit 1 (0x02) = CH2     Bit 5 (0x20) = CH6
 *   Bit 2 (0x04) = CH3     Bit 6 (0x40) = CH7
 *   Bit 3 (0x08) = CH4     Bit 7 (0x80) = CH8
 *
 * All channels:  0xFF
 * CH1 only:      0x01
 * CH1 + CH2:     0x03
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"
#include "DUTTestbench.h"

// ============================================================================
// TESTBENCH INSTANCE
// ============================================================================

DUTTestRunner runner;

// ============================================================================
// HELPER: BUILD CHANNEL POINTER ARRAY
// ============================================================================

static LVLPChannel* chPtrs[8];

static void buildChannelPointerArray() {
    for (uint8_t i = 0; i < 8; i++) {
        chPtrs[i] = &lvlpChannels[i];
    }
}

// ============================================================================
// USER: Add test cases here
// ============================================================================

static void configureTests() {
    // -----------------------------------------------------------------------
    // EXAMPLE 1 — Voltage Threshold
    // Drive CH1 to 3.3 V. Expect CH2 to cross 3.0 V within 100 ms.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "VCC_3V3 power-on latency (CH1->CH2)";
        tc.type = TEST_VOLTAGE_THRESHOLD;
        tc.voltageThreshold = {
            .driveChannelMask = 0x01,   // CH1
            .driveVoltage     = 3.3f,
            .senseChannelMask = 0x02,   // CH2
            .thresholdVoltage = 3.0f,
            .timeoutMs        = 100
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 2 — Voltage Accuracy
    // Drive CH1 to 5.0 V, verify it settles within ±100 mV after 200 ms.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH1 voltage accuracy at 5.0 V";
        tc.type = TEST_VOLTAGE_ACCURACY;
        tc.voltageAccuracy = {
            .channelMask    = 0x01,     // CH1
            .targetVoltage  = 5.0f,
            .toleranceVolts = 0.1f,     // ±100 mV
            .settleMs       = 200,
            .numSamples     = 8
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 3 — Current Consumption
    // Drive CH1 to 3.3 V, expect steady-state current 1 mA – 50 mA.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH1 idle current @ 3.3 V";
        tc.type = TEST_CURRENT_CONSUMPTION;
        tc.currentConsumption = {
            .channelMask  = 0x01,       // CH1
            .driveVoltage = 3.3f,
            .minCurrentA  = 0.001f,     // 1 mA
            .maxCurrentA  = 0.050f,     // 50 mA
            .settleMs     = 150,
            .numSamples   = 8
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 4 — Voltage Ripple
    // Drive CH2 to 5 V, measure ripple over 100 ms (32 samples).
    // Fail if peak-to-peak > 200 mV.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH2 output ripple @ 5 V";
        tc.type = TEST_VOLTAGE_RIPPLE;
        tc.voltageRipple = {
            .channelMask   = 0x02,      // CH2
            .driveVoltage  = 5.0f,
            .windowMs      = 100,
            .numSamples    = 32,
            .maxRippleVolts = 0.2f      // 200 mV
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 5 — Current Inrush
    // Apply 5 V to CH3, capture 50 samples over 500 ms (10 ms each).
    // Fail if peak > 200 mA.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH3 inrush current @ 5 V";
        tc.type = TEST_CURRENT_INRUSH;
        tc.currentInrush = {
            .channelMask      = 0x04,   // CH3
            .driveVoltage     = 5.0f,
            .windowMs         = 500,
            .sampleIntervalMs = 10,
            .maxInrushA       = 0.2f,   // 200 mA
            .settleThresholdA = 0.005f  // 5 mA settle band
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 6 — Power Sequencing (2 steps)
    // Step 1: enable CH1 @ 1.8 V, wait 50 ms, verify.
    // Step 2: enable CH2 @ 3.3 V, wait 100 ms, verify.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH1->CH2 power-on sequence";
        tc.type = TEST_POWER_SEQUENCE;
        tc.powerSequence.stepCount            = 2;
        tc.powerSequence.stepChannelMask[0]   = 0x01;   // CH1
        tc.powerSequence.stepTargetVoltage[0] = 1.8f;
        tc.powerSequence.stepDelayMs[0]       = 50;
        tc.powerSequence.stepChannelMask[1]   = 0x02;   // CH2
        tc.powerSequence.stepTargetVoltage[1] = 3.3f;
        tc.powerSequence.stepDelayMs[1]       = 100;
        tc.powerSequence.stepToleranceVolts   = 0.15f;  // ±150 mV
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 7 — PWM Integrity (CH1 drive, CH2 sense)
    // Drive CH1 with 50% duty cycle @ 1 kHz. Expect ~1.65 V DC on CH2.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH1 PWM 50% @ 1kHz -> CH2 DC";
        tc.type = TEST_PWM_INTEGRITY;
        tc.pwmIntegrity = {
            .driveChannelMask = 0x01,   // CH1 (PWM-capable)
            .senseChannelMask = 0x02,   // CH2
            .driveVoltage     = 3.3f,
            .dutyCycle        = 128,    // ~50%
            .frequency        = 1000,   // 1 kHz
            .settleMs         = 200,
            .toleranceVolts   = 0.2f
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 8 — Short-Circuit Protection
    // Force CH1 into OC condition (limit 5 mA). Expect fault in 200 ms.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "CH1 over-current protection";
        tc.type = TEST_SHORT_CIRCUIT_PROTECTION;
        tc.shortCircuit = {
            .channelMask        = 0x01, // CH1
            .driveVoltage       = 5.0f,
            .shortCurrentLimitA = 0.005f, // 5 mA — very low → triggers on normal DUT draw
            .timeoutMs          = 200
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 9 — Load Regulation
    // Drive CH1 @ 5 V with CH5 as load, expect voltage drop < 200 mV.
    // Set loadChannelMask = 0 when no load channel is wired (manual bench).
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 load regulation 5V/20mA";
        tc.type     = TEST_LOAD_REGULATION;
        tc.dutSetup = nullptr;
        tc.loadRegulation = {
            .driveChannelMask = 0x01,   // CH1 drives
            .loadChannelMask  = 0x00,   // 0 = no load channel (manual bench test)
            .driveVoltage     = 5.0f,
            .loadCurrentA     = 0.020f, // 20 mA (only used when loadChannelMask != 0)
            .settleMs         = 200,
            .maxDropVolts     = 0.2f    // 200 mV
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // EXAMPLE 10 — Cross-Channel Isolation
    // Drive CH1 @ 5 V. Channels CH2, CH3, CH4 must stay below 100 mV.
    // senseChannelMask = 0 checks all non-drive channels automatically.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 drive - neighbour isolation";
        tc.type     = TEST_CROSS_CHANNEL_ISOLATION;
        tc.dutSetup = nullptr;
        tc.crossChannelIsolation = {
            .driveChannelMask = 0x01,   // CH1
            .senseChannelMask = 0x00,   // 0 = auto (all non-drive channels)
            .driveVoltage     = 5.0f,
            .settleMs         = 100,
            .maxCouplingVolts = 0.1f    // 100 mV
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // Add your own tests below this line
    // -----------------------------------------------------------------------
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println(F("\n=== DUT Testbench Initializing ==="));

    // Hardware peripherals
    if (!initializeMCP4728()) {
        Serial.println(F("ERROR: MCP4728 DAC init failed!"));
    }
    if (!initializeMCP3208()) {
        Serial.println(F("ERROR: MCP3208 ADC init failed!"));
    }
    initializeSSD1309();
    initializeWS2812B();
    initializeEncoder(-100, 100, true);

    // Shift-register output enable
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllHigh();

    // Initialise LVLP channels
    for (uint8_t i = 0; i < 8; i++) {
        lvlpChannels[i].init();
        lvlpChannels[i].setLimits(12.0f, 0.5f); // Global safety limits
    }

    // Build testbench
    buildChannelPointerArray();
    runner.begin(chPtrs, 8);
    configureTests();

    Serial.println(F("=== Running tests ===\n"));
    runner.runAll();

    // Display summary on OLED
    runner.displaySummary(&u8g2);
}

// ============================================================================
// LOOP — idle after test run; press encoder to re-display summary
// ============================================================================

void loop() {
    bool buttonPressed = !digitalRead(pinEncoderSw);
    if (buttonPressed) {
        runner.displaySummary(&u8g2);
        delay(300); // debounce
    }
}
