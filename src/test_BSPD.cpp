/**
 * @file test_BSPD.cpp
 * @brief Testbench for BSPD (Brake System Plausibility Device).
 *
 * BSPD logic tested:
 * - Analog inputs: Hall sensor (HS), Brake sensor (BS)
 * - Outputs: SDC_OUT (SDC)
 *
 * Rules:
 * - SDC is 12V if both HS and BS are < threshold.
 * - SDC is 0V if HS or BS > threshold for > 500ms.
 * - SDC returns to 12V if HS and BS < threshold for > 10s.
 *
 * Channel Mapping:
 * - CH1 (0x01): HS (Hall Sensor)
 * - CH2 (0x02): BS (Brake Sensor)
 * - CH3 (0x04): SDC (Output from BSPD)
 *
 * Test Strategy:
 * - We use `dutSetup` to configure HS and BS dynamically.
 * - We use `TEST_STATIC_VOLTAGE` to passively check the static state of the SDC.
 * - We use `TEST_VOLTAGE_THRESHOLD` to check SDC recovering to 12V within 11s.
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"
#include "DUTTestbench.h"

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

static constexpr float HS_NORMAL_V = 1.0f;
static constexpr float HS_FAULT_V  = 4.0f;
static constexpr float BS_NORMAL_V = 1.0f;
static constexpr float BS_FAULT_V  = 4.0f;

static constexpr uint8_t MASK_HS    = 0x01; // CH1
static constexpr uint8_t MASK_BS    = 0x02; // CH2
static constexpr uint8_t MASK_SDC   = 0x04; // CH3

// ============================================================================
// TESTBENCH INSTANCE
// ============================================================================

DUTTestRunner runner;
static LVLPChannel* chPtrs[8];

static void buildChannelPointerArray() {
    for (uint8_t i = 0; i < 8; i++) {
        chPtrs[i] = &lvlpChannels[i];
    }
}

// ============================================================================
// DUT SETUP CALLBACKS
// ============================================================================

static void setupNormal() {
    lvlpChannels[0].setMode(MODE_VOLTAGE_SOURCE);
    lvlpChannels[0].setOutputVoltage(HS_NORMAL_V);
    lvlpChannels[1].setMode(MODE_VOLTAGE_SOURCE);
    lvlpChannels[1].setOutputVoltage(BS_NORMAL_V);
    lvlpChannels[0].update();
    lvlpChannels[1].update();
}

static void setupHSFaultShort() {
    // 1. Guarantee normal state first
    setupNormal();
    delay(100);

    // 2. Trigger fault
    lvlpChannels[0].setOutputVoltage(HS_FAULT_V);
    lvlpChannels[0].update();
    // Test starts ~20ms after this returns.
}

static void setupHSFaultLong() {
    setupNormal();
    delay(100);
    lvlpChannels[0].setOutputVoltage(HS_FAULT_V);
    lvlpChannels[0].update();
}

static void setupBSFaultLong() {
    setupNormal();
    delay(100);
    lvlpChannels[1].setOutputVoltage(BS_FAULT_V);
    lvlpChannels[1].update();
}

static void setupRecovery() {
    setupNormal();
    delay(100);

    // Trigger fault and hold for 600ms to guarantee SDC drops to 0V
    lvlpChannels[0].setOutputVoltage(HS_FAULT_V);
    lvlpChannels[0].update();
    delay(600);

    // Return to normal - 10s timer starts now
    lvlpChannels[0].setOutputVoltage(HS_NORMAL_V);
    lvlpChannels[0].update();
}

// ============================================================================
// USER: Add test cases here
// ============================================================================

static void configureTests() {
    // -----------------------------------------------------------------------
    // TEST 1: Normal condition, SDC is 12V
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "BSPD Normal: SDC is 12V";
        tc.type = TEST_STATIC_VOLTAGE;
        tc.dutSetup = setupNormal;
        tc.staticVoltage = {
            .senseChannelMask = MASK_SDC,
            .expectedVoltage  = 12.0f,
            .toleranceVolts   = 2.0f,     // SDC > 10V is fine
            .settleMs         = 100
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 2: HS Fault < 500ms, SDC is still 12V
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "BSPD HS Fault < 500ms: SDC still 12V";
        tc.type = TEST_STATIC_VOLTAGE;
        tc.dutSetup = setupHSFaultShort;
        tc.staticVoltage = {
            .senseChannelMask = MASK_SDC,
            .expectedVoltage  = 12.0f,
            .toleranceVolts   = 2.0f,
            .settleMs         = 380       // Total fault time: 20ms delay + 380ms = 400ms
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 3: HS Fault > 500ms, SDC is 0V
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "BSPD HS Fault > 500ms: SDC is 0V";
        tc.type = TEST_STATIC_VOLTAGE;
        tc.dutSetup = setupHSFaultLong;
        tc.staticVoltage = {
            .senseChannelMask = MASK_SDC,
            .expectedVoltage  = 0.0f,
            .toleranceVolts   = 1.0f,     // Expect SDC < 1.0V
            .settleMs         = 580       // Total fault time: 20ms delay + 580ms = 600ms
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 4: BS Fault > 500ms, SDC is 0V
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "BSPD BS Fault > 500ms: SDC is 0V";
        tc.type = TEST_STATIC_VOLTAGE;
        tc.dutSetup = setupBSFaultLong;
        tc.staticVoltage = {
            .senseChannelMask = MASK_SDC,
            .expectedVoltage  = 0.0f,
            .toleranceVolts   = 1.0f,
            .settleMs         = 580
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 5: Recovery > 10s, SDC goes to 12V
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name = "BSPD Recovery > 10s: SDC goes to 12V";
        tc.type = TEST_VOLTAGE_THRESHOLD;
        tc.dutSetup = setupRecovery;
        tc.voltageThreshold = {
            .driveChannelMask = 0x00,     // Passive
            .driveVoltage     = 0.0f,
            .senseChannelMask = MASK_SDC,
            .thresholdVoltage = 10.0f,
            .timeoutMs        = 11000     // 11 seconds to recover
        };
        // NOTE: This test will pass if SDC goes > 10V *any* time before 11s.
        // The latency measured will appear in the test report, allowing
        // manual verification that it took approximately 10 seconds.
        runner.addTest(tc);
    }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println(F("\n=== BSPD Testbench Initializing ==="));

    if (!initializeMCP4728()) {
        Serial.println(F("ERROR: MCP4728 DAC init failed!"));
    }
    if (!initializeMCP3208()) {
        Serial.println(F("ERROR: MCP3208 ADC init failed!"));
    }
    initializeSSD1309();
    initializeWS2812B();
    initializeEncoder(-100, 100, true);

    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllHigh();

    for (uint8_t i = 0; i < 8; i++) {
        lvlpChannels[i].init();
        lvlpChannels[i].setLimits(12.0f, 0.5f);
    }

    buildChannelPointerArray();
    runner.begin(chPtrs, 8);
    configureTests();

    Serial.println(F("=== Running BSPD Tests ===\n"));
    runner.runAll();

    runner.displaySummary(&u8g2);
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
    bool buttonPressed = !digitalRead(pinEncoderSw);
    if (buttonPressed) {
        runner.displaySummary(&u8g2);
        delay(300); // debounce
    }
}
