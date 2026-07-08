#include "LVLPChannel.h"

// DAC supply voltage (defined in hardwareIOSetup.h as const float
// dacSupplyVoltage=4.957;)
const float DAC_SUPPLY_VOLTAGE = 4.967;
const float vespVoltage = 3.269;

LVLPChannel::LVLPChannel(uint8_t chIndex, MCP3208 *adcPtr,
                         MCP3208::Channel adcCh, Adafruit_MCP4728 *dacPtr,
                         MCP4728_channel_t dacCh,
                         ShiftRegister74HC595<2> *srPtr, uint8_t srP,
                         int8_t pwmP, const ChannelCalibrationData &calDataRef,
                         CRGB *ledPtr)
    : channelIndex(chIndex), adc(adcPtr), adcChannel(adcCh), dac(dacPtr),
      dacChannel(dacCh), sr(srPtr), srPin(srP), pwmPin(pwmP),
      calData(calDataRef), channelMode(MODE_HIGH_IMPEDANCE), userTargetVoltage(0),
      targetCurrent(0), loopTargetVoltage(0), led(ledPtr),
      channelStatus(STATUS_NORMAL), maxVoltageLimit(11.0),
      maxCurrentLimit(0.01) {
  // Map the struct values to the old legacy variables to keep existing
  // functions working
  calData.K1 = calDataRef.K1;
  calData.K2 = calDataRef.K2;
  calData.offset = calDataRef.offset;
  calData.mADC = calDataRef.mADC;
  calData.bADC = calDataRef.bADC;

  pidKp = 5.0f;
  pidKi = 50.0f;
  pidKd = 0.0f;
  pidIntegral = 0.0f;
  pidPrevError = 0.0f;
  pidLastTime = millis();
}

void LVLPChannel::setLimits(float maxVoltage, float maxCurrent) {
  maxVoltageLimit = maxVoltage;
  maxCurrentLimit = maxCurrent;
}

void LVLPChannel::resetStatus() {
  setMode(MODE_HIGH_IMPEDANCE);
  channelStatus = STATUS_NORMAL;
  if (led) {
    *led = CRGB::Green;
    FastLED.show();
  }
}

void LVLPChannel::checkLimits() {

  if (channelStatus != STATUS_NORMAL)
    return;

  if (channelVoltageOut > maxVoltageLimit) {
    // setMode is safe here: channelStatus is still STATUS_NORMAL at this point,
    // so the guard in setMode() allows MODE_HIGH_IMPEDANCE.
    // This opens the relay AND drives the PWM pin LOW (for CH1-CH4).
    setMode(MODE_HIGH_IMPEDANCE);
    channelStatus = STATUS_FAIL_OVERVOLTAGE;
    if (led) {
      *led = CRGB::Blue;
      FastLED.show();
    }

  } else if (abs(channelCurrentOut) > maxCurrentLimit) {
    // Do not trip in high-impedance mode (ADC noise can cause false readings).
    if (channelMode == MODE_HIGH_IMPEDANCE) return;
    setMode(MODE_HIGH_IMPEDANCE);
    channelStatus = STATUS_FAIL_OVERCURRENT;
    if (led) {
      *led = CRGB::Red;
      FastLED.show();
    }
  }
}


void LVLPChannel::init() {
  disconnect();

  // Set DAC to 0
  dac->setChannelValue(dacChannel, calculateDacValue(0.0));

  if (pwmPin >= 0) {
    pinMode(pwmPin, OUTPUT);
    digitalWrite(pwmPin, LOW);
  }

  if (led) {
    *led = CRGB::Green;
    FastLED.show();
  }
}

uint16_t LVLPChannel::calculateDacValue(float volts) {
  // Formula derived from op-amp test: Vout = 3.385*opampGain -
  // Vdac*(opampGain-1) Solving for Vdac: Vdac = (3.385*opampGain - Vout) /
  // (opampGain - 1)
  float dacVoltage =
      (volts - calData.K1 * vespVoltage - calData.offset) / calData.K2;
  // Serial.println(dacVoltage);

  // Clamp the reference to 0.0 - DAC_SUPPLY_VOLTAGE
  if (dacVoltage < 0.0)
    dacVoltage = 0.0;
  if (dacVoltage > DAC_SUPPLY_VOLTAGE)
    dacVoltage = DAC_SUPPLY_VOLTAGE;

  dacValueAttribute = dacVoltage * calData.mDAC + calData.bDAC;
  dacVoltageAttribute = dacVoltage;

  return dacValueAttribute;
}

float LVLPChannel::calculateExpectedOutputVoltage(uint16_t dacValue) {
  // Reverse the formula from calculateDacValue
  // dacValue = dacVoltage * calData.mDAC + calData.bDAC
  float dacVoltage = (dacValue - calData.bDAC) / calData.mDAC;
  
  // dacVoltage = (volts - calData.K1 * vespVoltage - calData.offset) / calData.K2
  // volts = dacVoltage * calData.K2 + calData.K1 * vespVoltage + calData.offset
  float expectedVolts = dacVoltage * calData.K2 + calData.K1 * vespVoltage + calData.offset;
  
  return expectedVolts;
}

void LVLPChannel::setPIDTunings(float kp, float ki, float kd) {
  pidKp = kp;
  pidKi = ki;
  pidKd = kd;
}

void LVLPChannel::resetPID() {
  if (pidKi > 0.0f) {
      pidIntegral = loopTargetVoltage / pidKi;
  } else {
      pidIntegral = 0.0f;
  }
  pidPrevError = 0.0f;
  pidLastTime = millis();
}

bool LVLPChannel::setMode(LVLPMode mode) {
  if (channelStatus != STATUS_NORMAL && mode != MODE_HIGH_IMPEDANCE) {
    return false; // Prevent enabling channel if in a FAIL state
  }

  if (mode == MODE_PWM_GENERATOR && pwmPin < 0) {
    // Channel doesn't support PWM
    return false;
  }

  if (mode != channelMode) {
    if (mode == MODE_CURRENT_SOURCE || mode == MODE_RESISTIVE_LOAD) {
      resetPID();
    }
  }

  channelMode = mode;

  if (mode == MODE_HIGH_IMPEDANCE) {
    disconnect();
    // Set PWM pin LOW when disconnected
    if (pwmPin >= 0) {
      pinMode(pwmPin, OUTPUT);
      digitalWrite(pwmPin, LOW);
    }
  } else {
    // For mode PWM, we do not interfere with standard operations,
    // the external controller (ESP32) takes over the GPAOM1.
    connect();

    // For channels 1 to 4 (which have pwmPin >= 0),
    // the PWM pin must be set HIGH to operate the op-amp properly as a source.
    if (pwmPin >= 0 && mode != MODE_PWM_GENERATOR) {
      pinMode(pwmPin, OUTPUT);
      digitalWrite(pwmPin, HIGH);
    }
  }

  return true;
}

void LVLPChannel::disconnect() { sr->set(srPin, LOW); }

void LVLPChannel::connect() {
  if (channelStatus == STATUS_NORMAL) {
    sr->set(srPin, HIGH);
  }
}

void LVLPChannel::setOutputVoltage(float voltage) {
  userTargetVoltage = voltage;
  loopTargetVoltage = voltage;
  dac->setChannelValue(dacChannel, calculateDacValue(loopTargetVoltage));
}

void LVLPChannel::setDACOutput(uint16_t value) {
  dac->setChannelValue(dacChannel, value);
  // dac->setChannelValue(dacChannel, 0);
}

void LVLPChannel::setOutputCurrent(float current) {
  targetCurrent = current;
  // Current regulation is handled interactively in the update loop
  // to step loopTargetVoltage up/down.
}

void LVLPChannel::setCalibrationData(const ChannelCalibrationData &newCalData) {
  calData = newCalData;
}

bool LVLPChannel::setPwm(uint8_t dutycycle, uint32_t frequency) {
  if (pwmPin < 0 || channelMode != MODE_PWM_GENERATOR)
    return false;

  analogWriteResolution( 8);
  analogWriteFrequency(frequency);
  analogWrite(pwmPin, dutycycle);
  return true;
}

float LVLPChannel::readVoltage() {
  uint16_t raw = adc->read(adcChannel);
  float val = calData.mADC * raw + calData.bADC;
  return val;
}

uint16_t LVLPChannel::readMCP3208Value() {
  uint16_t raw = adc->read(adcChannel);
  return raw;
}

float LVLPChannel::readCurrent() {
  delay(1);
  float voutActual = readVoltage();
  // V_before_shunt is roughly what we command the DAC to generate
  float vBeforeShunt =
      calculateExpectedOutputVoltage(calculateDacValue(loopTargetVoltage));

  // Calculate current according to ohm's law

  if (channelMode != MODE_HIGH_IMPEDANCE) {
    float current = (vBeforeShunt - voutActual) / SHUNT_RESISTANCE;
    return current;
  }
  else
  {
      return 0.0f;
  }


}

void LVLPChannel::update() {

  
  channelVoltageOut = readVoltage();
  channelCurrentOut = readCurrent();

  checkLimits();
     

  if (channelStatus != STATUS_NORMAL)
    return; // Halt loop if channel is in fail mode

  if (channelMode == MODE_CURRENT_SOURCE ||
      channelMode == MODE_RESISTIVE_LOAD) {
    float actualCurrent = readCurrent();
    
    uint32_t now = millis();
    float dt = (now - pidLastTime) / 1000.0f; // time in seconds
    if (dt <= 0.0f) dt = 0.001f;
    
    float error = targetCurrent - actualCurrent;
    
    pidIntegral += error * dt;
    
    // Anti-windup: limit integral term
    float maxIntegral = 15.0f;
    if (pidKi > 0.0f) {
        maxIntegral = 15.0f / pidKi;
    }
    if (pidIntegral > maxIntegral) pidIntegral = maxIntegral;
    if (pidIntegral < 0.0f) pidIntegral = 0.0f;
    
    float derivative = (error - pidPrevError) / dt;
    
    loopTargetVoltage = (pidKp * error) + (pidKi * pidIntegral) + (pidKd * derivative);

    // Saturation limits (assume 15V rail as theoretical maximum)
    if (loopTargetVoltage > 15.0f)
      loopTargetVoltage = 15.0f;
    if (loopTargetVoltage < 0.0f)
      loopTargetVoltage = 0.0f;

    dac->setChannelValue(dacChannel, calculateDacValue(loopTargetVoltage));
    
    pidPrevError = error;
    pidLastTime = now;
  }
}

void LVLPChannel::printDebugInfo() const {
  Serial.println("=== LVLPChannel Debug Info ===");
  Serial.print("Channel Index: "); Serial.println(channelIndex);
  Serial.print("Channel Mode: "); Serial.println(channelMode);
  Serial.print("Channel Status: "); Serial.println(channelStatus);
  Serial.print("Target Voltage: "); Serial.println(userTargetVoltage, 4);
  Serial.print("Target Current: "); Serial.println(targetCurrent, 4);


  Serial.print("Current(A)= "); Serial.println(channelCurrentOut, 4);
  Serial.print("VoltageOut(V)= "); Serial.println(channelVoltageOut, 4);

  Serial.print("Current DAC Voltage: "); Serial.println(dacValueAttribute, 4);

  Serial.print("DAC Value Attribute: "); Serial.println(dacValueAttribute);
  Serial.print("Max Voltage Limit: "); Serial.println(maxVoltageLimit, 4);
  Serial.print("Max Current Limit: "); Serial.println(maxCurrentLimit, 4);
  Serial.print("Cal K1: "); Serial.println(calData.K1, 6);
  Serial.print("Cal K2: "); Serial.println(calData.K2, 6);
  Serial.print("Cal Offset: "); Serial.println(calData.offset, 6);
  Serial.print("Cal mADC: "); Serial.println(calData.mADC, 6);
  Serial.print("Cal bADC: "); Serial.println(calData.bADC, 6);
  Serial.print("Cal mDAC: "); Serial.println(calData.mDAC, 6);
  Serial.print("Cal bDAC: "); Serial.println(calData.bDAC, 6);
  // Serial.print("Cal K1: "); Serial.println(calData.K1, 4);
  // Serial.print("Cal K2: "); Serial.println(calData.K2, 4);
  // Serial.print("Cal Offset: "); Serial.println(calData.offset, 4);
  // Serial.print("Cal mADC: "); Serial.println(calData.mADC, 4);
  // Serial.print("Cal bADC: "); Serial.println(calData.bADC, 4);
  // Serial.print("Cal mDAC: "); Serial.println(calData.mDAC, 4);
  // Serial.print("Cal bDAC: "); Serial.println(calData.bDAC, 4);
  Serial.println("==============================");
}
