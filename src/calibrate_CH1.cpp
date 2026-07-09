/**
 * @file calibrate_CH1.cpp
 * @brief External absolute calibration routine for LVLP Channel 1.
 *
 * This sketch is enabled via the [env:calibrate_CH1] PlatformIO environment.
 * It guides the operator through three calibration phases using a calibrated
 * voltage source and a digital multimeter connected to the PCB:
 *
 *  Phase 1 — ADC readback (mADC, bADC)
 *    CH1 SSR is OPEN (high impedance). Apply known voltages directly to the
 *    ADC measurement node and enter the multimeter reading for each point.
 *
 *  Phase 2 — DAC->Output transfer function (K2, offset)
 *    CH1 SSR is CLOSED (voltage source mode). Firmware commands known DAC
 *    raw values; operator measures the actual output with the multimeter.
 *
 *  Phase 3 — DAC raw mapping (mDAC, bDAC) [optional]
 *    Operator measures the DAC output pin voltage directly with a multimeter
 *    to correct for reference/VCC deviations.
 *
 *  Verification — confirms the freshly derived calibration is consistent.
 *
 * At the end the complete chCalData[0] initialiser is printed ready for
 * copy-paste into hardwareIOSetup.h.
 *
 * Loop serial menu (type letter + Enter):
 *   r  -- re-run full calibration
 *   v  -- re-run verification only
 *   p  -- print current calibration block
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"

// ============================================================================
// Configuration constants
// ============================================================================

/** Vesp -- the ESP32 3.3 V rail voltage used in the K1 formula. */
static constexpr float kVesp = 3.269f;

/** Current limit applied to CH1 during Phase 2 (A). */
static constexpr float kCalCurrentLimit = 0.05f;   // 50 mA
/** Voltage limit for CH1 during calibration (V). */
static constexpr float kCalVoltageLimit = 11.5f;

// ADC calibration setpoints (V) -- user must apply these with the voltage source.
static const float kAdcSetpoints[] = { 0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f };
static constexpr uint8_t kAdcNumPoints =
    (uint8_t)(sizeof(kAdcSetpoints) / sizeof(kAdcSetpoints[0]));

// DAC voltage setpoints used for Phase 2 (V at the MCP4728 output pin).
// Range chosen to cover the op-amp's useful linear region (2.0 – 3.5 V).
// Raw values are computed on the fly from the current mDAC/bDAC.
static const float kDacVoltageSetpoints[] = { 2.0f, 2.375f, 2.75f, 3.125f, 3.5f };
static constexpr uint8_t kDacNumPoints =
    (uint8_t)(sizeof(kDacVoltageSetpoints) / sizeof(kDacVoltageSetpoints[0]));

// Verification voltages (V) -- firmware sets these after calibration and reads back.
static const float kVerifyVoltages[] = { 2.0f, 5.0f, 9.0f };
static constexpr uint8_t kVerifyNumPoints =
    (uint8_t)(sizeof(kVerifyVoltages) / sizeof(kVerifyVoltages[0]));

// ============================================================================
// Helpers
// ============================================================================

/**
 * @brief Print a separator line to Serial.
 */
static void printSeparator(char c = '=', uint8_t len = 56)
{
    for (uint8_t i = 0; i < len; i++) Serial.print(c);
    Serial.println();
}

/**
 * @brief Block until the user types a float value and presses Enter.
 *
 * Discards empty lines and re-prompts on parse failure.
 *
 * @param prompt  Message shown before waiting for input.
 * @return        Parsed float value.
 */
static float waitForFloat(const char* prompt)
{
    while (true)
    {
        Serial.print(prompt);
        Serial.flush();

        // Wait for data
        while (!Serial.available()) { delay(10); }

        String line = Serial.readStringUntil('\n');
        line.trim();

        if (line.length() == 0) continue;   // empty line -- re-prompt

        float value = line.toFloat();

        // toFloat() returns 0.0 on failure; distinguish valid "0" from garbage
        bool seemsZero = (line == "0" || line == "0.0" ||
                          line == "0.00" || line == "0.000" ||
                          line == ".0" || line == "00");
        if (value == 0.0f && !seemsZero)
        {
            Serial.printf("  [!] Could not parse \"%s\" as a number. Try again.\r\n",
                          line.c_str());
            continue;
        }
        return value;
    }
}

/**
 * @brief Block until the user presses Enter (bare or with any text).
 */
static void waitForEnter(const char* prompt)
{
    Serial.println(prompt);
    Serial.flush();
    while (true)
    {
        while (!Serial.available()) { delay(10); }
        Serial.readStringUntil('\n');
        break;
    }
}

/**
 * @brief Simple least-squares linear fit y = slope*x + intercept.
 *
 * @param x         Array of x samples.
 * @param y         Array of y samples.
 * @param n         Number of samples.
 * @param slope     Output slope.
 * @param intercept Output intercept.
 * @return          true on success, false if the fit is degenerate.
 */
static bool linearFit(const float* x, const float* y, uint8_t n,
                       float& slope, float& intercept)
{
    if (n < 2) return false;

    float sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
    for (uint8_t i = 0; i < n; i++)
    {
        sumX  += x[i];
        sumY  += y[i];
        sumXX += x[i] * x[i];
        sumXY += x[i] * y[i];
    }
    float denom = (float)n * sumXX - sumX * sumX;
    if (fabsf(denom) < 1e-9f) return false;

    slope     = ((float)n * sumXY - sumX * sumY) / denom;
    intercept = (sumY - slope * sumX) / (float)n;
    return true;
}

// ============================================================================
// Phase 1 -- ADC calibration  (mADC, bADC)
// ============================================================================

/**
 * @brief Calibrate the MCP3208 ADC readback for CH1.
 *
 * CH1 SSR stays OPEN (high impedance). The operator applies a calibrated
 * voltage source directly to the ADC measurement node (the CH1 output
 * terminal) and enters the actual multimeter reading for each setpoint.
 *
 * After the routine, chCalData[0].mADC and chCalData[0].bADC are updated
 * and applied to lvlpChannels[0].
 *
 * @return true on success.
 */
static bool calibratePhase1_ADC()
{
    printSeparator();
    Serial.println(F("  PHASE 1 -- ADC Readback Calibration (mADC, bADC)"));
    printSeparator();
    Serial.println(F(""));
    Serial.println(F("  CH1 is in HIGH IMPEDANCE mode (SSR open)."));
    Serial.println(F("  Connect your calibrated voltage source AND multimeter"));
    Serial.println(F("  directly to the CH1 output/ADC measurement node on the PCB."));
    Serial.println(F(""));
    Serial.println(F("  For each setpoint you will:"));
    Serial.println(F("    1. Set the voltage source to the requested value."));
    Serial.println(F("    2. Read the actual voltage on your multimeter."));
    Serial.println(F("    3. Press Enter to take the ADC sample."));
    Serial.println(F("    4. Type that multimeter reading and press Enter."));
    printSeparator('-');

    // Ensure CH1 is in high-impedance (SSR open, DAC at 0)
    lvlpChannels[0].resetStatus();
    lvlpChannels[0].setMode(MODE_HIGH_IMPEDANCE);
    lvlpChannels[0].setOutputVoltage(0.0f);

    float measuredV[kAdcNumPoints];
    float rawCounts[kAdcNumPoints];

    for (uint8_t i = 0; i < kAdcNumPoints; i++)
    {
        Serial.printf("\r\n  [Point %u/%u]  Set source to %.1f V\r\n",
                      i + 1, kAdcNumPoints, kAdcSetpoints[i]);

        waitForEnter("  Press Enter when source is stable and ready...");

        // Average 32 ADC readings for noise reduction
        uint32_t rawSum = 0;
        for (uint8_t s = 0; s < 32; s++)
        {
            rawSum += lvlpChannels[0].readMCP3208Value();
            delay(2);
        }
        float rawAvg   = (float)rawSum / 32.0f;
        rawCounts[i]   = rawAvg;

        Serial.printf("  ADC raw (avg 32 samples): %.2f\r\n", rawAvg);

        measuredV[i] = waitForFloat("  Enter multimeter reading (V): ");
        Serial.printf("  -> Point stored: raw=%.2f  Vmeasured=%.4f V\r\n",
                      rawCounts[i], measuredV[i]);
    }

    // Linear fit:  Vmeasured = mADC * raw + bADC
    float mADC, bADC;
    if (!linearFit(rawCounts, measuredV, kAdcNumPoints, mADC, bADC))
    {
        Serial.println(F("\r\n  [ERROR] Linear fit failed. Check your data."));
        return false;
    }

    Serial.printf("\r\n  Fit result:  mADC = %.9f\r\n", mADC);
    Serial.printf(  "               bADC = %.9f\r\n",   bADC);

    // Sanity check (expected ~0.004 to 0.006 V/count for this hardware)
    if (mADC <= 0.0f || mADC > 0.1f)
        Serial.println(F("  [WARNING] mADC value looks unusual. Verify measurements."));

    // Apply immediately so Phase 2 readVoltage() uses the new ADC calibration
    chCalData[0].mADC = mADC;
    chCalData[0].bADC = bADC;
    lvlpChannels[0].setCalibrationData(chCalData[0]);

    Serial.println(F("\r\n  Phase 1 complete -- mADC and bADC updated."));
    return true;
}

// ============================================================================
// Phase 2 -- DAC->Output calibration  (K2, offset)
// ============================================================================

/**
 * @brief Calibrate the DAC-to-output transfer function for CH1.
 *
 * CH1 SSR is CLOSED (voltage source mode). The operator measures the actual
 * CH1 output terminal voltage with a multimeter for each DAC raw setpoint.
 * The current limit is raised to kCalCurrentLimit (50 mA) for this phase.
 *
 * After the routine, chCalData[0].K2 and chCalData[0].offset are updated.
 * K1 is kept at its existing value (hardware constant).
 *
 * @return true on success.
 */
static bool calibratePhase2_DACOutput()
{
    printSeparator();
    Serial.println(F("  PHASE 2 -- DAC to Output Calibration (K2, offset)"));
    printSeparator();
    Serial.println(F(""));
    Serial.println(F("  CH1 will now drive its output (SSR CLOSED)."));
    Serial.println(F("  DISCONNECT the calibrated voltage source used in Phase 1."));
    Serial.println(F("  Keep the multimeter connected to the CH1 OUTPUT terminal."));
    Serial.println(F(""));
    Serial.println(F("  For each DAC setpoint you will:"));
    Serial.println(F("    1. Wait for the firmware to set the DAC value."));
    Serial.println(F("    2. Read and type the actual output voltage."));
    printSeparator('-');

    waitForEnter("  Press Enter when the voltage source is disconnected and ready...");

    // Prepare CH1 as voltage source with raised limits for calibration
    lvlpChannels[0].resetStatus();
    lvlpChannels[0].setLimits(kCalVoltageLimit, kCalCurrentLimit);
    lvlpChannels[0].setMode(MODE_VOLTAGE_SOURCE);

    float dacVoltages[kDacNumPoints];
    float measuredV[kDacNumPoints];

    // Derive raw DAC counts from the voltage setpoints using current mDAC/bDAC.
    // dacRaw = dacVoltage * mDAC + bDAC
    const float mDAC = chCalData[0].mDAC;
    const float bDAC = chCalData[0].bDAC;

    for (uint8_t i = 0; i < kDacNumPoints; i++)
    {
        float    dacVoltage = kDacVoltageSetpoints[i];
        uint16_t dacRaw     = (uint16_t)(dacVoltage * mDAC + bDAC);
        dacVoltages[i]      = dacVoltage;

        // Apply the raw DAC value directly (bypasses the K formula intentionally)
        lvlpChannels[0].setDACOutput(dacRaw);

        Serial.printf("\r\n  [Point %u/%u]  Vdac target = %.3f V  (raw = %u)\r\n",
                      i + 1, kDacNumPoints, dacVoltage, dacRaw);
        Serial.println(F("  Settling 500 ms..."));
        delay(500);

        measuredV[i] = waitForFloat("  Enter multimeter reading at CH1 output (V): ");
        Serial.printf("  -> Point stored: Vdac=%.3f V  Vout=%.4f V\r\n",
                      dacVoltage, measuredV[i]);
    }

    // Safe: disconnect after data collection
    lvlpChannels[0].setMode(MODE_HIGH_IMPEDANCE);
    lvlpChannels[0].setOutputVoltage(0.0f);

    // Linear fit:  Vout = K2 * dacVoltage + (K1*Vesp + offset)
    //   slope     = K2
    //   intercept = K1*Vesp + offset  =>  offset = intercept - K1*Vesp
    float slope, intercept;
    if (!linearFit(dacVoltages, measuredV, kDacNumPoints, slope, intercept))
    {
        Serial.println(F("\r\n  [ERROR] Linear fit failed. Check your data."));
        return false;
    }

    float K2     = slope;
    float K1     = chCalData[0].K1;   // keep existing K1
    float offset = intercept - K1 * kVesp;

    Serial.printf("\r\n  Fit result:  K2     = %.9f\r\n", K2);
    Serial.printf(  "               offset = %.9f\r\n",   offset);

    // Sanity check (expected K2 around -3 to -5)
    if (K2 >= 0.0f || K2 < -10.0f)
        Serial.println(F("  [WARNING] K2 value looks unusual. Verify measurements."));

    chCalData[0].K2     = K2;
    chCalData[0].offset = offset;
    lvlpChannels[0].setCalibrationData(chCalData[0]);

    Serial.println(F("\r\n  Phase 2 complete -- K2 and offset updated."));
    return true;
}

// ============================================================================
// Phase 3 -- DAC raw mapping  (mDAC, bDAC)  [optional]
// ============================================================================

/**
 * @brief Calibrate the raw DAC integer to voltage mapping (mDAC, bDAC).
 *
 * The operator measures the MCP4728 DAC output pin voltage directly with a
 * multimeter for a set of raw values. This corrects for VCC/reference drift.
 *
 * This phase is optional; typing 's' skips it and keeps existing values.
 *
 * @return true on success or user skip.
 */
static bool calibratePhase3_DACRaw()
{
    printSeparator();
    Serial.println(F("  PHASE 3 -- DAC Raw Mapping (mDAC, bDAC)  [OPTIONAL]"));
    printSeparator();
    Serial.println(F(""));
    Serial.println(F("  In this phase you measure the MCP4728 DAC output PIN directly."));
    Serial.println(F("  Connect your multimeter probe to the DAC output pin that"));
    Serial.println(F("  feeds the CH1 op-amp (before the op-amp input)."));
    Serial.println(F(""));
    Serial.println(F("  Type 's' + Enter to SKIP, or press Enter to proceed."));

    // Wait for user decision
    while (!Serial.available()) { delay(10); }
    String choice = Serial.readStringUntil('\n');
    choice.trim();
    choice.toLowerCase();

    if (choice == "s")
    {
        Serial.println(F("  Phase 3 skipped -- existing mDAC and bDAC kept."));
        return true;
    }

    printSeparator('-');
    Serial.println(F("  CH1 SSR remains OPEN -- only the DAC output pin is active."));
    Serial.println(F("  For each DAC raw value, measure the voltage at the DAC pin."));

    // Ensure SSR is open during this phase
    lvlpChannels[0].resetStatus();
    lvlpChannels[0].setMode(MODE_HIGH_IMPEDANCE);

    float dacRawF[kDacNumPoints];
    float dacPinV[kDacNumPoints];

    const float mDAC_cur = chCalData[0].mDAC;
    const float bDAC_cur = chCalData[0].bDAC;

    for (uint8_t i = 0; i < kDacNumPoints; i++)
    {
        float    dacVoltage = kDacVoltageSetpoints[i];
        uint16_t dacRaw     = (uint16_t)(dacVoltage * mDAC_cur + bDAC_cur);
        dacRawF[i]          = (float)dacRaw;

        lvlpChannels[0].setDACOutput(dacRaw);

        Serial.printf("\r\n  [Point %u/%u]  Vdac target = %.3f V  (raw = %u)\r\n",
                      i + 1, kDacNumPoints, dacVoltage, dacRaw);
        Serial.println(F("  Settling 300 ms..."));
        delay(300);

        dacPinV[i] = waitForFloat("  Enter DAC pin voltage (V): ");
        Serial.printf("  -> Point stored: raw=%u  VdacPin=%.4f V\r\n",
                      dacRaw, dacPinV[i]);
    }

    // Return DAC to 0 for safety
    lvlpChannels[0].setDACOutput(0);

    // Linear fit:  dacRaw = mDAC * VdacPin + bDAC
    float mDAC, bDAC;
    if (!linearFit(dacPinV, dacRawF, kDacNumPoints, mDAC, bDAC))
    {
        Serial.println(F("\r\n  [ERROR] Linear fit failed. Check your data."));
        return false;
    }

    Serial.printf("\r\n  Fit result:  mDAC = %.9f\r\n", mDAC);
    Serial.printf(  "               bDAC = %.9f\r\n",   bDAC);

    // Sanity check (expected ~800 counts/V; 4095/5 V = 819)
    if (mDAC < 600.0f || mDAC > 1100.0f)
        Serial.println(F("  [WARNING] mDAC value looks unusual. Verify measurements."));

    chCalData[0].mDAC = mDAC;
    chCalData[0].bDAC = bDAC;
    lvlpChannels[0].setCalibrationData(chCalData[0]);

    Serial.println(F("\r\n  Phase 3 complete -- mDAC and bDAC updated."));
    return true;
}

// ============================================================================
// Verification
// ============================================================================

/**
 * @brief Drive CH1 to known voltages and compare the ADC readback.
 *
 * Prints a table of SET / READ / ERROR so the operator can cross-check with
 * the multimeter against the freshly derived calibration.
 */
static void runVerification()
{
    printSeparator();
    Serial.println(F("  VERIFICATION -- Cross-checking calibration"));
    printSeparator();
    Serial.println(F("  CH1 will be driven to known voltages using the new calibration."));
    Serial.println(F("  Compare the READ column with your multimeter reading."));
    printSeparator('-');

    lvlpChannels[0].resetStatus();
    lvlpChannels[0].setLimits(kCalVoltageLimit, kCalCurrentLimit);
    lvlpChannels[0].setMode(MODE_VOLTAGE_SOURCE);

    Serial.printf("  %-10s  %-11s  %-14s  %s\r\n",
                  "SET (V)", "READ (V)", "ERROR (mV)", "Status");
    printSeparator('-', 52);

    for (uint8_t i = 0; i < kVerifyNumPoints; i++)
    {
        float targetV = kVerifyVoltages[i];
        lvlpChannels[0].setOutputVoltage(targetV);
        delay(400);   // settle

        // Average 16 readings for stability
        float sumV = 0.0f;
        for (uint8_t s = 0; s < 16; s++)
        {
            sumV += lvlpChannels[0].readVoltage();
            delay(2);
        }
        float readV = sumV / 16.0f;
        float errMV = (readV - targetV) * 1000.0f;
        bool  ok    = fabsf(errMV) < 200.0f;   // 200 mV tolerance at this stage

        Serial.printf("  %-10.3f  %-11.4f  %-14.1f  %s\r\n",
                      targetV, readV, errMV, ok ? "OK" : "CHECK");

                      delay(10000);
    }

    // Leave CH1 in a safe state
    lvlpChannels[0].setMode(MODE_HIGH_IMPEDANCE);
    lvlpChannels[0].setOutputVoltage(0.0f);

    printSeparator('-', 52);
    Serial.println(F("  Verification complete."));
    Serial.println(F("  Tip: use your multimeter to cross-check output while the"));
    Serial.println(F("  firmware drives each voltage (add extra delay in loop if needed)."));
}

// ============================================================================
// Print calibration block
// ============================================================================

/**
 * @brief Print the full chCalData[0] initialiser ready for copy-paste into
 *        hardwareIOSetup.h.
 */
static void printCalibrationBlock()
{
    printSeparator();
    Serial.println(F("  RESULT -- copy-paste into hardwareIOSetup.h"));
    printSeparator();
    const ChannelCalibrationData& c = chCalData[0];
    Serial.println(F("ChannelCalibrationData chCalData[8] = {"));
    Serial.printf(  "    { %.9ff, %.9ff, %.9ff,   %.9ff, %.9ff, %.9ff, %.9ff }, // CH1\n",
                  c.K1, c.K2, c.offset,
                  c.mADC, c.bADC,
                  c.mDAC, c.bDAC);
    Serial.println(F("    // CH2..CH8: flash [env:test_LVLPChannel] and press encoder SW at boot"));
    Serial.println(F("};"));
    printSeparator();
}

// ============================================================================
// Full calibration sequence
// ============================================================================

/**
 * @brief Run all three calibration phases followed by verification.
 */
static void runFullCalibration()
{
    Serial.println(F("\r\n"));
    printSeparator('*');
    Serial.println(F("  CH1 EXTERNAL ABSOLUTE CALIBRATION"));
    printSeparator('*');
    Serial.println(F("  Equipment required:"));
    Serial.println(F("    - Calibrated voltage source (0 to 10 V)"));
    Serial.println(F("    - Digital multimeter"));
    Serial.println(F(""));
    Serial.println(F("  Phases:"));
    Serial.println(F("    Phase 1 -- ADC readback   (mADC, bADC)"));
    Serial.println(F("    Phase 2 -- DAC->Output    (K2, offset)"));
    Serial.println(F("    Phase 3 -- DAC raw map    (mDAC, bDAC)  [optional]"));
    printSeparator('*');
    delay(500);

    // bool ok = calibratePhase1_ADC();
    // if (!ok)
    // {
    //     Serial.println(F("[ABORT] Phase 1 failed."));
    //     return;
    // }

    // ok = calibratePhase2_DACOutput();
    // if (!ok)
    // {
    //     Serial.println(F("[ABORT] Phase 2 failed."));
    //     return;
    // }

    // calibratePhase3_DACRaw();   // optional, never aborts

    // printCalibrationBlock();
    runVerification();

    Serial.println(F("\r\n[DONE] Calibration complete."));
    Serial.println(F("Commands: 'r'=re-run  'v'=verify only  'p'=print cal block"));
}

// ============================================================================
// Arduino entry points
// ============================================================================

void setup()
{
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Serial.println(F("\r\n=== CH1 External Calibration Firmware ==="));
    Serial.println(F("    env: calibrate_CH1"));

    // ---- Peripheral initialisation ----------------------------------------

    if (!initializeMCP4728())
        Serial.println(F("[ERROR] DAC initialisation failed! Check I2C wiring."));

    if (!initializeMCP3208())
        Serial.println(F("[ERROR] ADC initialisation failed! Check SPI wiring."));

    initializeWS2812B();

    // Enable shift register (SSR driver); all SSRs start OPEN
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllLow();

    // Initialise only CH1 (index 0)
    lvlpChannels[0].init();
    lvlpChannels[0].setLimits(kCalVoltageLimit, kCalCurrentLimit);

    Serial.println(F("Hardware ready. Starting calibration...\r\n"));
    delay(500);

    runFullCalibration();
}

void loop()
{
    if (Serial.available())
    {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();

        if      (cmd == "r") { runFullCalibration();       }
        else if (cmd == "1") { calibratePhase1_ADC();      }
        else if (cmd == "2") { calibratePhase2_DACOutput();}
        else if (cmd == "3") { calibratePhase3_DACRaw();   }
        else if (cmd == "v") { runVerification();           }
        else if (cmd == "p") { printCalibrationBlock();    }
        else
        {
            Serial.println(F("Commands:"));
            Serial.println(F("  r  = re-run full calibration (phases 1+2+3+verify)"));
            Serial.println(F("  1  = run Phase 1 only  (ADC: mADC, bADC)"));
            Serial.println(F("  2  = run Phase 2 only  (DAC->Output: K2, offset)"));
            Serial.println(F("  3  = run Phase 3 only  (DAC raw: mDAC, bDAC)"));
            Serial.println(F("  v  = verification only"));
            Serial.println(F("  p  = print calibration block"));
        }
    }
}
