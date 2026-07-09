/**
 * @file    test_HPChannel.cpp
 * @brief   Interactive test and calibration routine for both HP Channels (HPCH).
 *
 * Build environment: test_HPChannel  (platformio.ini)
 * Baud rate        : 115200
 *
 * ── Serial command menu ────────────────────────────────────────────────────
 *
 *   Z  – Calibrate ACS725 zero-current offset (both channels)
 *
 *   A  – Collect ADC calibration POINT 0 (low-voltage reference)
 *         You will be prompted to enter the known VIn and VOut voltages.
 *
 *   B  – Collect ADC calibration POINT 1 (high-voltage reference)
 *         After this point the linear coefficients are computed.
 *
 *   S  – Calibrate ACS725 sensitivity with a known current
 *         You will be prompted for the channel index and current value.
 *
 *   P  – Print calibration block for copy-paste into hardwareIOSetup.h
 *
 *   C  – Connect / disconnect toggle for a selected channel
 *
 *   R  – Reset channel fault status
 *
 *   D  – Print full debug info for both channels
 *
 *   H  – Show this help text
 *
 * ── Encoder button ─────────────────────────────────────────────────────────
 *   Short press  – toggle connection on HPCH1
 * ───────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"

// ============================================================================
// Constants
// ============================================================================
constexpr uint8_t kNumHPChannels = 2;

// ============================================================================
// State
// ============================================================================
static bool channelConnected[kNumHPChannels] = {false, false};

// ============================================================================
// Helper — blocking Serial float prompt
// ============================================================================
/**
 * @brief Blocks until the user types a floating-point number over Serial.
 *
 * Displays the given prompt, then waits for a newline-terminated number.
 * Accepts negative numbers and decimals.
 */
static float promptFloat(const char* prompt) {
    Serial.print(prompt);
    Serial.flush();

    // Drain any residual newlines
    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n' && c != '\r') {
            // Put it back would be ideal but we can't — just discard stale data
        }
    }

    String input = "";
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (input.length() > 0) break;
            } else {
                Serial.print(c); // local echo
                input += c;
            }
        }
    }
    Serial.println();
    return input.toFloat();
}

/**
 * @brief Blocks until the user types an integer over Serial.
 */
static int promptInt(const char* prompt) {
    return static_cast<int>(promptFloat(prompt));
}

// ============================================================================
// Help text
// ============================================================================
static void printHelp() {
    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println(  "║         HP Channel Test — Command Menu       ║");
    Serial.println(  "╠══════════════════════════════════════════════╣");
    Serial.println(  "║  Z  ACS725 zero-offset calibration           ║");
    Serial.println(  "║  A  ADC cal point 0 (low  voltage)           ║");
    Serial.println(  "║  B  ADC cal point 1 (high voltage)           ║");
    Serial.println(  "║  S  ACS725 sensitivity calibration           ║");
    Serial.println(  "║  P  Print calibration block for paste        ║");
    Serial.println(  "║  C  Connect/disconnect channel               ║");
    Serial.println(  "║  R  Reset channel fault                      ║");
    Serial.println(  "║  D  Debug info (both channels)               ║");
    Serial.println(  "║  H  Show this help                           ║");
    Serial.println(  "╚══════════════════════════════════════════════╝\n");
}

// ============================================================================
// Calibration helpers
// ============================================================================
static void runACS725ZeroOffset() {
    Serial.println("\n--- ACS725 Zero-Offset Calibration ---");
    Serial.println("Make sure NO current is flowing through the HP channels.");
    Serial.println("Press ENTER when ready...");
    while (!Serial.available()) { delay(10); }
    while (Serial.available()) Serial.read(); // flush

    for (uint8_t i = 0; i < kNumHPChannels; i++) {
        hpChannels[i].calibrateACS725ZeroOffset();
    }
    Serial.println("ACS725 zero-offset calibration complete.\n");
}

static void runADCCalPoint(uint8_t pointIndex) {
    Serial.printf("\n--- ADC Calibration Point %u (%s voltage) ---\n",
                  pointIndex, pointIndex == 0 ? "LOW" : "HIGH");
    Serial.printf("Apply a stable, known voltage to both HP channels' inputs, "
                  "then measure VIn and VOut with a multimeter.\n");

    for (uint8_t i = 0; i < kNumHPChannels; i++) {
        Serial.printf("\nHPCH%u:\n", i + 1);

        char promptVIn[64], promptVOut[64];
        snprintf(promptVIn,  sizeof(promptVIn),  "  Enter measured VIn  (V) for HPCH%u: ", i + 1);
        snprintf(promptVOut, sizeof(promptVOut), "  Enter measured VOut (V) for HPCH%u: ", i + 1);

        float knownVIn  = promptFloat(promptVIn);
        float knownVOut = promptFloat(promptVOut);

        bool done = hpChannels[i].calibrateADCPoint(pointIndex, knownVIn, knownVOut);
        if (done) {
            Serial.printf("HPCH%u ADC calibration complete — coefficients updated.\n", i + 1);
        }
    }
}

static void runACS725Sensitivity() {
    Serial.println("\n--- ACS725 Sensitivity Calibration ---");
    Serial.println("The channel must be actively carrying a known, stable current.");

    int chIdx = promptInt("Enter channel to calibrate (1 or 2): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHPChannels) {
        Serial.println("Invalid channel.");
        return;
    }

    float knownI = promptFloat("Enter known current (A): ");
    bool ok = hpChannels[chIdx].calibrateACS725Sensitivity(knownI);
    if (ok) {
        Serial.printf("HPCH%u sensitivity calibration successful.\n", chIdx + 1);
    } else {
        Serial.printf("HPCH%u sensitivity calibration FAILED.\n", chIdx + 1);
    }
}

static void runPrintCalibrationBlock() {
    Serial.println();
    for (uint8_t i = 0; i < kNumHPChannels; i++) {
        hpChannels[i].printCalibrationBlockForPaste();
    }
}

static void runToggleConnection() {
    int chIdx = promptInt("Enter channel to toggle (1 or 2): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHPChannels) {
        Serial.println("Invalid channel.");
        return;
    }

    if (hpChannels[chIdx].getStatus() != HPCH_STATUS_NORMAL) {
        Serial.printf("HPCH%u has a fault — reset it first (R command).\n", chIdx + 1);
        return;
    }

    channelConnected[chIdx] = !channelConnected[chIdx];
    if (channelConnected[chIdx]) {
        hpChannels[chIdx].connect();
        Serial.printf("HPCH%u CONNECTED\n", chIdx + 1);
    } else {
        hpChannels[chIdx].disconnect();
        Serial.printf("HPCH%u DISCONNECTED\n", chIdx + 1);
    }
}

static void runResetFault() {
    int chIdx = promptInt("Enter channel to reset (1 or 2): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHPChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    hpChannels[chIdx].resetStatus();
    channelConnected[chIdx] = false;
    Serial.printf("HPCH%u fault cleared.\n", chIdx + 1);
}

static void runDebugInfo() {
    for (uint8_t i = 0; i < kNumHPChannels; i++) {
        hpChannels[i].update();
        hpChannels[i].printDebugInfo();
    }
}

// ============================================================================
// Process a single incoming character command
// ============================================================================
static void processCommand(char cmd) {
    switch (cmd) {
        case 'z': case 'Z': runACS725ZeroOffset();      break;
        case 'a': case 'A': runADCCalPoint(0);          break;
        case 'b': case 'B': runADCCalPoint(1);          break;
        case 's': case 'S': runACS725Sensitivity();     break;
        case 'p': case 'P': runPrintCalibrationBlock(); break;
        case 'c': case 'C': runToggleConnection();      break;
        case 'r': case 'R': runResetFault();            break;
        case 'd': case 'D': runDebugInfo();             break;
        case 'h': case 'H': printHelp();                break;
        case '\n': case '\r': break; // Ignore bare newlines
        default:
            Serial.printf("Unknown command '%c'. Press H for help.\n", cmd);
            break;
    }
}

// ============================================================================
// setup()
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(2000); // Allow serial monitor to connect

    Serial.println("\n==============================");
    Serial.println("  HP Channel Test — Starting  ");
    Serial.println("==============================");

    // ── Hardware init ───────────────────────────────────────────────────────
    Wire.begin(pinI2cSda, pinI2cScl);
    SPI.begin(pinSpiClk, pinSpiMiso, pinSpiMosi);

    // Shift register — disable output enable initially
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllLow();

    // LEDs
    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear();
    FastLED.show();

    // ADC
    if (!initializeMCP3208()) {
        Serial.println("[WARN] MCP3208 init reported issues — continuing anyway.");
    }

    // Encoder button (used for quick connect toggle)
    pinMode(pinEncoderSw, INPUT_PULLUP);

    // ── HP Channels ─────────────────────────────────────────────────────────
    Serial.println("\nInitialising HP Channels...");
    for (uint8_t i = 0; i < kNumHPChannels; i++) {
        hpChannels[i].init();
        hpChannels[i].setLimits(10.0f, 1.0f); // 25 V / 10 A default limits
        Serial.printf("  HPCH%u initialised.\n", i + 1);
    }

    // ── Auto zero-offset on boot ────────────────────────────────────────────
    Serial.println("\nRunning ACS725 zero-offset calibration on boot (channels open)...");
    for (uint8_t i = 0; i < kNumHPChannels; i++) {
        hpChannels[i].calibrateACS725ZeroOffset();
    }

    printHelp();
    Serial.println("Ready. Type a command above, or wait for the monitoring loop.\n");
}

// ============================================================================
// loop()
// ============================================================================
void loop() {
    // ── Serial command handler ───────────────────────────────────────────────
    if (Serial.available()) {
        char cmd = Serial.read();
        processCommand(cmd);
    }

    // ── Encoder button — short press toggles HPCH1 ──────────────────────────
    static bool lastBtnState = HIGH;
    bool btnState = digitalRead(pinEncoderSw);
    if (lastBtnState == HIGH && btnState == LOW) {
        // Falling edge detected
        delay(20); // Simple debounce
        if (digitalRead(pinEncoderSw) == LOW) {
            if (hpChannels[0].getStatus() != HPCH_STATUS_NORMAL) {
                hpChannels[0].resetStatus();
                channelConnected[0] = false;
                Serial.println("[BTN] HPCH1 fault reset.");
            } else {
                channelConnected[0] = !channelConnected[0];
                if (channelConnected[0]) {
                    hpChannels[0].connect();
                    Serial.println("[BTN] HPCH1 CONNECTED");
                } else {
                    hpChannels[0].disconnect();
                    Serial.println("[BTN] HPCH1 DISCONNECTED");
                }
            }
        }
    }
    lastBtnState = btnState;

    // ── Periodic monitoring — every 1 s ─────────────────────────────────────
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();

        for (uint8_t i = 0; i < kNumHPChannels; i++) {
            hpChannels[i].update();

            HPCHStatus st = hpChannels[i].getStatus();
            const char* stStr = "NORMAL";
            if (st == HPCH_STATUS_FAIL_OVERCURRENT) stStr = "OVERCURRENT";
            else if (st == HPCH_STATUS_FAIL_OVERVOLTAGE) stStr = "OVERVOLTAGE";
            else if (st == HPCH_STATUS_FAIL_OTHER)       stStr = "OTHER";

            Serial.printf("HPCH%u | %-11s | VIn=%6.3fV  VOut=%6.3fV  I=%7.4fA\n",
                          i + 1,
                          stStr,
                          hpChannels[i].readVIn(),
                          hpChannels[i].readVOut(),
                          hpChannels[i].readCurrent());

            // Warn if a fault just occurred
            if (st != HPCH_STATUS_NORMAL) {
                Serial.printf("[!] HPCH%u FAULT: %s — use R to reset.\n",
                              i + 1, stStr);
            }
        }
        Serial.println("------");
    }
}
