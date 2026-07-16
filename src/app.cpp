/**
 * @file app.cpp
 * @brief Unified PCB Tester application firmware.
 *
 * Exposes all channels (8x LVLP, 2x HPCH, 1x HV) to the PC GUI through the
 * NDJSON serial protocol (lib/Protocol) over native USB-CDC:
 *   - pushed telemetry at a configurable rate (default 10 Hz)
 *   - manual channel control (ch.set / ch.connect / ch.limits / ch.reset)
 *   - E-stop from the GUI ("estop" command) or the encoder push button
 *
 * The OLED acts as a passive status display while the GUI is in control.
 *
 * Build with [env:app]. Use the ESP32-S3 *native USB* connector (the build
 * enables ARDUINO_USB_CDC_ON_BOOT), not the UART-bridge connector.
 */

#include <Arduino.h>
#include "hardwareIOSetup.h"
#include "Protocol.h"
#include "CalibrationStore.h"
#include "DUTTestbench.h"

static TesterProtocol proto;
static CalibrationStore calStore;
static DUTTestRunner testRunner;
static LVLPChannel* lvlpPtrs[8];
static HPCH* hpPtrs[2];

static constexpr uint32_t UPDATE_PERIOD_MS = 50; // channel update pass (20 Hz)
static constexpr uint32_t OLED_PERIOD_MS = 250;

static uint32_t lastUpdateMs = 0;
static uint32_t lastOledMs = 0;
static bool btnWasPressed = false;

// ============================================================================
// OLED status page
// ============================================================================

static uint8_t countLvlpFaults() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < 8; i++)
    if (lvlpChannels[i].getStatus() != STATUS_NORMAL) n++;
  return n;
}

static uint8_t countHpFaults() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < 2; i++)
    if (hpChannels[i].getStatus() != HPCH_STATUS_NORMAL) n++;
  return n;
}

static void drawStatusScreen() {
  char line[32];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);

  snprintf(line, sizeof(line), "PCB Tester v%s", TesterProtocol::FW_VERSION);
  u8g2.drawStr(0, 10, line);
  u8g2.drawHLine(0, 13, 128);

  snprintf(line, sizeof(line), "USB:%s  telem:%uHz", Serial ? "ok" : "--",
           proto.getTelemetryRateHz());
  u8g2.drawStr(0, 26, line);

  snprintf(line, sizeof(line), "LVLP flt:%u  HP flt:%u", countLvlpFaults(),
           countHpFaults());
  u8g2.drawStr(0, 38, line);

  snprintf(line, sizeof(line), "HV:%s %.1fV",
           hvChannels[0].isConnected() ? "on" : "off",
           hvChannels[0].getLastVoltage());
  u8g2.drawStr(0, 50, line);

  if (proto.isEstopLatched()) {
    u8g2.setFont(u8g2_font_7x14B_tf);
    u8g2.drawStr(24, 64, "* E-STOP *");
  } else {
    u8g2.drawStr(0, 62, "Btn = E-stop");
  }

  u8g2.sendBuffer();
}

// ============================================================================
// Setup / loop
// ============================================================================

void setup() {
  // The default CDC buffers are 256 B. Long protocol lines (tb.add tests are
  // ~900 B) arrive while loop() is stalled in the OLED redraw and overflow
  // the RX buffer -> corrupted line -> E_PARSE without id -> client timeout.
  // Size both directions generously before begin().
  Serial.setRxBufferSize(4096);
#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(4096);
#endif
  Serial.begin(115200);
#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE && \
    defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Never block the loop on an unplugged/closed host port.
  Serial.setTxTimeoutMs(0);
#endif

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
  for (uint8_t i = 0; i < 2; i++) {
    hpChannels[i].init();
  }
  hvChannels[0].init();
  hvChannels[0].setLimits(70.0f);

  // Calibration from NVS overrides the hardwareIOSetup.h factory defaults;
  // channels keep the defaults until the first cal.save.
  uint8_t calLoaded = 0;
  {
    ChannelCalibrationData ld;
    for (uint8_t i = 0; i < 8; i++)
      if (calStore.loadLVLP(i, ld)) { lvlpChannels[i].setCalibrationData(ld); calLoaded++; }
    HPCHCalibrationData hd;
    uint16_t zero;
    for (uint8_t i = 0; i < 2; i++)
      if (calStore.loadHP(i, hd, zero)) {
        hpChannels[i].setCalibrationData(hd);
        hpChannels[i].setACS725ZeroADC(zero);
        calLoaded++;
      }
    HVChannelCalibrationData vd;
    if (calStore.loadHV(0, vd)) { hvChannels[0].setCalibrationData(vd); calLoaded++; }
  }

  for (uint8_t i = 0; i < 8; i++) lvlpPtrs[i] = &lvlpChannels[i];
  for (uint8_t i = 0; i < 2; i++) hpPtrs[i] = &hpChannels[i];
  testRunner.begin(lvlpPtrs, 8);
  testRunner.bindHPChannels(hpPtrs, 2); // sense_hp_mask in static_voltage tests
  testRunner.bindShiftRegister(&sr);
  testRunner.setDisplay(&u8g2);

  proto.begin(lvlpChannels, 8, hpChannels, 2, hvChannels, 1, Serial);
  proto.setCalibrationStore(&calStore);
  proto.setTestRunner(&testRunner);
  Serial.printf("{\"type\":\"log\",\"lvl\":\"info\",\"msg\":\"PCB Tester app ready, cal from NVS: %u/11 channels\"}\n",
                calLoaded);

  drawStatusScreen();
}

void loop() {
  proto.service();

  // Encoder push button = physical E-stop (active low, edge-triggered)
  bool btnPressed = (digitalRead(pinEncoderSw) == LOW);
  if (btnPressed && !btnWasPressed) {
    proto.triggerEstop("button");
  }
  btnWasPressed = btnPressed;

  uint32_t now = millis();

  if (now - lastUpdateMs >= UPDATE_PERIOD_MS) {
    lastUpdateMs = now;
    for (uint8_t i = 0; i < 8; i++) lvlpChannels[i].update();
    for (uint8_t i = 0; i < 2; i++) hpChannels[i].update();
    hvChannels[0].update();
    proto.pollFaultEvents();
  }

  if (Serial) {
    proto.emitTelemetryIfDue();
  }

  if (now - lastOledMs >= OLED_PERIOD_MS) {
    lastOledMs = now;
    drawStatusScreen();
  }
}
