/**
 * @file test_pwm.cpp
 * @brief Dedicated PWM generation test for LVLP channels CH1-CH4.
 *
 * Wiring required:
 *   Oscilloscope probe on pinCtrl1 (GPIO 14) for CH1,
 *                          pinCtrl2 (GPIO 15) for CH2,
 *                          pinCtrl3 (GPIO 16) for CH3,
 *                          pinCtrl4 (GPIO 10) for CH4.
 *
 * Serial commands (115200 baud):
 *   v<volts>   Set DAC output voltage amplitude  e.g. "v5.0"
 *   d<0-max>   Set duty cycle (max = 2^bits-1)   e.g. "d128" (8-bit) or "d512" (10-bit)
 *   r<1-14>    Set PWM resolution in bits         e.g. "r10"
 *   f<Hz>      Set frequency                     e.g. "f1000"
 *   c<1-4>     Select channel                    e.g. "c1"
 *   s          Sweep duty cycle continuously
 *   p          Pause sweep / hold current duty
 *   ?          Print status
 *
 * On boot: CH1 outputs 5 V amplitude, 50% duty at 1 kHz automatically.
 *
 * NOTE: setOutputVoltage() programs the DAC, which sets the HIGH-level
 * voltage the op-amp drives. The LEDC PWM pin then switches that output
 * on/off at the chosen frequency. Both calls are required for any signal
 * to appear on the channel output.
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"

// ─── Constants ───────────────────────────────────────────────────────────────
static constexpr uint32_t kDefaultFrequency  = 1000; // 1 kHz
static constexpr uint8_t  kDefaultResolution = 8;    // bits (1-14)
static constexpr float    kDefaultVoltage    = 5.0f; // DAC amplitude (V)

// Channels with a PWM-capable control pin (indices 0-3, i.e. CH1-CH4)
static constexpr uint8_t kPwmChannelCount = 4;

// ─── State ───────────────────────────────────────────────────────────────────
static uint8_t   gSelectedChannel = 0;              // 0-based index (0 = CH1)
static uint8_t   gPwmResolution   = kDefaultResolution;
static uint16_t  gDutyCycle       = (1u << kDefaultResolution) / 2; // 50 %
static uint32_t  gFrequency       = kDefaultFrequency;
static float     gOutputVoltage   = kDefaultVoltage; // DAC amplitude in Volts
static bool      gSweeping        = false;
static uint16_t  gSweepDuty       = 0;
static uint32_t  gLastSweepMs     = 0;

// Helper: max duty value for the current resolution
static inline uint16_t maxDuty() { return (1u << gPwmResolution) - 1u; }

// ─── Helpers ─────────────────────────────────────────────────────────────────
static void printStatus() {
    LVLPChannel *ch = &lvlpChannels[gSelectedChannel];
    int8_t pwmPin = -1;
    // Retrieve pwmPin from known mapping (mirrors hardwareIOSetup.h)
    const int8_t pwmPins[4] = { pinCtrl1, pinCtrl2, pinCtrl3, pinCtrl4 };
    pwmPin = pwmPins[gSelectedChannel];

    Serial.println(F("─────────────────────────────────"));
    Serial.printf ("Selected Channel : CH%u (GPIO %d)\n", gSelectedChannel + 1, pwmPin);
    Serial.printf ("Output Voltage   : %.3f V (DAC amplitude)\n", gOutputVoltage);
    Serial.printf ("Resolution       : %u bit  (max duty = %u)\n", gPwmResolution, maxDuty());
    Serial.printf ("Frequency        : %lu Hz\n", (unsigned long)gFrequency);
    Serial.printf ("Duty cycle       : %u / %u  (%.1f %%)\n",
                   gDutyCycle, maxDuty(), gDutyCycle * 100.0f / maxDuty());
    Serial.printf ("Mode             : %s\n",
                   ch->getMode() == MODE_PWM_GENERATOR ? "PWM_GENERATOR" : "OTHER");
    Serial.printf ("Status           : %s\n",
                   ch->getStatus() == STATUS_NORMAL ? "NORMAL" : "FAULT");
    Serial.printf ("Sweep            : %s\n", gSweeping ? "ON" : "OFF");
    Serial.println(F("─────────────────────────────────"));
}

static bool applyPwm() {
    LVLPChannel *ch = &lvlpChannels[gSelectedChannel];

    // Ensure the channel is in PWM mode (setMode returns false if
    // pwmPin == -1, which means CH5-CH8 cannot be selected).
    if (!ch->setMode(MODE_PWM_GENERATOR)) {
        Serial.printf("ERROR: CH%u does not support PWM or is in a fault state.\n",
                      gSelectedChannel + 1);
        return false;
    }

    // Apply the desired resolution before calling setPwm().
    ch->setPwmResolution(gPwmResolution);

    // The DAC sets the HIGH-level voltage the op-amp drives.
    // Without this call the DAC stays at 0 V (set by init()) and
    // the output remains silent regardless of the LEDC duty cycle.
    ch->setOutputVoltage(gOutputVoltage);

    if (!ch->setPwm(gDutyCycle, gFrequency)) {
        Serial.printf("ERROR: setPwm() failed on CH%u. "
                      "Check that mode is MODE_PWM_GENERATOR.\n",
                      gSelectedChannel + 1);
        return false;
    }
    return true;
}

static void parseCommand(const String &cmd) {
    if (cmd.length() == 0) return;

    char first = cmd.charAt(0);

    if (first == 'v') {
        // Output voltage (DAC amplitude): v<volts>  e.g. "v5.0"
        float val = cmd.substring(1).toFloat();
        if (val < 0.0f || val > 11.0f) {
            Serial.println("Voltage must be 0.0 - 11.0 V");
            return;
        }
        gOutputVoltage = val;
        if (applyPwm()) {
            Serial.printf("Output voltage set to %.3f V\n", gOutputVoltage);
        }

    } else if (first == 'r') {
        // Resolution: r<1-14>  e.g. "r10"
        int val = cmd.substring(1).toInt();
        if (val < 1 || val > 14) {
            Serial.println("Resolution must be 1-14 bits");
            return;
        }
        gPwmResolution = (uint8_t)val;
        // Clamp current duty to the new max so it stays in range
        uint16_t newMax = maxDuty();
        if (gDutyCycle > newMax) gDutyCycle = newMax;
        if (gSweepDuty > newMax) gSweepDuty = 0;
        if (applyPwm()) {
            Serial.printf("Resolution set to %u bit (max duty = %u)\n",
                          gPwmResolution, newMax);
        }

    } else if (first == 'd') {
        // Duty cycle: d<0-max>  where max = 2^resolution - 1
        long val = cmd.substring(1).toInt();
        val = constrain(val, 0L, (long)maxDuty());
        gDutyCycle = (uint16_t)val;
        gSweeping  = false;
        if (applyPwm()) {
            Serial.printf("Duty set to %u / %u (%.1f %%)\n",
                          gDutyCycle, maxDuty(), gDutyCycle * 100.0f / maxDuty());
        }

    } else if (first == 'f') {
        // Frequency: f<Hz>
        long val = cmd.substring(1).toInt();
        if (val < 1) {
            Serial.println("Frequency must be >= 1 Hz");
            return;
        }
        gFrequency = (uint32_t)val;
        if (applyPwm()) {
            Serial.printf("Frequency set to %lu Hz\n", (unsigned long)gFrequency);
        }

    } else if (first == 'c') {
        // Channel select: c<1-4>
        int ch = cmd.substring(1).toInt();
        if (ch < 1 || ch > (int)kPwmChannelCount) {
            Serial.printf("Channel must be 1-%u\n", kPwmChannelCount);
            return;
        }
        // Park the old channel in high-impedance
        lvlpChannels[gSelectedChannel].setMode(MODE_HIGH_IMPEDANCE);

        gSelectedChannel = (uint8_t)(ch - 1);
        if (applyPwm()) {
            Serial.printf("Switched to CH%u\n", ch);
            printStatus();
        }

    } else if (first == 's') {
        gSweeping   = true;
        gSweepDuty  = 0;
        Serial.println("Sweep started. Send 'p' to pause.");

    } else if (first == 'p') {
        gSweeping = false;
        Serial.println("Sweep paused.");

    } else if (first == '?') {
        printStatus();

    } else {
        Serial.println(F("Unknown command. Valid: v<V>  r<1-14>  d<0-max>  f<Hz>  c<1-4>  s  p  ?"));
    }
}

// ─── Arduino lifecycle ────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== LVLP PWM Generator Test ==="));

    // Shift-register output enable (active LOW)
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllLow();

    // I2C + DAC
    Wire.begin(pinI2cSda, pinI2cScl);
    if (!initializeMCP4728()) {
        Serial.println(F("WARNING: DAC init failed – voltage-source functions unavailable."));
    }

    // SPI + ADC
    if (!initializeMCP3208()) {
        Serial.println(F("WARNING: ADC init failed."));
    }

    // LEDs
    initializeWS2812B();

    // Initialise all LVLP channels (sets DAC to 0, drives control pins LOW)
    for (uint8_t i = 0; i < 8; i++) {
        lvlpChannels[i].init();
    }

    // ── Apply default PWM on CH1 ──────────────────────────────────────────
    Serial.printf("Applying default PWM: CH%u  volt=%.2fV  res=%ubit  duty=%u/%u  freq=%lu Hz\n",
                  gSelectedChannel + 1, gOutputVoltage,
                  gPwmResolution, gDutyCycle, maxDuty(),
                  (unsigned long)gFrequency);

    if (applyPwm()) {
        Serial.println(F("PWM running. Observe channel output on oscilloscope."));
    } else {
        Serial.println(F("ERROR: Could not start PWM on CH1!"));
    }

    printStatus();

    Serial.println(F("\nSerial commands: v<V>  r<1-14>  d<0-max>  f<Hz>  c<1-4>  s(weep)  p(ause)  ?"));
}

void loop() {
    // ── Serial command parser ─────────────────────────────────────────────
    static String inputBuf;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            inputBuf.trim();
            if (inputBuf.length()) {
                parseCommand(inputBuf);
            }
            inputBuf = "";
        } else {
            inputBuf += c;
        }
    }

    // ── Duty-cycle sweep (increments every 20 ms) ─────────────────────────
    // Full sweep duration ≈ maxDuty() * 20 ms  (e.g. 5 s at 8-bit, 20 s at 10-bit)
    if (gSweeping) {
        uint32_t now = millis();
        if (now - gLastSweepMs >= 20) {
            gLastSweepMs = now;
            gDutyCycle   = gSweepDuty;

            // v2.x LEDC API: ledcWrite takes (channel, duty).
            // Channel mapping mirrors the switch-case in LVLPChannel::setPwm().
            const uint8_t ledcChannels[4] = { 0, 1, 2, 3 };
            ledcWrite(ledcChannels[gSelectedChannel], gSweepDuty);

            // Advance and wrap at the correct max for the current resolution.
            if (gSweepDuty >= maxDuty()) {
                gSweepDuty = 0;
            } else {
                gSweepDuty++;
            }
        }
    }
}
