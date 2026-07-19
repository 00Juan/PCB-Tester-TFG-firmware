// ============================================================================
// test_settling.cpp
//
// Measures the LVLP output/ADC settling time after a sudden output-voltage
// step, so the over-current debounce window can be sized from real data.
//
// WHY: readCurrent() computes current = (vBeforeShunt - voutActual)/Rshunt,
// where vBeforeShunt is FEED-FORWARD (jumps instantly with the DAC command)
// but voutActual is the physically-settled ADC reading (lags). During the
// settling window the apparent current spikes and falsely trips the
// over-current protection. This sketch quantifies how long that window is.
//
// The output must be in the same physical state you care about (typically
// open / no DUT, which is the worst case for the false trip). Connect nothing,
// or the real load, to the channel under test.
//
// SERIAL MENU (115200 baud):
//   c<n>   select channel 1..8            (default 1)
//   l<v>   low  voltage of the step       (default 1.0)
//   h<v>   high voltage of the step       (default 10.0)
//   w<ms>  capture window in ms           (default 200)
//   g      run one UP step   (low -> high) and analyse
//   j      run one DOWN step (high -> low) and analyse
//   ?      print help / current settings
//
// The encoder push-button also triggers an UP-step capture.
// ============================================================================

#include <Arduino.h>
#include "hardwareIOSetup.h"

// ----------------------------------------------------------------------------
// Capture buffer
// ----------------------------------------------------------------------------
constexpr uint16_t kMaxSamples = 600; // 600 * 6 B ~= 3.6 KB

struct Sample {
  uint32_t tUs;  // time since the step command was issued
  float vout;    // ADC-measured output voltage
};

static Sample gSamples[kMaxSamples];
static uint16_t gSampleCount = 0;

// ----------------------------------------------------------------------------
// Test parameters (adjustable over serial)
// ----------------------------------------------------------------------------
static uint8_t gChannel = 0;      // 0-based index (CH1)
static float gLowVolts = 1.0f;
static float gHighVolts = 10.0f;
static uint32_t gWindowMs = 200;

// The channel-update period used by the real firmware (app.cpp).
// Used only to translate a settling time into a recommended debounce count.
constexpr uint32_t kUpdatePeriodMs = 50;
constexpr float kShuntOhms = 10.5f;

// Current thresholds (mA) we report the crossing time for, so the window can
// be sized against whatever limit the LVLP channel ends up using.
static const float kCurrentThreshMa[] = {100.0f, 70.0f, 10.0f, 1.0f, 0.1f, 0.01};
constexpr uint8_t kNumThresh = sizeof(kCurrentThreshMa) / sizeof(kCurrentThreshMa[0]);

// ----------------------------------------------------------------------------
// Analysis helpers
// ----------------------------------------------------------------------------

// Time (ms) after which the apparent-current magnitude STAYS below `threshMa`
// for the rest of the capture. Returns -1 if it never settles below.
static float timeBelowCurrent(float vCmd, float threshMa) {
  float threshA = threshMa / 1000.0f;
  float settleUs = -1.0f;
  bool below = false;
  for (uint16_t k = 0; k < gSampleCount; k++) {
    float iApp = (vCmd - gSamples[k].vout) / kShuntOhms;
    if (fabsf(iApp) < threshA) {
      if (!below) {
        below = true;
        settleUs = gSamples[k].tUs;
      }
    } else {
      below = false;
      settleUs = -1.0f;
    }
  }
  return (settleUs < 0.0f) ? -1.0f : settleUs / 1000.0f;
}

// Time (ms) after which voutActual stays within `fracTol` of its final value.
static float timeWithinVoltage(float vFinal, float stepSize, float fracTol) {
  float band = fabsf(stepSize) * fracTol;
  float settleUs = -1.0f;
  bool inside = false;
  for (uint16_t k = 0; k < gSampleCount; k++) {
    if (fabsf(gSamples[k].vout - vFinal) <= band) {
      if (!inside) {
        inside = true;
        settleUs = gSamples[k].tUs;
      }
    } else {
      inside = false;
      settleUs = -1.0f;
    }
  }
  return (settleUs < 0.0f) ? -1.0f : settleUs / 1000.0f;
}

static void analyseAndReport(float vCmd, float fromV, float toV) {
  if (gSampleCount == 0) {
    Serial.println("No samples captured.");
    return;
  }

  // Final settled value = mean of the last 5% of samples.
  uint16_t tailStart = gSampleCount - (gSampleCount / 20 + 1);
  float sum = 0.0f;
  uint16_t n = 0;
  for (uint16_t k = tailStart; k < gSampleCount; k++) {
    sum += gSamples[k].vout;
    n++;
  }
  float vFinal = sum / n;
  float stepSize = toV - fromV;

  Serial.println("\n================ SETTLING CAPTURE ================");
  Serial.printf("CH%u  step %.3f V -> %.3f V   (feed-forward Vcmd = %.3f V)\n",
                gChannel + 1, fromV, toV, vCmd);
  Serial.printf("samples=%u  window=%lu ms  spacing=%.3f ms\n",
                gSampleCount, (unsigned long)gWindowMs,
                (gSamples[gSampleCount - 1].tUs / 1000.0f) / gSampleCount);
  Serial.printf("Vfinal(measured) = %.4f V\n\n", vFinal);

  // ---- Downsampled trace: ~30 rows -------------------------------------
  Serial.println("  t(ms)   Vout(V)   Iapparent(mA)");
  uint16_t stride = gSampleCount / 30;
  if (stride == 0) stride = 1;
  for (uint16_t k = 0; k < gSampleCount; k += stride) {
    float iAppMa = ((vCmd - gSamples[k].vout) / kShuntOhms) * 1000.0f;
    Serial.printf("  %6.2f   %7.4f   %9.3f\n",
                  gSamples[k].tUs / 1000.0f, gSamples[k].vout, iAppMa);
  }

  // ---- Voltage settling -------------------------------------------------
  Serial.println("\nVoltage settling (time to stay within band of Vfinal):");
  const float vTols[] = {0.05f, 0.02f, 0.01f};
  for (uint8_t i = 0; i < 3; i++) {
    float t = timeWithinVoltage(vFinal, stepSize, vTols[i]);
    Serial.printf("  +/-%4.1f%%  : %s\n", vTols[i] * 100.0f,
                  t < 0 ? "not reached" : (String(t, 2) + " ms").c_str());
  }

  // ---- Apparent-current settling (the number that actually matters) -----
  Serial.println("\nApparent over-current clears (|Iapp| stays below threshold):");
  float worst = 0.0f;
  for (uint8_t i = 0; i < kNumThresh; i++) {
    float t = timeBelowCurrent(vCmd, kCurrentThreshMa[i]);
    Serial.printf("  < %7.2f mA : %s\n", kCurrentThreshMa[i],
                  t < 0 ? "not reached" : (String(t, 2) + " ms").c_str());
    if (t > worst) worst = t;
  }

  // ---- Debounce recommendation -----------------------------------------
  Serial.println("\n--- Debounce sizing (@ 50 ms update period) ---");
  for (uint8_t i = 0; i < kNumThresh; i++) {
    float t = timeBelowCurrent(vCmd, kCurrentThreshMa[i]);
    if (t < 0) continue;
    // Number of consecutive over-limit samples to ride through: settling time
    // divided by the update period, rounded up, +1 sample of margin.
    int samples = (int)ceilf(t / kUpdatePeriodMs) + 1;
    Serial.printf("  limit %6.2f mA -> settle %.1f ms -> debounce >= %d samples (~%d ms)\n",
                  kCurrentThreshMa[i], t, samples, samples * (int)kUpdatePeriodMs);
  }
  Serial.println("==================================================\n");
}

// ----------------------------------------------------------------------------
// Capture
// ----------------------------------------------------------------------------
static void runCapture(float fromV, float toV) {
  LVLPChannel *ch = &lvlpChannels[gChannel];

  // Establish the starting point and let it fully settle.
  ch->setMode(MODE_VOLTAGE_SOURCE);
  ch->setOutputVoltage(fromV);
  delay(500);

  uint32_t spacingUs = (gWindowMs * 1000UL) / kMaxSamples;

  // t0 is captured BEFORE the DAC write, matching the real firmware where the
  // command returns and update() samples some time later.
  uint32_t t0 = micros();
  ch->setOutputVoltage(toV);
  // Feed-forward voltage the current calc jumps to the instant of the command.
  float vCmd = ch->calculateExpectedOutputVoltage(ch->dacValueAttribute);

  gSampleCount = 0;
  uint32_t nextT = 0;
  for (uint16_t k = 0; k < kMaxSamples; k++) {
    while ((micros() - t0) < nextT) {
      // busy-wait for even spacing
    }
    float vout = ch->readVoltage();
    gSamples[k].tUs = micros() - t0;
    gSamples[k].vout = vout;
    gSampleCount++;
    nextT += spacingUs;
  }

  analyseAndReport(vCmd, fromV, toV);

  // Return the channel to a quiet, safe state.
  ch->setOutputVoltage(gLowVolts);
}

// ----------------------------------------------------------------------------
static void printHelp() {
  Serial.println("\n--- LVLP settling-time measurement ---");
  Serial.printf("channel = CH%u | low = %.3f V | high = %.3f V | window = %lu ms\n",
                gChannel + 1, gLowVolts, gHighVolts, (unsigned long)gWindowMs);
  Serial.println("commands: c<n> l<v> h<v> w<ms> | g=up-step  j=down-step  ?=help");
  Serial.println("(encoder button also runs an up-step)");
}

static void handleSerial() {
  if (!Serial.available()) return;
  char cmd = Serial.read();
  switch (cmd) {
    case 'c': {
      int n = Serial.parseInt();
      if (n >= 1 && n <= 8) gChannel = n - 1;
      printHelp();
      break;
    }
    case 'l': gLowVolts = Serial.parseFloat(); printHelp(); break;
    case 'h': gHighVolts = Serial.parseFloat(); printHelp(); break;
    case 'w': {
      long w = Serial.parseInt();
      if (w > 0) gWindowMs = (uint32_t)w;
      printHelp();
      break;
    }
    case 'g': runCapture(gLowVolts, gHighVolts); break;
    case 'j': runCapture(gHighVolts, gLowVolts); break;
    case '?': printHelp(); break;
    default: break;
  }
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n--- test_settling: LVLP step-response capture ---");

  if (!initializeMCP4728()) Serial.println("MCP4728 init failed!");
  if (!initializeMCP3208()) Serial.println("MCP3208 init failed!");
  initializeWS2812B();
  initializeEncoder(-100, 100, true);

  pinMode(pinSrOe, OUTPUT);
  digitalWrite(pinSrOe, LOW);
  sr.setAllLow();

  for (uint8_t i = 0; i < 8; i++) {
    lvlpChannels[i].init();
    // Wide limits so nothing trips; this sketch never calls update()/checkLimits
    // anyway, but keep the channel out of a FAIL state just in case.
    lvlpChannels[i].setLimits(12.0f, 1.0f);
    lvlpChannels[i].setMode(MODE_HIGH_IMPEDANCE);
    lvlpChannels[i].setOutputVoltage(0.0f);
  }

  printHelp();
}

void loop() {
  handleSerial();

  // Encoder button (active low) triggers an up-step capture, debounced.
  static bool wasPressed = false;
  bool pressed = (digitalRead(pinEncoderSw) == LOW);
  if (pressed && !wasPressed) {
    runCapture(gLowVolts, gHighVolts);
  }
  wasPressed = pressed;
}
