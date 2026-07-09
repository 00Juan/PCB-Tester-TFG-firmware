/**
 * @file    test_HVChannel.cpp
 * @brief   Interactive test and calibration routine for HV Channels.
 *
 * Build environment: test_HVChannel  (platformio.ini)
 * Baud rate        : 115200
 *
 * ── Serial command menu ────────────────────────────────────────────────────
 *
 *   K  – Reset calibration table (start fresh)
 *
 *   Z  – Calibrate dead-zone boundary
 *         Apply a known voltage just above the dead-zone (~31–35 V) and
 *         enter the exact value when prompted.
 *
 *   A  – Add one calibration anchor point
 *         Apply a stable known voltage and enter its value.
 *         Repeat for as many voltage steps as desired (more = better accuracy).
 *
 *   F  – Finalise calibration (sort table, validate)
 *
 *   P  – Print calibration block for copy-paste into hardwareIOSetup.h
 *
 *   C  – Connect / disconnect toggle for a selected channel
 *
 *   R  – Reset channel fault status
 *
 *   L  – Set overvoltage limit for a channel
 *
 *   D  – Print full debug info for all HV channels
 *
 *   V  – Read and print voltage (single shot)
 *
 *   W  – Read and print raw ADC value (single shot)
 *
 *   H  – Show this help text
 *
 * ── Monitoring loop ─────────────────────────────────────────────────────────
 *   Voltage is printed every 1 s for all channels.
 * ────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"

// ============================================================================
// Constants
// ============================================================================
constexpr uint8_t kNumHVChannels = 1; // Adjust to match your hardware

// ============================================================================
// State
// ============================================================================
static bool channelConnected[kNumHVChannels] = {};

// ============================================================================
// Helper — blocking Serial prompts
// ============================================================================
static float promptFloat(const char* prompt) {
    Serial.print(prompt);
    Serial.flush();

    // Drain stale input
    while (Serial.available()) Serial.read();

    String input = "";
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (input.length() > 0) break;
            } else {
                Serial.print(c);
                input += c;
            }
        }
    }
    Serial.println();
    return input.toFloat();
}

static int promptInt(const char* prompt) {
    return static_cast<int>(promptFloat(prompt));
}

// ============================================================================
// Help text
// ============================================================================
static void printHelp() {
    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println(  "║        HV Channel Test — Command Menu        ║");
    Serial.println(  "╠══════════════════════════════════════════════╣");
    Serial.println(  "║  K  Reset calibration table                  ║");
    Serial.println(  "║  Z  Calibrate dead-zone boundary             ║");
    Serial.println(  "║  A  Add calibration anchor point             ║");
    Serial.println(  "║  F  Finalise calibration                     ║");
    Serial.println(  "║  P  Print calibration block for paste        ║");
    Serial.println(  "║  C  Connect / disconnect channel             ║");
    Serial.println(  "║  R  Reset channel fault                      ║");
    Serial.println(  "║  L  Set overvoltage limit                    ║");
    Serial.println(  "║  D  Debug info (all channels)                ║");
    Serial.println(  "║  V  Read voltage (single shot)               ║");
    Serial.println(  "║  W  Read raw ADC (single shot)               ║");
    Serial.println(  "║  H  Show this help                           ║");
    Serial.println(  "╚══════════════════════════════════════════════╝\n");
}

// ============================================================================
// Command handlers
// ============================================================================

static void runCalibrationReset() {
    int chIdx = promptInt("Enter channel to reset calibration (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    hvChannels[chIdx].calibrationReset();
}

static void runDeadZone() {
    int chIdx = promptInt("Enter channel index (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    Serial.println("Apply a known voltage just above the dead-zone to the HV input.");
    float v = promptFloat("Enter the applied voltage (V): ");
    hvChannels[chIdx].calibrateDeadZone(v);
}

static void runAddPoint() {
    int chIdx = promptInt("Enter channel index (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    float v = promptFloat("Enter the applied voltage (V): ");
    bool ok = hvChannels[chIdx].calibrateAddPoint(v, 64);
    if (!ok) {
        Serial.println("Could not add point — table full.");
    }
}

static void runFinalise() {
    int chIdx = promptInt("Enter channel index (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    bool ok = hvChannels[chIdx].calibrateFinish();
    if (ok) {
        Serial.printf("HVChannel%d calibration finalised.\n", chIdx + 1);
    } else {
        Serial.printf("HVChannel%d calibration INVALID — collect more points.\n", chIdx + 1);
    }
}

static void runPrintCalibrationBlock() {
    for (uint8_t i = 0; i < kNumHVChannels; i++) {
        hvChannels[i].printCalibrationBlockForPaste();
    }
}

static void runToggleConnection() {
    int chIdx = promptInt("Enter channel to toggle (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    if (hvChannels[chIdx].getStatus() != HV_STATUS_NORMAL) {
        Serial.printf("HVChannel%d has a fault — reset it first (R command).\n", chIdx + 1);
        return;
    }
    channelConnected[chIdx] = !channelConnected[chIdx];
    if (channelConnected[chIdx]) {
        hvChannels[chIdx].connect();
        Serial.printf("HVChannel%d CONNECTED\n", chIdx + 1);
    } else {
        hvChannels[chIdx].disconnect();
        Serial.printf("HVChannel%d DISCONNECTED\n", chIdx + 1);
    }
}

static void runResetFault() {
    int chIdx = promptInt("Enter channel to reset (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    hvChannels[chIdx].resetStatus();
    channelConnected[chIdx] = false;
    Serial.printf("HVChannel%d fault cleared.\n", chIdx + 1);
}

static void runSetLimit() {
    int chIdx = promptInt("Enter channel index (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    float lim = promptFloat("Enter overvoltage limit (V): ");
    hvChannels[chIdx].setLimits(lim);
    Serial.printf("HVChannel%d overvoltage limit set to %.2f V.\n", chIdx + 1, lim);
}

static void runDebugInfo() {
    for (uint8_t i = 0; i < kNumHVChannels; i++) {
        hvChannels[i].update();
        hvChannels[i].printDebugInfo();
    }
}

static void runReadVoltage() {
    int chIdx = promptInt("Enter channel index (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    float v = hvChannels[chIdx].readVoltage();
    Serial.printf("HVChannel%d voltage: %.4f V\n", chIdx + 1, v);
}

static void runReadRaw() {
    int chIdx = promptInt("Enter channel index (1-based): ") - 1;
    if (chIdx < 0 || chIdx >= kNumHVChannels) {
        Serial.println("Invalid channel.");
        return;
    }
    uint16_t raw = hvChannels[chIdx].readRaw();
    Serial.printf("HVChannel%d raw ADC: %u\n", chIdx + 1, raw);
}

// ============================================================================
// Command dispatcher
// ============================================================================
static void processCommand(char cmd) {
    switch (cmd) {
        case 'k': case 'K': runCalibrationReset();       break;
        case 'z': case 'Z': runDeadZone();               break;
        case 'a': case 'A': runAddPoint();               break;
        case 'f': case 'F': runFinalise();               break;
        case 'p': case 'P': runPrintCalibrationBlock();  break;
        case 'c': case 'C': runToggleConnection();       break;
        case 'r': case 'R': runResetFault();             break;
        case 'l': case 'L': runSetLimit();               break;
        case 'd': case 'D': runDebugInfo();              break;
        case 'v': case 'V': runReadVoltage();            break;
        case 'w': case 'W': runReadRaw();                break;
        case 'h': case 'H': printHelp();                 break;
        case '\n': case '\r': break;
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
    delay(2000);

    Serial.println("\n==============================");
    Serial.println("  HV Channel Test — Starting  ");
    Serial.println("==============================");

    // ── Hardware init ───────────────────────────────────────────────────────
    Wire.begin(pinI2cSda, pinI2cScl);
    SPI.begin(pinSpiClk, pinSpiMiso, pinSpiMosi);

    // Shift register — enable output
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

    // ── HV Channels ─────────────────────────────────────────────────────────
    Serial.println("\nInitialising HV Channels...");
    for (uint8_t i = 0; i < kNumHVChannels; i++) {
        hvChannels[i].init();
        Serial.printf("  HVChannel%u initialised.\n", i + 1);
    }

    printHelp();
    Serial.println("Ready. Type a command above.\n");
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

    // ── Periodic monitoring — every 1 s ─────────────────────────────────────
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();

        for (uint8_t i = 0; i < kNumHVChannels; i++) {
            hvChannels[i].update();

            HVChannelStatus st = hvChannels[i].getStatus();
            const char* stStr = "NORMAL";
            if (st == HV_STATUS_FAIL_OVERVOLTAGE) stStr = "OVERVOLTAGE";
            else if (st == HV_STATUS_FAIL_OTHER)  stStr = "OTHER";

            Serial.printf("HVCh%u | %-11s | V=%8.3f V | raw=%4u\n",
                          i + 1,
                          stStr,
                          hvChannels[i].readVoltage(),
                          hvChannels[i].readRaw());

            if (st != HV_STATUS_NORMAL) {
                Serial.printf("[!] HVChannel%u FAULT: %s — use R to reset.\n",
                              i + 1, stStr);
            }
        }
        Serial.println("------");
    }
}
