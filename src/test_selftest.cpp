/**
 * @file test_selftest.cpp
 * @brief LVLP Self-Test — uses CH5-CH8 to simulate a DUT.
 *
 * PHYSICAL WIRE MAP (jumper wires on the bench)
 * =============================================
 *   Node A : CH1 output (idx 0, mask 0x01)  ↔  CH5 output (idx 4, mask 0x10)
 *   Node B : CH2 output (idx 1, mask 0x02)  ↔  CH6 output (idx 5, mask 0x20)
 *   Node C : CH3 output (idx 2, mask 0x04)  ↔  CH7 output (idx 6, mask 0x40)
 *   Node D : CH4 output (idx 3, mask 0x08)  ↔  CH8 output (idx 7, mask 0x80)
 *
 * ROLE ASSIGNMENT
 * ===============
 *   Tester channels (stimulus / measurement) : CH1, CH2, CH3, CH4 (indices 0–3)
 *   DUT simulator channels                    : CH5, CH6, CH7, CH8 (indices 4–7)
 *
 * DUT SIMULATOR CURRENT PHYSICS
 * ==============================
 * Each channel has a 10.5 Ω series shunt between its op-amp output and
 * the relay/SSR output.  When two channels are wired together at their
 * relay outputs, the circuit for one node is:
 *
 *   CH_tester_opamp ─[R=10.5Ω]─ NODE ─[R=10.5Ω]─ CH_sim_opamp
 *
 * To make the tester channel measure I_target amps:
 *   V_sim = V_tester − 2 × I_target × R_shunt
 *
 * This formula is captured in dutSimVoltage() below.
 *
 * HOW TO FLASH
 * ============
 *   pio run -e test_selftest --target upload
 *
 * SERIAL OUTPUT
 *   Open monitor at 115200 baud.  A full report is printed after all tests.
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"
#include "DUTTestbench.h"

// ============================================================================
// CONSTANTS
// ============================================================================

static constexpr float SHUNT_R = 10.5f;  // Ω, matches LVLPChannel::SHUNT_RESISTANCE

/**
 * @brief Compute the voltage a DUT-sim channel must drive so that the
 *        tester channel measures I_target amperes through its own shunt.
 *
 * @param testerVoltage  Voltage the tester channel will be driving (V)
 * @param targetCurrentA Desired current reading on the tester channel (A)
 * @return Voltage to set on the DUT-sim channel (V), clamped to ≥ 0.
 */
static float dutSimVoltage(float testerVoltage, float targetCurrentA) {
    float v = testerVoltage - 2.0f * targetCurrentA * SHUNT_R;
    return (v < 0.0f) ? 0.0f : v;
}

// ============================================================================
// DUT SIMULATOR HELPER
// ============================================================================

/**
 * @brief Configure a DUT-sim channel as a voltage source.
 * @param dutIdx   LVLPChannel index (4–7 for CH5–CH8)
 * @param voltage  Voltage to drive (V)
 */
static void dutDrive(uint8_t dutIdx, float voltage) {
    lvlpChannels[dutIdx].resetStatus();
    lvlpChannels[dutIdx].setLimits(12.0f, 0.5f);   // generous — DUT sim won't fault
    lvlpChannels[dutIdx].setOutputVoltage(voltage);
    lvlpChannels[dutIdx].setMode(MODE_VOLTAGE_SOURCE);
}

// ============================================================================
// DUT SETUP CALLBACKS  (one static function per test)
// ============================================================================
// Each callback is called by the runner AFTER safetyDisconnectAll() and
// BEFORE the tester channels are configured.  Only DUT-sim channels (4–7)
// should be touched here.
//
// Node mapping reminder:
//   CH5 (idx 4) = DUT side of node A  (paired with CH1)
//   CH6 (idx 5) = DUT side of node B  (paired with CH2)
//   CH7 (idx 6) = DUT side of node C  (paired with CH3)
//   CH8 (idx 7) = DUT side of node D  (paired with CH4)

/**
 * TEST 1 — VOLTAGE_THRESHOLD
 * Scenario:
 *   - Tester drives CH1 @ 3.3 V on node A  (CH5 stays HIGH_Z)
 *   - DUT sim pre-drives CH6 @ 3.3 V on node B
 *   - Tester polls CH2 (node B) and should detect threshold immediately
 *
 * Expected: PASS, latency < 50 ms
 */
static void dut_voltageThreshold() {
    dutDrive(5, 3.3f);   // CH6 drives node B → CH2 senses it
}

/**
 * TEST 2 — VOLTAGE_ACCURACY
 * Scenario:
 *   - Tester drives CH1 @ 5.0 V on node A and measures its own output
 *   - CH5 stays HIGH_Z (no load)
 *
 * Expected: PASS, |error| ≤ 0.15 V
 */
// dutSetup = nullptr  (no DUT sim action needed)

/**
 * TEST 3 — VOLTAGE_RIPPLE
 * Scenario:
 *   - Tester drives CH2 @ 5.0 V on node B and samples its own output
 *   - CH6 stays HIGH_Z
 *
 * Expected: PASS, ripple < 0.25 V  (DAC is DC-stable)
 */
// dutSetup = nullptr

/**
 * TEST 4 — CURRENT_CONSUMPTION
 * Scenario:
 *   - CH5 drives node A at V_sim = dutSimVoltage(3.3V, 15mA) ≈ 2.985 V
 *   - Creates ~15 mA current measured by CH1 through CH1's own shunt
 *   - Tester drives CH1 @ 3.3 V and reads current
 *
 * Expected: PASS, current in [8 mA, 30 mA]
 */
static void dut_currentConsumption() {
    dutDrive(4, dutSimVoltage(3.3f, 0.015f));   // CH5 sinks ~15 mA from CH1
}

/**
 * TEST 5 — CURRENT_INRUSH
 * Scenario:
 *   - CH7 drives node C at V_sim = dutSimVoltage(5.0V, 30mA) ≈ 4.37 V
 *   - Creates a constant ~30 mA on node C measured by CH3
 *   - (True inrush requires a dynamic load; here we simulate steady-state
 *     for framework validation)
 *
 * Expected: PASS, peak ≤ 50 mA
 */
static void dut_currentInrush() {
    dutDrive(6, dutSimVoltage(5.0f, 0.030f));   // CH7 sinks ~30 mA from CH3
}

/**
 * TEST 6 — POWER SEQUENCE
 * Scenario:
 *   - Tester sequences CH1 → CH2, measuring each channel's own output
 *   - CH5 and CH6 stay HIGH_Z (no interaction)
 *
 * Expected: PASS, both steps within ±0.2 V tolerance
 */
// dutSetup = nullptr

/**
 * TEST 7 — PWM INTEGRITY
 * Scenario:
 *   - Tester drives CH1 in PWM mode (50 % duty, 1 kHz) on node A
 *   - CH5 (HIGH_Z) is used as the sense channel on the same node A
 *   - CH5's ADC samples the PWM waveform; average ≈ V_drive × 0.5
 *
 * Note: senseChannelMask = 0x10 (CH5). Both drive and sense are on node A.
 *
 * Expected: PASS, measured DC within ±0.35 V of expected (1.65 V for 3.3 V drive)
 */
// dutSetup = nullptr (CH5 already HIGH_Z after safetyDisconnectAll)

/**
 * TEST 8 — SHORT-CIRCUIT PROTECTION
 * Scenario:
 *   - CH5 drives node A at V_sim = dutSimVoltage(5.0V, 20mA) ≈ 4.58 V
 *   - CH1 is configured with a 5 mA current limit
 *   - ~20 mA flows through CH1's shunt → OC fault fires
 *
 * Expected: PASS, fault within 200 ms
 */
static void dut_shortCircuit() {
    dutDrive(4, dutSimVoltage(5.0f, 0.020f));   // CH5 draws ~20 mA from CH1
}

/**
 * TEST 9 — LOAD REGULATION
 * Scenario:
 *   - Phase 1: CH1 drives 5 V (node A), CH5 HIGH_Z  → measure V_noload
 *   - Phase 2: runner applies CH5 (loadChannelMask = 0x10) at computed voltage
 *              to sink 20 mA                         → measure V_loaded
 *   - Expected drop: 2 × 0.020 × 10.5 = 0.42 V (both shunts contribute),
 *     but CH1 only reads its own shunt drop: 0.020 × 10.5 ≈ 0.21 V
 *
 * No dutSetup needed — the runner handles both phases internally via
 * loadChannelMask = 0x10.
 *
 * Expected: PASS, drop ≤ 0.5 V
 */
// dutSetup = nullptr (runner manages the load channel internally)

/**
 * TEST 10 — CROSS-CHANNEL ISOLATION
 * Scenario:
 *   - Tester drives CH1 @ 5 V on node A (CH1 + CH5 on same physical wire)
 *   - senseChannelMask = 0x0E (CH2, CH3, CH4) — nodes B, C, D are isolated
 *   - CH6, CH7, CH8 stay HIGH_Z; their nodes float near 0 V
 *
 * driveChannelMask includes CH5 (0x11) to skip it in the coupling check,
 * since it physically reads the same voltage as CH1.
 *
 * Expected: PASS, isolation channels read ≤ 0.15 V
 */
// dutSetup = nullptr

// ============================================================================
// TESTBENCH INSTANCE
// ============================================================================

DUTTestRunner runner;
static LVLPChannel* chPtrs[8];

// ============================================================================
// TEST CONFIGURATION
// ============================================================================

static void configureSelftests() {

    // -----------------------------------------------------------------------
    // TEST 1 — Voltage threshold latency  (node B: CH2 senses CH6's drive)
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "VCC_3V3 threshold latency (CH1->CH2 via CH6)";
        tc.type     = TEST_VOLTAGE_THRESHOLD;
        tc.dutSetup = dut_voltageThreshold;
        tc.voltageThreshold = {
            .driveChannelMask = 0x01,   // CH1 drives node A
            .driveVoltage     = 3.3f,
            .senseChannelMask = 0x02,   // CH2 senses node B (driven by CH6)
            .thresholdVoltage = 3.0f,
            .timeoutMs        = 200
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 2 — Voltage accuracy  (CH1 drives and measures its own output)
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 voltage accuracy @ 5.0 V (no load)";
        tc.type     = TEST_VOLTAGE_ACCURACY;
        tc.dutSetup = nullptr;
        tc.voltageAccuracy = {
            .channelMask    = 0x01,     // CH1
            .targetVoltage  = 5.0f,
            .toleranceVolts = 0.15f,    // ±150 mV
            .settleMs       = 200,
            .numSamples     = 8
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 3 — Voltage ripple  (CH2 drives and samples its own output)
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH2 output ripple @ 5.0 V";
        tc.type     = TEST_VOLTAGE_RIPPLE;
        tc.dutSetup = nullptr;
        tc.voltageRipple = {
            .channelMask    = 0x02,     // CH2 (node B, CH6 HIGH_Z)
            .driveVoltage   = 5.0f,
            .windowMs       = 100,
            .numSamples     = 32,
            .maxRippleVolts = 0.25f     // 250 mV — DAC is DC-stable
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 4 — Current consumption  (CH5 sinks ~15 mA from CH1 @ 3.3 V)
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 idle current @ 3.3 V (CH5 = 15 mA load)";
        tc.type     = TEST_CURRENT_CONSUMPTION;
        tc.dutSetup = dut_currentConsumption;
        tc.currentConsumption = {
            .channelMask  = 0x01,       // CH1 drives and measures
            .driveVoltage = 3.3f,
            .minCurrentA  = 0.008f,     //  8 mA lower bound (calibration margin)
            .maxCurrentA  = 0.030f,     // 30 mA upper bound
            .settleMs     = 150,
            .numSamples   = 8
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 5 — Current inrush  (CH7 creates constant ~30 mA on CH3 @ 5 V)
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH3 inrush @ 5.0 V (CH7 = 30 mA simulated load)";
        tc.type     = TEST_CURRENT_INRUSH;
        tc.dutSetup = dut_currentInrush;
        tc.currentInrush = {
            .channelMask      = 0x04,   // CH3
            .driveVoltage     = 5.0f,
            .windowMs         = 300,
            .sampleIntervalMs = 10,
            .maxInrushA       = 0.075f, // 75 mA max (transient calc spike ~62mA, actual ~30 mA)
            .settleThresholdA = 0.005f  //  5 mA settle band
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 6 — Power sequence  (CH1 @ 1.8 V then CH2 @ 3.3 V, no DUT load)
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1->CH2 power-on sequence (no DUT load)";
        tc.type     = TEST_POWER_SEQUENCE;
        tc.dutSetup = nullptr;
        tc.powerSequence.stepCount               = 2;
        tc.powerSequence.stepChannelMask[0]      = 0x01;   // Step 1: CH1
        tc.powerSequence.stepTargetVoltage[0]    = 1.8f;
        tc.powerSequence.stepDelayMs[0]          = 100;
        tc.powerSequence.stepChannelMask[1]      = 0x02;   // Step 2: CH2
        tc.powerSequence.stepTargetVoltage[1]    = 3.3f;
        tc.powerSequence.stepDelayMs[1]          = 100;
        tc.powerSequence.stepToleranceVolts      = 0.20f;  // ±200 mV
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 7 — PWM integrity  (CH1 PWM 50 % @ 1 kHz, CH5 reads avg DC)
    //
    // CH5 is on the same physical node A as CH1.  In HIGH_Z mode, CH5's
    // relay is open, but its ADC pin is exposed to node A, so it reads the
    // time-averaged voltage of the PWM waveform.
    // Expected DC = 3.3 V × 128/255 ≈ 1.655 V
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 PWM 50% @1kHz -> CH5 avg DC (same node)";
        tc.type     = TEST_PWM_INTEGRITY;
        tc.dutSetup = nullptr;           // CH5 left HIGH_Z by safetyDisconnectAll
        tc.pwmIntegrity = {
            .driveChannelMask = 0x01,   // CH1 (PWM-capable, has pwmPin)
            .senseChannelMask = 0x10,   // CH5 reads node A average
            .driveVoltage     = 3.3f,
            .dutyCycle        = 128,    // ≈ 50 %
            .frequency        = 10,   // 1 kHz
            .settleMs         = 300,    // 300 ms → 300 full PWM cycles averaged
            .toleranceVolts   = 0.35f   // generous: ADC sampling may alias
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 8 — Short-circuit protection  (CH5 causes ~20 mA on CH1)
    //
    // CH1 current limit set to 5 mA.  CH5 (pre-set by dutSetup) draws ~20 mA.
    // OC fault should fire within 200 ms.
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 OC protection (CH5 = 20 mA load, limit = 5 mA)";
        tc.type     = TEST_SHORT_CIRCUIT_PROTECTION;
        tc.dutSetup = dut_shortCircuit;
        tc.shortCircuit = {
            .channelMask        = 0x01, // CH1
            .driveVoltage       = 5.0f,
            .shortCurrentLimitA = 0.005f, // 5 mA — triggers on ~20 mA load
            .timeoutMs          = 200
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 9 — Load regulation  (runner uses loadChannelMask to add CH5 load)
    //
    // Phase 1: CH1 @ 5 V, CH5 HIGH_Z → V_noload
    // Phase 2: runner drives CH5 at dutSimVoltage(5V, 20mA) → V_loaded
    // Expected drop ≈ 20mA × 10.5Ω = 0.21 V on CH1's shunt
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1 load regulation 5V/20mA (CH5 as load)";
        tc.type     = TEST_LOAD_REGULATION;
        tc.dutSetup = nullptr;           // runner handles Phase 2 internally
        tc.loadRegulation = {
            .driveChannelMask = 0x01,   // CH1 drives and measures
            .loadChannelMask  = 0x10,   // CH5 acts as load in Phase 2
            .driveVoltage     = 5.0f,
            .loadCurrentA     = 0.020f, // 20 mA
            .settleMs         = 200,
            .maxDropVolts     = 0.50f   // 500 mV — accounts for both shunts
        };
        runner.addTest(tc);
    }

    // -----------------------------------------------------------------------
    // TEST 10 — Cross-channel isolation  (CH1 drives, CH2/CH3/CH4 must stay quiet)
    //
    // driveChannelMask = 0x11 includes CH5 (same physical node as CH1) so the
    // runner skips it in the coupling check.
    // senseChannelMask = 0x0E checks only CH2, CH3, CH4 (nodes B, C, D).
    // -----------------------------------------------------------------------
    {
        TestCase tc;
        tc.name     = "CH1+CH5 @ 5V -> CH2/CH3/CH4 isolation";
        tc.type     = TEST_CROSS_CHANNEL_ISOLATION;
        tc.dutSetup = nullptr;
        tc.crossChannelIsolation = {
            .driveChannelMask = 0x11,   // CH1 (0x01) + CH5 (0x10) on same node A
            .senseChannelMask = 0x0E,   // CH2 (0x02) + CH3 (0x04) + CH4 (0x08) only
            .driveVoltage     = 5.0f,
            .settleMs         = 100,
            .maxCouplingVolts = 0.15f   // 150 mV
        };
        runner.addTest(tc);
    }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println(F("\n=== LVLP Self-Test (CH1-4 tester / CH5-8 DUT sim) ==="));
    Serial.println(F("Wiring: CH1<->CH5  CH2<->CH6  CH3<->CH7  CH4<->CH8"));
    Serial.println();

    // --- Hardware initialisation ---
    if (!initializeMCP4728()) {
        Serial.println(F("ERROR: DAC (MCP4728) init failed!"));
    }
    if (!initializeMCP3208()) {
        Serial.println(F("ERROR: ADC (MCP3208) init failed!"));
    }
    initializeSSD1309();
    initializeWS2812B();
    initializeEncoder(-100, 100, true);

    // Shift-register: enable output
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllHigh();

    // Initialise all 8 LVLP channels (tester + DUT sim)
    for (uint8_t i = 0; i < 8; i++) {
        lvlpChannels[i].init();
        // Set generous safety limits so DUT sim channels don't spuriously fault
        lvlpChannels[i].setLimits(12.0f, 0.05f);
    }

    // Build runner with all 8 channels (runner needs DUT-sim channels for
    // the load regulation test's Phase 2)
    for (uint8_t i = 0; i < 8; i++) chPtrs[i] = &lvlpChannels[i];
    runner.begin(chPtrs, 8);

    // Queue all self-tests
    configureSelftests();

    Serial.println(F("=== Starting self-test sequence ===\n"));
    runner.runAll();

    // Show summary on OLED
    runner.displaySummary(&u8g2);
}

// ============================================================================
// LOOP — press encoder button to redisplay summary
// ============================================================================

void loop() {
    if (!digitalRead(pinEncoderSw)) {
        runner.displaySummary(&u8g2);
        runner.printSummary();
        delay(300); // debounce
    }
}
