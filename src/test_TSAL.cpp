/**
 * @file test_TSAL.cpp
 * @brief Testbench for TSAL (Tractive System Active Light).
 *
 * TSAL logic tested (see "Requisitos y Funcionamiento TSAL", EV4.10.x):
 * - Green_Circuit: GreenLed ON only when TS_is_OFF=1 (all AIRs open, precharge
 *   open, no HV in the accumulator) AND Safe_State=1 (no implausibility).
 *   Internal signals exercised: TS_is_OFF, AIR+_Stuck, AIR-_Stuck,
 *   PRECHARGE_Stuck, RelayClosed_but_No_Voltage, SCS_Failure, Safe_State.
 * - Red_Circuit: NOT powered in this bench (no HV is connected to HV+), so
 *   RedLed must stay OFF in every scenario. This also proves the red LED
 *   signal (Red_On) is independent from the green-LED control logic.
 * - Latching (EV4.10.5) is NOT implemented on this DUT revision:
 *   implausibilities must clear as soon as the stimulus is removed and the
 *   green LED must auto-recover (verified by an explicit test). The DUT is
 *   therefore powered once for the whole campaign; no power-cycle between
 *   tests is needed.
 *
 * Input signal levels (from the TSAL spec):
 * - AIR+_AUX2 / AIR-_AUX2 / PRECHARGE_AUX2 (physical relay state):
 *     5 V = closed, 0.8 V = open. SCS: < 217 mV = short-to-GND / open circuit.
 * - HV_Accu_State: 5 V = no HV present, 0.8 V = HV present. Also SCS.
 * - AIR+_Coil / AIR-_Coil / Precharge_Coil (intended relay state):
 *     5 V = open, 0 V = closed.
 * - SDC_END: 5 V = shutdown circuit open, 0 V = closed.
 *
 * Channel Mapping (LVLP CH1-CH8 drive the DUT inputs):
 * - CH1: HV_Accu_State
 * - CH2: AIR-_AUX2
 * - CH3: AIR-_Coil
 * - CH4: SDC_END
 * - CH5: Precharge_Coil
 * - CH6: PRECHARGE_AUX2
 * - CH7: AIR+_AUX2
 * - CH8: AIR+_Coil
 * - CH9  (HPCH1): GreenLed (DUT output, open-drain)
 * - CH10 (HPCH2): RedLed   (DUT output, open-drain)
 * - CH11 (HV):    DUT GND return, switched by the HV channel relay.
 *   LV supply is external and always present; CH11 is closed once at the
 *   start of the campaign to power the DUT and opened at the end.
 *
 * LED sensing: GreenLed/RedLed are open-drain outputs. On the PCBT both are
 * pulled to a fixed 12 V rail through the HP channels (PMOS must conduct),
 * so with the HP channel connected:
 *   LED ON  -> output pulls the line low  -> VOut ~ 0 V
 *   LED OFF -> output in high impedance   -> VOut ~ 12 V
 *
 * Test Strategy:
 * - `dutSetup` callbacks drive the 8 LVLP inputs; every scenario starts from
 *   the safe-state values, so the previous test's stimulus is always removed
 *   (no latching -> the DUT logic follows the new inputs directly).
 * - `TEST_STATIC_VOLTAGE` with `senseHPChannelMask` passively checks the
 *   LED lines on the HP channels.
 * - Note: some implausibilities overlap when observed from outside (only the
 *   LEDs are visible). E.g. RelayClosed_but_No_Voltage scenarios also trip
 *   the stuck-relay XOR; both must force GreenLed OFF, which is what is
 *   asserted.
 */

#include "DUTTestbench.h"
#include "hardwareIOSetup.h"
#include <Arduino.h>

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

// LVLP channel assignment (1-based, as wired to the DUT)
static constexpr uint8_t CH_HV_ACCU_STATE = 1;
static constexpr uint8_t CH_AIR_M_AUX2    = 2;
static constexpr uint8_t CH_AIR_M_COIL    = 3;
static constexpr uint8_t CH_SDC_END       = 4;
static constexpr uint8_t CH_PRE_COIL      = 5;
static constexpr uint8_t CH_PRE_AUX2      = 6;
static constexpr uint8_t CH_AIR_P_AUX2    = 7;
static constexpr uint8_t CH_AIR_P_COIL    = 8;

// Signal levels per TSAL spec
static constexpr float AUX2_CLOSED_V       = 5.0f; // physical relay closed
static constexpr float AUX2_OPEN_V         = 0.8f; // physical relay open
static constexpr float HV_ACCU_NO_HV_V     = 5.0f; // no HV in accumulator
static constexpr float HV_ACCU_HV_PRES_V   = 0.8f; // HV present
static constexpr float COIL_OPEN_V         = 5.0f; // intended state: open
static constexpr float COIL_CLOSED_V       = 0.0f; // intended state: closed
static constexpr float SDC_OPEN_V          = 5.0f; // shutdown circuit open
static constexpr float SDC_CLOSED_V        = 0.0f; // shutdown circuit closed
static constexpr float SCS_SHORT_GND_V     = 0.0f; // < 217 mV -> SCS_Failure

// LED lines on the HP channels (open-drain outputs pulled to 12 V)
static constexpr float LED_ON_V   = 0.0f;  // DUT pulls the line low
static constexpr float LED_OFF_V  = 12.0f; // line at the PCBT 12 V rail
static constexpr float LED_TOL_V  = 2.0f;

static constexpr uint8_t MASK_HP_GREEN = 0x01; // HPCH1 = CH9 (GreenLed)
static constexpr uint8_t MASK_HP_RED   = 0x02; // HPCH2 = CH10 (RedLed)

// ============================================================================
// TESTBENCH INSTANCE
// ============================================================================

DUTTestRunner runner;
static LVLPChannel *chPtrs[8];
static HPCH *hpPtrs[2];

static void buildChannelPointerArrays() {
  for (uint8_t i = 0; i < 8; i++) {
    chPtrs[i] = &lvlpChannels[i];
  }
  hpPtrs[0] = &hpChannels[0];
  hpPtrs[1] = &hpChannels[1];
}

// ============================================================================
// DUT SETUP HELPERS
// ============================================================================

static void driveInput(uint8_t ch, float volts) {
  lvlpChannels[ch - 1].setMode(MODE_VOLTAGE_SOURCE);
  lvlpChannels[ch - 1].setOutputVoltage(volts);
  lvlpChannels[ch - 1].update();
}

// SCS open-circuit fault: leave the DUT input floating
static void openInput(uint8_t ch) {
  lvlpChannels[ch - 1].setMode(MODE_HIGH_IMPEDANCE);
}

// ============================================================================
// DUT SETUP CALLBACKS
// ============================================================================

/**
 * Baseline "vehicle safe" state (green LED must turn ON):
 * - All AIRs + precharge physically open (AUX2 = 0.8 V) and intended open
 *   (Coil = 5 V) -> no stuck relay.
 * - No HV in the accumulator (HV_Accu_State = 5 V).
 * - Shutdown circuit open (SDC_END = 5 V); coils open, so no
 *   RelayClosed_but_No_Voltage.
 * - All SCS signals above 217 mV -> no SCS_Failure.
 * => TS_is_OFF = 1 and Safe_State = 1.
 *
 * The DUT stays powered for the whole campaign (no latching, so no
 * power-cycle is needed): each test simply re-drives the safe values and
 * overrides the channels relevant to its scenario.
 */
static void setupSafeState() {
  driveInput(CH_HV_ACCU_STATE, HV_ACCU_NO_HV_V);
  driveInput(CH_AIR_P_AUX2, AUX2_OPEN_V);
  driveInput(CH_AIR_M_AUX2, AUX2_OPEN_V);
  driveInput(CH_PRE_AUX2, AUX2_OPEN_V);
  driveInput(CH_AIR_P_COIL, COIL_OPEN_V);
  driveInput(CH_AIR_M_COIL, COIL_OPEN_V);
  driveInput(CH_PRE_COIL, COIL_OPEN_V);
  driveInput(CH_SDC_END, SDC_OPEN_V);

  delay(200); // let tester outputs and DUT logic settle
}

// --- TS_is_OFF violations (no implausibility involved) ----------------------

/** HV present in the accumulator -> TS_is_OFF = 0, green must stay OFF. */
static void setupHVPresent() {
  setupSafeState();
  driveInput(CH_HV_ACCU_STATE, HV_ACCU_HV_PRES_V);
}

/**
 * One relay coherently closed (AUX2 = closed AND Coil = closed) with the
 * shutdown circuit closed (SDC_END = 0 V, so RelayClosed_but_No_Voltage
 * cannot fire) -> pure TS_is_OFF = 0 condition.
 */
static void setupAirPClosed() {
  setupSafeState();
  driveInput(CH_AIR_P_AUX2, AUX2_CLOSED_V);
  driveInput(CH_AIR_P_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

static void setupAirMClosed() {
  setupSafeState();
  driveInput(CH_AIR_M_AUX2, AUX2_CLOSED_V);
  driveInput(CH_AIR_M_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

static void setupPreClosed() {
  setupSafeState();
  driveInput(CH_PRE_AUX2, AUX2_CLOSED_V);
  driveInput(CH_PRE_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

// --- Implausibility a) stuck relay, EV4.10.13 (XOR Coil vs State) -----------

/**
 * Intended closed (Coil = 0 V) but physically open (AUX2 = 0.8 V) -> XOR = 1.
 * SDC_END is driven to 0 V so RelayClosed_but_No_Voltage does NOT fire and
 * the XOR path is the only thing blocking the green LED (TS_is_OFF stays 1).
 */
static void setupStuckAirP() {
  setupSafeState();
  driveInput(CH_AIR_P_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

static void setupStuckAirM() {
  setupSafeState();
  driveInput(CH_AIR_M_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

static void setupStuckPre() {
  setupSafeState();
  driveInput(CH_PRE_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

// --- Implausibility b) RelayClosed_but_No_Voltage, EV4.10.14 ----------------

/**
 * No HV (HV_Accu_State = 5 V), shutdown open (SDC_END = 5 V) but a coil
 * commands closed (Coil = 0 V) -> RelayClosed_but_No_Voltage = 1.
 * Externally this overlaps with the stuck-relay XOR (AUX2 stays open);
 * both paths must force the green LED OFF.
 */
static void setupRlyClosedNoVoltAirP() {
  setupSafeState();
  driveInput(CH_AIR_P_COIL, COIL_CLOSED_V);
}

static void setupRlyClosedNoVoltAirM() {
  setupSafeState();
  driveInput(CH_AIR_M_COIL, COIL_CLOSED_V);
}

static void setupRlyClosedNoVoltPre() {
  setupSafeState();
  driveInput(CH_PRE_COIL, COIL_CLOSED_V);
}

// --- Implausibility c) SCS failures, T11.9.2 (< 217 mV) ---------------------

static void setupScsShortAirPAux2() {
  setupSafeState();
  driveInput(CH_AIR_P_AUX2, SCS_SHORT_GND_V);
}

static void setupScsShortAirMAux2() {
  setupSafeState();
  driveInput(CH_AIR_M_AUX2, SCS_SHORT_GND_V);
}

static void setupScsShortPreAux2() {
  setupSafeState();
  driveInput(CH_PRE_AUX2, SCS_SHORT_GND_V);
}

/**
 * HV_Accu_State shorted to GND: 0 V also reads as "HV present" in the TS
 * logic, so the green LED is blocked by two paths; the assert (green OFF)
 * holds either way.
 */
static void setupScsShortHvAccu() {
  setupSafeState();
  driveInput(CH_HV_ACCU_STATE, SCS_SHORT_GND_V);
}

static void setupScsOpenAirPAux2() {
  setupSafeState();
  openInput(CH_AIR_P_AUX2);
}

static void setupScsOpenAirMAux2() {
  setupSafeState();
  openInput(CH_AIR_M_AUX2);
}

static void setupScsOpenPreAux2() {
  setupSafeState();
  openInput(CH_PRE_AUX2);
}

static void setupScsOpenHvAccu() {
  setupSafeState();
  openInput(CH_HV_ACCU_STATE);
}

// --- Auto-recovery (no latching implemented) ---------------------------------

/**
 * Trigger a stuck-relay implausibility, hold it, then return every input to
 * the safe state WITHOUT power-cycling the DUT. Since latching (EV4.10.5)
 * is not implemented, Safe_State must recover and the green LED must turn
 * back ON on its own.
 */
static void setupImplausAutoRecovery() {
  setupStuckAirP();
  delay(500); // implausibility present and visible to the DUT

  // Remove the fault: back to the full safe state, DUT stays powered
  driveInput(CH_AIR_P_COIL, COIL_OPEN_V);
  driveInput(CH_SDC_END, SDC_OPEN_V);
}

// --- Red LED independence ----------------------------------------------------

/**
 * "TS active" pattern on every green-circuit input: HV flagged present, all
 * relays closed, shutdown closed. Since no real HV is connected to HV+, the
 * red LED must remain OFF regardless of the green-circuit inputs.
 */
static void setupTsActiveInputs() {
  setupSafeState();
  driveInput(CH_HV_ACCU_STATE, HV_ACCU_HV_PRES_V);
  driveInput(CH_AIR_P_AUX2, AUX2_CLOSED_V);
  driveInput(CH_AIR_M_AUX2, AUX2_CLOSED_V);
  driveInput(CH_PRE_AUX2, AUX2_CLOSED_V);
  driveInput(CH_AIR_P_COIL, COIL_CLOSED_V);
  driveInput(CH_AIR_M_COIL, COIL_CLOSED_V);
  driveInput(CH_PRE_COIL, COIL_CLOSED_V);
  driveInput(CH_SDC_END, SDC_CLOSED_V);
}

// ============================================================================
// TEST CASES
// ============================================================================

static void addLedTest(const char *name, DUTSetupFn setupFn, uint8_t hpMask,
                       float expectedV) {
  TestCase tc;
  tc.name = name;
  tc.type = TEST_STATIC_VOLTAGE;
  tc.dutSetup = setupFn;
  tc.staticVoltage = {.senseChannelMask = 0,
                      .expectedVoltage = expectedV,
                      .toleranceVolts = LED_TOL_V,
                      .settleMs = 300,
                      .senseHPChannelMask = hpMask};
  runner.addTest(tc);
}

static void configureTests() {
  // -----------------------------------------------------------------------
  // 1-2: Safe state -> TS_is_OFF=1, Safe_State=1 -> green ON, red OFF
  // -----------------------------------------------------------------------
  addLedTest("Safe: TS_is_OFF=1 Green ON", setupSafeState, MASK_HP_GREEN,
             LED_ON_V);
  addLedTest("Safe: Red_On=0 Red OFF", setupSafeState, MASK_HP_RED, LED_OFF_V);

  // -----------------------------------------------------------------------
  // 3-6: TS state violations -> TS_is_OFF=0 -> green OFF
  // -----------------------------------------------------------------------
  addLedTest("HV pres: TS_is_OFF=0 Grn OFF", setupHVPresent, MASK_HP_GREEN,
             LED_OFF_V);
  addLedTest("AIR+ cls: TS_is_OFF=0 GrnOFF", setupAirPClosed, MASK_HP_GREEN,
             LED_OFF_V);
  addLedTest("AIR- cls: TS_is_OFF=0 GrnOFF", setupAirMClosed, MASK_HP_GREEN,
             LED_OFF_V);
  addLedTest("PRE cls: TS_is_OFF=0 Grn OFF", setupPreClosed, MASK_HP_GREEN,
             LED_OFF_V);

  // -----------------------------------------------------------------------
  // 7-9: Stuck relays (EV4.10.13) -> Safe_State=0 -> green OFF
  // -----------------------------------------------------------------------
  addLedTest("AIR+_Stuck=1 Green OFF", setupStuckAirP, MASK_HP_GREEN,
             LED_OFF_V);
  addLedTest("AIR-_Stuck=1 Green OFF", setupStuckAirM, MASK_HP_GREEN,
             LED_OFF_V);
  addLedTest("PRECHARGE_Stuck=1 Green OFF", setupStuckPre, MASK_HP_GREEN,
             LED_OFF_V);

  // -----------------------------------------------------------------------
  // 10-12: RelayClosed_but_No_Voltage (EV4.10.14) -> green OFF
  // -----------------------------------------------------------------------
  addLedTest("RlyClsdNoVolt AIR+ Grn OFF", setupRlyClosedNoVoltAirP,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("RlyClsdNoVolt AIR- Grn OFF", setupRlyClosedNoVoltAirM,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("RlyClsdNoVolt PRE Grn OFF", setupRlyClosedNoVoltPre,
             MASK_HP_GREEN, LED_OFF_V);

  // -----------------------------------------------------------------------
  // 13-16: SCS_Failure, short to GND (T11.9.2) -> green OFF
  // -----------------------------------------------------------------------
  addLedTest("SCS AIR+AUX2 gnd: Green OFF", setupScsShortAirPAux2,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("SCS AIR-AUX2 gnd: Green OFF", setupScsShortAirMAux2,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("SCS PREAUX2 gnd: Green OFF", setupScsShortPreAux2,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("SCS HVAccu gnd: Green OFF", setupScsShortHvAccu, MASK_HP_GREEN,
             LED_OFF_V);

  // -----------------------------------------------------------------------
  // 17-20: SCS_Failure, open circuit (T11.9.2) -> green OFF
  // -----------------------------------------------------------------------
  addLedTest("SCS AIR+AUX2 open: Grn OFF", setupScsOpenAirPAux2,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("SCS AIR-AUX2 open: Grn OFF", setupScsOpenAirMAux2,
             MASK_HP_GREEN, LED_OFF_V);
  addLedTest("SCS PREAUX2 open: Grn OFF", setupScsOpenPreAux2, MASK_HP_GREEN,
             LED_OFF_V);
  addLedTest("SCS HVAccu open: Grn OFF", setupScsOpenHvAccu, MASK_HP_GREEN,
             LED_OFF_V);

  // -----------------------------------------------------------------------
  // 21: No latching -> green LED auto-recovers when implausibility clears
  // -----------------------------------------------------------------------
  addLedTest("Implaus clear: Green auto-ON", setupImplausAutoRecovery,
             MASK_HP_GREEN, LED_ON_V);

  // -----------------------------------------------------------------------
  // 22-23: Red_Circuit independence (no HV connected -> red always OFF)
  // -----------------------------------------------------------------------
  addLedTest("Implaus: Red_On=0 Red OFF", setupStuckAirP, MASK_HP_RED,
             LED_OFF_V);
  addLedTest("TS inputs ON: Red OFF", setupTsActiveInputs, MASK_HP_RED,
             LED_OFF_V);
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println(F("\n=== TSAL Testbench Initializing ==="));

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
    lvlpChannels[i].setLimits(12.0f, 0.05f);
  }

  // GreenLed (HPCH1/CH9) and RedLed (HPCH2/CH10): the LED lines are pulled
  // to the fixed 12 V rail through these channels, so both PMOS must conduct
  // for the whole campaign to sense the open-drain DUT outputs.
  for (uint8_t i = 0; i < 2; i++) {
    hpChannels[i].init();
    hpChannels[i].setLimits(15.0f, 1.0f);
    hpChannels[i].connect();
  }

  // CH11 (HV channel relay) switches the DUT GND return; closed once for
  // the whole campaign right before the tests start.
  hvChannels[0].init();
  hvChannels[0].setLimits(80.0f);

  buildChannelPointerArrays();
  runner.begin(chPtrs, 8);
  runner.bindHPChannels(hpPtrs, 2);
  configureTests();

  while (digitalRead(pinEncoderSw));

  // Power the DUT once for the whole campaign (no latching -> no
  // power-cycling between tests needed).
  hvChannels[0].connect();
  delay(300);

  Serial.println(F("=== Running TSAL Tests ===\n"));
  runner.runAll(&u8g2);

  // Leave the DUT unpowered when the campaign ends
  hvChannels[0].disconnect();

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
