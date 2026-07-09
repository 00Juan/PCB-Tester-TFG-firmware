/**
 * @file test_BSPD.cpp
 * @brief Testbench for BSPD (Brake System Plausibility Device).
 *
 * BSPD logic tested:
 * - Analog inputs: Hall sensor (HS), Brake sensor (BS)
 * - Outputs: SDC_OUT (SDC)
 *
 * Rules:
 * - SDC is 12V if any of HS or BS is < 1.5V.
 * - SDC is 0V if both HS and BS > 4V for > 500ms.
 * - SDC returns to 12V if any of HS or BS < 1.5V for > 10s.
 * - Implausibility: If ANY of HS or BS > 4.5V or < 0.5V, SDC is 0V immediately.
 *   - Will NOT automatically recover even if sensors return to normal.
 *   - Requires power cycle via sr.set(8, LOW) -> sr.set(8, HIGH) while sensors
 * are in normal range.
 *
 * Channel Mapping:
 * - CH1 (0x01): HS (Hall Sensor)
 * - CH2 (0x02): BS (Brake Sensor)
 * - CH3 (0x04): SDC_OUT (Output from BSPD)
 * - CH4 (0x08): SDC_IN  (12V Source)
 *
 * Test Strategy:
 * - We use `dutSetup` to configure HS and BS dynamically and power-cycle the
 * BSPD.
 * - We use `TEST_STATIC_VOLTAGE` to passively check the static state of the
 * SDC.
 * - We use `TEST_VOLTAGE_THRESHOLD` to check SDC recovering to 12V within 11s.
 */

#include "DUTTestbench.h"
#include "hardwareIOSetup.h"
#include <Arduino.h>

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

static constexpr float HS_NORMAL_V = 1.0f;
static constexpr float HS_FAULT_V = 4.0f;
static constexpr float BS_NORMAL_V = 1.0f;
static constexpr float BS_FAULT_V = 4.0f;

// Implausibility thresholds (> 4.5V or < 0.5V)
static constexpr float HS_IMPLAUS_HIGH_V = 5.0f;
static constexpr float HS_IMPLAUS_LOW_V = 0.0f;

static constexpr uint8_t MASK_HS = 0x01;     // CH1
static constexpr uint8_t MASK_BS = 0x02;     // CH2
static constexpr uint8_t MASK_SDC = 0x04;    // CH3
static constexpr uint8_t MASK_SDC_IN = 0x08; // CH4

// ============================================================================
// TESTBENCH INSTANCE
// ============================================================================

DUTTestRunner runner;
static LVLPChannel *chPtrs[8];

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

  // Drive CH4 (SDC_IN) constantly at 12V
  lvlpChannels[3].setMode(MODE_VOLTAGE_SOURCE);
  lvlpChannels[3].setOutputVoltage(12.0f);

  lvlpChannels[0].update();
  lvlpChannels[1].update();
  lvlpChannels[3].update();

  // Wait for tester outputs to physically settle
  delay(100);

  // Power cycle BSPD to guarantee it boots cleanly with normal sensor values.
  // This removes it from any implausibility state from a previous test.
  sr.set(8, LOW);
  delay(1000);
  sr.set(8, HIGH);
  delay(100); // Wait for BSPD boot
}

static void setupSingleFaultHS() {
  setupNormal();
  lvlpChannels[0].setOutputVoltage(HS_FAULT_V);
  lvlpChannels[0].update();
}

static void setupSingleFaultBS() {
  setupNormal();
  lvlpChannels[1].setOutputVoltage(BS_FAULT_V);
  lvlpChannels[1].update();
}

static void setupDoubleFault() {
  setupNormal();
  lvlpChannels[0].setOutputVoltage(HS_FAULT_V);
  lvlpChannels[1].setOutputVoltage(BS_FAULT_V);
  lvlpChannels[0].update();
  lvlpChannels[1].update();
}

static void setupRecovery() {
  setupNormal();

  // Trigger double fault and hold for 600ms to guarantee SDC drops to 0V
  lvlpChannels[0].setOutputVoltage(HS_FAULT_V);
  lvlpChannels[1].setOutputVoltage(BS_FAULT_V);
  lvlpChannels[0].update();
  lvlpChannels[1].update();
  delay(600);

  // Return ONE channel to normal (any < 1.5V) - 10s timer starts now
  lvlpChannels[0].setOutputVoltage(HS_NORMAL_V);
  lvlpChannels[0].update();
}

static void setupImplausHighHS() {
  setupNormal();
  lvlpChannels[0].setOutputVoltage(HS_IMPLAUS_HIGH_V);
  lvlpChannels[0].update();
}

static void setupImplausLowHS() {
  setupNormal();
  lvlpChannels[0].setOutputVoltage(HS_IMPLAUS_LOW_V);
  lvlpChannels[0].update();
}

static void setupImplausNoAutoRecovery() {
  setupNormal();

  // Trigger implausibility
  lvlpChannels[0].setOutputVoltage(HS_IMPLAUS_HIGH_V);
  lvlpChannels[0].update();
  delay(200); // ensure it drops

  // Return to normal (implausibility should remain latched)
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
    tc.staticVoltage = {.senseChannelMask = MASK_SDC,
                        .expectedVoltage = 12.0f,
                        .toleranceVolts = 2.0f, // SDC > 10V is fine
                        .settleMs = 100};
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 2: Single Fault HS > 500ms, SDC still 12V (because BS is OK)
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Single Fault HS > 500ms: SDC 12V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupSingleFaultHS;
    tc.staticVoltage = {
        .senseChannelMask = MASK_SDC,
        .expectedVoltage = 12.0f,
        .toleranceVolts = 2.0f,
        .settleMs = 580 // Wait > 500ms to prove it ignores single fault
    };
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 3: Single Fault BS > 500ms, SDC still 12V (because HS is OK)
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Single Fault BS > 500ms: SDC 12V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupSingleFaultBS;
    tc.staticVoltage = {
        .senseChannelMask = MASK_SDC,
        .expectedVoltage = 12.0f,
        .toleranceVolts = 2.0f,
        .settleMs = 580 // Wait > 500ms to prove it ignores single fault
    };
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 4: Double Fault < 500ms, SDC still 12V
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Double Fault < 500ms: SDC 12V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupDoubleFault;
    tc.staticVoltage = {
        .senseChannelMask = MASK_SDC,
        .expectedVoltage = 12.0f,
        .toleranceVolts = 2.0f,
        .settleMs = 380 // Check just before 500ms
    };
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 5: Double Fault > 500ms, SDC is 0V
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Double Fault > 500ms: SDC 0V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupDoubleFault;
    tc.staticVoltage = {
        .senseChannelMask = MASK_SDC,
        .expectedVoltage = 0.0f,
        .toleranceVolts = 1.0f,
        .settleMs = 580 // Check just after 500ms
    };
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 6: Recovery > 10s, SDC goes to 12V
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Recovery > 10s: SDC goes to 12V";
    tc.type = TEST_VOLTAGE_THRESHOLD;
    tc.dutSetup = setupRecovery;
    tc.voltageThreshold = {
        .driveChannelMask = 0x00, // Passive
        .driveVoltage = 0.0f,
        .senseChannelMask = MASK_SDC,
        .thresholdVoltage = 10.0f,
        .timeoutMs = 15000 // 11 seconds to recover
    };
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 7: Implausibility HS > 4.5V (SDC drops)
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Implausibility HS > 4.5V: SDC 0V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupImplausHighHS;
    tc.staticVoltage = {
        .senseChannelMask = MASK_SDC,
        .expectedVoltage = 0.0f,
        .toleranceVolts = 1.0f,
        .settleMs = 100 // Should trigger immediately
    };
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 8: Implausibility HS < 0.5V (SDC drops)
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Implausibility HS < 0.5V: SDC 0V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupImplausLowHS;
    tc.staticVoltage = {.senseChannelMask = MASK_SDC,
                        .expectedVoltage = 0.0f,
                        .toleranceVolts = 1.0f,
                        .settleMs = 100};
    runner.addTest(tc);
  }

  // -----------------------------------------------------------------------
  // TEST 9: Implausibility No Auto Recovery
  // -----------------------------------------------------------------------
  {
    TestCase tc;
    tc.name = "BSPD Implausibility No Auto Rec (>11s): SDC 0V";
    tc.type = TEST_STATIC_VOLTAGE;
    tc.dutSetup = setupImplausNoAutoRecovery;
    tc.staticVoltage = {
        .senseChannelMask = MASK_SDC,
        .expectedVoltage = 0.0f,
        .toleranceVolts = 1.0f,
        .settleMs = 11000 // Wait 11s to prove it does NOT recover to 12V
    };
    runner.addTest(tc);
  }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

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
  sr.setAllLow();

  for (uint8_t i = 0; i < 8; i++) {
    lvlpChannels[i].init();
    lvlpChannels[i].setLimits(12.0f, 0.5f);
  }

  buildChannelPointerArray();
  runner.begin(chPtrs, 8);
  configureTests();

  while(digitalRead(pinEncoderSw));
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
