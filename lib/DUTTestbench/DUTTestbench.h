#ifndef DUT_TESTBENCH_H
#define DUT_TESTBENCH_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "LVLPChannel.h"

// ============================================================================
// CONSTANTS
// ============================================================================

static constexpr uint8_t DUT_MAX_TESTS    = 32;
static constexpr uint8_t DUT_MAX_CHANNELS = 8;

// Maximum samples collected for ripple / inrush captures
static constexpr uint8_t DUT_MAX_SAMPLES  = 64;

// ============================================================================
// TEST OUTCOME
// ============================================================================

enum TestOutcome : uint8_t {
    OUTCOME_PASS,       ///< All assertions satisfied
    OUTCOME_FAIL,       ///< One or more assertions failed
    OUTCOME_TIMEOUT,    ///< Condition not reached within timeoutMs
    OUTCOME_ERROR,      ///< Configuration error (invalid mask, mode, etc.)
    OUTCOME_SKIP,       ///< Test was skipped (e.g. channel not available)
};

// ============================================================================
// TEST RESULT
// ============================================================================

struct TestResult {
    const char* testName;       ///< Pointer to static string from TestCase
    TestOutcome outcome;
    float       measuredValue;  ///< Primary measured value (V, A, ms, …)
    float       expectedValue;  ///< Reference / target value
    uint32_t    elapsedMs;      ///< Wall-clock time the test consumed
    char        details[80];    ///< Human-readable supplementary detail
};

// ============================================================================
// TEST TYPES
// ============================================================================

enum TestType : uint8_t {
    TEST_VOLTAGE_THRESHOLD,       ///< Drive channels → wait until sense channels cross threshold
    TEST_VOLTAGE_ACCURACY,        ///< Set voltage, settle, measure error vs tolerance
    TEST_VOLTAGE_RIPPLE,          ///< Sample voltage over window, compute peak-to-peak & RMS noise
    TEST_CURRENT_CONSUMPTION,     ///< Set voltage, measure steady-state current in [min, max]
    TEST_CURRENT_INRUSH,          ///< Monitor current from t=0, capture peak & settle time
    TEST_POWER_SEQUENCE,          ///< Enable channels in order with inter-step delays & checks
    TEST_PWM_INTEGRITY,           ///< Drive PWM, measure average DC on sense channel
    TEST_SHORT_CIRCUIT_PROTECTION,///< Force channel into short, verify fault fires within timeout
    TEST_LOAD_REGULATION,         ///< Measure voltage drop from no-load to loaded condition
    TEST_CROSS_CHANNEL_ISOLATION, ///< Drive one channel, verify neighbours stay near 0 V
    TEST_STATIC_VOLTAGE,          ///< Passively measure voltage on channels after a delay
};

// ============================================================================
// PARAMETER STRUCTS (one per test type)
// ============================================================================

/**
 * TEST_VOLTAGE_THRESHOLD
 *
 * Drive driveChannelMask channels to driveVoltage.
 * Poll senseChannelMask until all sense channels read >= thresholdVoltage.
 * Record latency. Fail if timeoutMs exceeded.
 *
 * measuredValue = time-to-threshold in ms (or elapsed on timeout).
 */
struct VoltageThresholdParams {
    uint8_t  driveChannelMask;   ///< Bitmask (bit 0 = CH1 … bit 7 = CH8)
    float    driveVoltage;       ///< Voltage applied to drive channels (V)
    uint8_t  senseChannelMask;   ///< Channels whose voltage is monitored
    float    thresholdVoltage;   ///< Minimum voltage to consider threshold crossed (V)
    uint32_t timeoutMs;          ///< Maximum time allowed (ms)
};

/**
 * TEST_VOLTAGE_ACCURACY
 *
 * Set channelMask channels to targetVoltage, wait settleMs, then measure.
 * PASS if |measured - target| <= toleranceVolts on all channels.
 *
 * measuredValue = worst-case absolute error across channels (V).
 */
struct VoltageAccuracyParams {
    uint8_t channelMask;        ///< Channels to drive and measure
    float   targetVoltage;      ///< Desired output voltage (V)
    float   toleranceVolts;     ///< Allowed error band (V), e.g. 0.05 for ±50 mV
    uint32_t settleMs;          ///< Settle time before sampling (ms)
    uint8_t  numSamples;        ///< Number of ADC readings to average (1–DUT_MAX_SAMPLES)
};

/**
 * TEST_VOLTAGE_RIPPLE
 *
 * Drive channelMask to driveVoltage. Collect numSamples ADC readings over
 * windowMs. Compute peak-to-peak and RMS noise.
 * PASS if peakToPeak <= maxRippleVolts.
 *
 * measuredValue = peak-to-peak ripple (V).
 */
struct VoltageRippleParams {
    uint8_t  channelMask;       ///< Channel(s) to drive and sample (each evaluated independently)
    float    driveVoltage;      ///< Voltage applied (V)
    uint32_t windowMs;          ///< Sampling window duration (ms)
    uint8_t  numSamples;        ///< Number of samples within window (1–DUT_MAX_SAMPLES)
    float    maxRippleVolts;    ///< Maximum allowed peak-to-peak ripple (V)
};

/**
 * TEST_CURRENT_CONSUMPTION
 *
 * Set channelMask to driveVoltage, wait settleMs, then measure current.
 * PASS if minCurrentA <= measured <= maxCurrentA.
 *
 * measuredValue = measured current (A).
 */
struct CurrentConsumptionParams {
    uint8_t  channelMask;       ///< Channel(s) to drive and measure
    float    driveVoltage;      ///< Supply voltage (V)
    float    minCurrentA;       ///< Minimum acceptable current (A)
    float    maxCurrentA;       ///< Maximum acceptable current (A)
    uint32_t settleMs;          ///< Settle time before sampling (ms)
    uint8_t  numSamples;        ///< ADC readings to average (1–DUT_MAX_SAMPLES)
};

/**
 * TEST_CURRENT_INRUSH
 *
 * Apply driveVoltage to channelMask at t=0. Sample current every sampleIntervalMs
 * for up to windowMs. Record peak current and time to settle within settleThresholdA
 * of steady state.
 *
 * measuredValue = peak inrush current (A).
 */
struct CurrentInrushParams {
    uint8_t  channelMask;           ///< Channel(s) to drive
    float    driveVoltage;          ///< Supply voltage (V)
    uint32_t windowMs;              ///< Total capture duration (ms)
    uint32_t sampleIntervalMs;      ///< Time between samples (ms, >= 1)
    float    maxInrushA;            ///< Maximum allowed peak current (A)
    float    settleThresholdA;      ///< Current must stay within this band to count as settled (A)
};

/**
 * TEST_POWER_SEQUENCE
 *
 * Enable channels one by one in the order encoded in stepChannelMask[].
 * After each step wait stepDelayMs, then verify the channel reached stepTargetV.
 * PASS if every step passes within its per-step timeout.
 *
 * measuredValue = number of steps that passed (out of stepCount).
 */
struct PowerSequenceParams {
    uint8_t  stepCount;                     ///< Number of sequencing steps (1–8)
    uint8_t  stepChannelMask[8];            ///< Channel(s) to enable at each step
    float    stepTargetVoltage[8];          ///< Expected voltage after each step (V)
    uint32_t stepDelayMs[8];                ///< Delay before measuring each step (ms)
    float    stepToleranceVolts;            ///< Voltage tolerance applied to all steps (V)
};

/**
 * TEST_PWM_INTEGRITY
 *
 * Drive driveChannelMask with a PWM signal (dutyCycle 0–255, frequency Hz).
 * After settleMs, measure the average DC voltage on senseChannelMask.
 * Expected DC ≈ driveVoltage * (dutyCycle / 255).
 * PASS if |measured - expected| <= toleranceVolts.
 *
 * measuredValue = measured average DC voltage (V).
 */
struct PwmIntegrityParams {
    uint8_t  driveChannelMask;   ///< Must be CH1–CH4 (PWM-capable channels)
    uint8_t  senseChannelMask;   ///< Channel(s) measuring the resulting DC
    float    driveVoltage;       ///< High-level voltage on the PWM channel (V)
    uint8_t  dutyCycle;          ///< PWM duty cycle (0–255)
    uint32_t frequency;          ///< PWM frequency (Hz)
    uint32_t settleMs;           ///< Settle time before sampling (ms)
    float    toleranceVolts;     ///< Allowed error on the measured DC (V)
};

/**
 * TEST_SHORT_CIRCUIT_PROTECTION
 *
 * Set channelMask to driveVoltage with the current limit deliberately low
 * (shortCurrentLimitA). Then monitor for STATUS_FAIL_OVERCURRENT within timeoutMs.
 * PASS if the fault fires within timeoutMs.
 * After the test, channels are reset and their previous limits are restored.
 *
 * measuredValue = time until fault fired (ms). 0 if OUTCOME_TIMEOUT.
 */
struct ShortCircuitProtectionParams {
    uint8_t  channelMask;           ///< Channel(s) to test
    float    driveVoltage;          ///< Voltage to attempt driving (V)
    float    shortCurrentLimitA;    ///< Threshold to trigger over-current (A)
    uint32_t timeoutMs;             ///< Maximum wait for fault (ms)
};

/**
 * TEST_LOAD_REGULATION
 *
 * Phase 1: driveChannelMask channels drive driveVoltage with no load → measure V_noload.
 * Phase 2: loadChannelMask channels are enabled as voltage sources at a computed voltage
 *          (driveVoltage - 2*loadCurrentA*R_shunt) to sink loadCurrentA → measure V_loaded.
 * PASS if (V_noload - V_loaded) <= maxDropVolts.
 *
 * Set loadChannelMask = 0 to skip the loading phase (useful for manual bench tests).
 * measuredValue = voltage drop (V).
 */
struct LoadRegulationParams {
    uint8_t  driveChannelMask;  ///< Tester channel(s) to drive (voltage source)
    uint8_t  loadChannelMask;   ///< DUT-sim channel(s) to act as load in Phase 2 (0 = none)
    float    driveVoltage;      ///< Voltage source setpoint (V)
    float    loadCurrentA;      ///< Target sink current for Phase 2 (A)
    uint32_t settleMs;          ///< Settle time for each phase (ms)
    float    maxDropVolts;      ///< Maximum allowed voltage drop (V)
};

/**
 * TEST_CROSS_CHANNEL_ISOLATION
 *
 * Drive driveChannelMask to driveVoltage. After settleMs, check that every
 * channel in senseChannelMask reads <= maxCouplingVolts.
 * If senseChannelMask == 0, all channels NOT in driveChannelMask are checked.
 *
 * Tip: when using paired wired channels (e.g. CH1↔CH5), include the partner
 * channel in driveChannelMask or senseChannelMask explicitly to avoid false
 * positives from floating nodes.
 *
 * measuredValue = worst-case coupling voltage across checked channels (V).
 */
struct CrossChannelIsolationParams {
    uint8_t  driveChannelMask;   ///< Channel(s) actively driven
    uint8_t  senseChannelMask;   ///< Channels to check for coupling (0 = all non-drive channels)
    float    driveVoltage;       ///< Stimulus voltage (V)
    uint32_t settleMs;           ///< Settle time before sampling (ms)
    float    maxCouplingVolts;   ///< Maximum tolerated voltage on checked channels (V)
};

/**
 * TEST_STATIC_VOLTAGE
 *
 * Passively read the voltage on senseChannelMask after settleMs.
 * PASS if |measured - expectedVoltage| <= toleranceVolts on all channels.
 *
 * measuredValue = worst-case error from expected (V).
 */
struct StaticVoltageParams {
    uint8_t  senseChannelMask;   ///< Channel(s) to read
    float    expectedVoltage;    ///< Target voltage (V)
    float    toleranceVolts;     ///< Allowed error band (V)
    uint32_t settleMs;           ///< Delay before sampling (ms)
};

// ============================================================================
// DUT SETUP CALLBACK
// ============================================================================

/**
 * @brief Optional callback invoked at the start of each test, after all
 *        channels are safely disconnected (safetyDisconnectAll) and before
 *        the tester channels are configured.
 *
 * Use this to configure DUT-simulator channels (e.g. CH5–CH8) for the
 * specific test scenario being run. Set to nullptr when no DUT setup is
 * needed.
 */
typedef void (*DUTSetupFn)(void);

// ============================================================================
// UNIFIED TEST CASE
// ============================================================================

struct TestCase {
    const char* name;     ///< Human-readable label (must point to a static string)
    TestType    type;
    DUTSetupFn  dutSetup; ///< Called before the test after safetyDisconnectAll; nullptr = none
    union {
        VoltageThresholdParams       voltageThreshold;
        VoltageAccuracyParams        voltageAccuracy;
        VoltageRippleParams          voltageRipple;
        CurrentConsumptionParams     currentConsumption;
        CurrentInrushParams          currentInrush;
        PowerSequenceParams          powerSequence;
        PwmIntegrityParams           pwmIntegrity;
        ShortCircuitProtectionParams shortCircuit;
        LoadRegulationParams         loadRegulation;
        CrossChannelIsolationParams  crossChannelIsolation;
        StaticVoltageParams          staticVoltage;
    };
};

// ============================================================================
// TEST RUNNER
// ============================================================================

class DUTTestRunner {
public:
    DUTTestRunner();

    /**
     * @brief Bind channel pointers and initialise the runner.
     * @param channels  Array of LVLPChannel pointers (must stay valid for the runner's lifetime)
     * @param count     Number of channels (max DUT_MAX_CHANNELS)
     */
    void begin(LVLPChannel* channels[], uint8_t count);

    /**
     * @brief Queue a test case.
     * @return true if added successfully, false if queue is full.
     */
    bool addTest(const TestCase& tc);

    /**
     * @brief Clear all queued test cases and results.
     */
    void clearTests();

    /**
     * @brief Set the OLED display for real-time updates.
     */
    void setDisplay(U8G2* display) { display_ = display; }

    /**
     * @brief Run all queued tests sequentially (blocking).
     *        Channels are left in MODE_HIGH_IMPEDANCE after each test.
     * @param display Optional OLED display to show test progress.
     */
    void runAll(U8G2* display = nullptr);

    /**
     * @brief Run a single test case by index.
     * @return Result of the test.
     */
    TestResult runOne(uint8_t index);

    // -------------------------------------------------------------------------
    // Result accessors
    // -------------------------------------------------------------------------

    uint8_t          getTestCount()              const { return testCount_; }
    const TestResult& getResult(uint8_t i)       const { return results_[i]; }

    // -------------------------------------------------------------------------
    // Reporting
    // -------------------------------------------------------------------------

    /**
     * @brief Print full test report to Serial.
     */
    void printReport() const;

    /**
     * @brief Print summary (PASS/FAIL counts + list) to Serial.
     */
    void printSummary() const;

    /**
     * @brief Render summary on an OLED display (optional).
     *        Displays pass/fail counts and scrollable list.
     * @param display  Pointer to a U8G2 display object (must already be initialised).
     */
    void displaySummary(U8G2* display) const;

private:
    // -------------------------------------------------------------------------
    // Storage
    // -------------------------------------------------------------------------
    LVLPChannel* ch_[DUT_MAX_CHANNELS];
    uint8_t      chCount_;

    TestCase   tests_[DUT_MAX_TESTS];
    TestResult results_[DUT_MAX_TESTS];
    uint8_t    testCount_;

    U8G2* display_ = nullptr;

    // -------------------------------------------------------------------------
    // Per-type runners (called by runOne)
    // -------------------------------------------------------------------------
    TestResult runVoltageThresholdTest     (const TestCase& tc);
    TestResult runVoltageAccuracyTest      (const TestCase& tc);
    TestResult runVoltageRippleTest        (const TestCase& tc);
    TestResult runCurrentConsumptionTest   (const TestCase& tc);
    TestResult runCurrentInrushTest        (const TestCase& tc);
    TestResult runPowerSequenceTest        (const TestCase& tc);
    TestResult runPwmIntegrityTest         (const TestCase& tc);
    TestResult runShortCircuitProtectionTest(const TestCase& tc);
    TestResult runLoadRegulationTest       (const TestCase& tc);
    TestResult runCrossChannelIsolationTest(const TestCase& tc);
    TestResult runStaticVoltageTest        (const TestCase& tc);

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /// Disconnects all channels (high impedance + 0 V DAC)
    void safetyDisconnectAll();

    /// Applies mode + voltage to channels selected by bitmask
    void applyChannelMask(uint8_t mask, LVLPMode mode, float voltage);

    /// Samples the average voltage of a single channel (numSamples averaged)
    float sampleAverageVoltage(uint8_t chIdx, uint8_t numSamples);

    /// Render test status in real-time
    void updateRealtimeDisplay(const char* testName, uint32_t elapsedMs, uint32_t totalMs);

    /// Blocking wait that updates the OLED display while waiting
    void waitAndDisplay(uint32_t waitMs, const char* testName);

    /// Samples the average current of a single channel (numSamples averaged)
    float sampleAverageCurrent(uint8_t chIdx, uint8_t numSamples);

    /// Returns outcome string literal for Serial printing
    static const char* outcomeStr(TestOutcome o);
};

#endif // DUT_TESTBENCH_H
