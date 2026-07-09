#include <Arduino.h>
#include "hardwareIOSetup.h"

constexpr uint8_t kNumTestChannels = 8;
constexpr float kReferenceVespVoltage = 3.269f;
LVLPChannel *testChannels[kNumTestChannels] = {
    &lvlpChannels[0],
    &lvlpChannels[1],
    &lvlpChannels[2],
    &lvlpChannels[3],
    &lvlpChannels[4],
    &lvlpChannels[5],
    &lvlpChannels[6],
    &lvlpChannels[7],
};

struct ChannelReading
{
    float voltage;
    float current;
};

ChannelReading channelReadings[kNumTestChannels];

void setAllChannelsHighImpedance()
{
    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        testChannels[i]->setMode(MODE_HIGH_IMPEDANCE);
        testChannels[i]->setOutputVoltage(0.0f);
    }
}

void setOnlyChannelAsVoltageSource(uint8_t channelIndex, float voltage)
{
    setAllChannelsHighImpedance();
    testChannels[channelIndex]->setMode(MODE_VOLTAGE_SOURCE);
    testChannels[channelIndex]->setOutputVoltage(voltage);
}

struct LinearFit
{
    float slope;
    float intercept;
    bool valid;
};

LinearFit fitLinearModel(const float *xValues, const float *yValues, uint8_t count)
{
    LinearFit fit = {0.0f, 0.0f, false};
    if (count < 2)
    {
        return fit;
    }

    float sumX = 0.0f;
    float sumY = 0.0f;
    float sumXX = 0.0f;
    float sumXY = 0.0f;

    for (uint8_t i = 0; i < count; i++)
    {
        sumX += xValues[i];
        sumY += yValues[i];
        sumXX += xValues[i] * xValues[i];
        sumXY += xValues[i] * yValues[i];
    }

    float denominator = (count * sumXX) - (sumX * sumX);
    if (fabsf(denominator) < 1e-6f)
    {
        return fit;
    }

    fit.slope = ((count * sumXY) - (sumX * sumY)) / denominator;
    fit.intercept = (sumY - (fit.slope * sumX)) / count;
    fit.valid = true;
    return fit;
}

void applyCalibrationToChannel(uint8_t channelIndex, const ChannelCalibrationData &calibration)
{
    chCalData[channelIndex] = calibration;
    testChannels[channelIndex]->setCalibrationData(calibration);
}

void printCalibrationData(uint8_t channelIndex, const ChannelCalibrationData &calibration)
{
    Serial.printf("CH%u calibration:\n", channelIndex + 1);
    Serial.printf("  K1=%.6f K2=%.6f offset=%.6f\n", calibration.K1, calibration.K2, calibration.offset);
    Serial.printf("  mADC=%.6f bADC=%.6f mDAC=%.6f bDAC=%.6f\n", calibration.mADC, calibration.bADC, calibration.mDAC, calibration.bDAC);
}

void printCalibrationBlockForPaste()
{
    Serial.println("\n=== chCalData block for paste ===");
    Serial.println("ChannelCalibrationData chCalData[8] = {");
    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        const ChannelCalibrationData &calibration = chCalData[i];
        Serial.printf("    { %.9ff, %.9ff, %.9ff, %.9ff, %.9ff, %.9ff, %.9ff }, // CH%u\n",
                      calibration.K1,
                      calibration.K2,
                      calibration.offset,
                      calibration.mADC,
                      calibration.bADC,
                      calibration.mDAC,
                      calibration.bDAC,
                      i + 1);
    }
    Serial.println("};");
    Serial.println("=== End chCalData block ===\n");
}

bool calibrateChannelWithReference(uint8_t channelIndex, const float *targets, uint8_t targetCount)
{
    if (channelIndex == 0)
    {
        Serial.println("CH1 is the reference channel; keeping its calibration as-is.");
        return true;
    }

    // Raise the current limit on the active channel during calibration so the
    // protection does not trip on the (very low) shared-bus leakage currents.
    // Save the old limit and restore it when done.
    static constexpr float kCalCurrentLimitA  = 0.05f;  // 50 mA
    static constexpr float kCalVoltageLimitV  = 12.0f;  // above any target
    static constexpr uint8_t kAvgSamples      = 16;     // ADC averaging per point

    testChannels[channelIndex]->setLimits(kCalVoltageLimitV, kCalCurrentLimitA);

    float rawSamples[8];
    float referenceSamples[8];
    float dacVoltageSamples[8];

    for (uint8_t sample = 0; sample < targetCount; sample++)
    {
        setOnlyChannelAsVoltageSource(channelIndex, targets[sample]);

        // Settle: 500 ms gives the DAC + op-amp time to fully settle.
        delay(500);

        // --- guard: abort this channel if it tripped during settling ---
        if (testChannels[channelIndex]->getStatus() != STATUS_NORMAL)
        {
            Serial.printf("CH%u tripped at target %.1f V — aborting calibration for this channel.\n",
                          channelIndex + 1, targets[sample]);
            setAllChannelsHighImpedance();
            return false;
        }

        // Average kAvgSamples reads to reduce ADC noise.
        float sumRef = 0.0f;
        float sumRaw = 0.0f;
        for (uint8_t a = 0; a < kAvgSamples; a++)
        {
            sumRef += testChannels[0]->readVoltage();
            sumRaw += static_cast<float>(testChannels[channelIndex]->readMCP3208Value());
            delay(2);
        }
        referenceSamples[sample]  = sumRef / kAvgSamples;
        rawSamples[sample]        = sumRaw / kAvgSamples;
        dacVoltageSamples[sample] = testChannels[channelIndex]->dacVoltageAttribute;

        Serial.printf("CH%u sample %u: target=%.3f V, ref=%.4f V, raw=%.1f, dacV=%.4f V\n",
                      channelIndex + 1,
                      sample + 1,
                      targets[sample],
                      referenceSamples[sample],
                      rawSamples[sample],
                      dacVoltageSamples[sample]);
    }

    LinearFit adcFit    = fitLinearModel(rawSamples,        referenceSamples, targetCount);
    LinearFit outputFit = fitLinearModel(dacVoltageSamples, referenceSamples, targetCount);

    if (!adcFit.valid || !outputFit.valid)
    {
        Serial.printf("CH%u calibration failed: insufficient linear fit quality.\n", channelIndex + 1);
        return false;
    }

    ChannelCalibrationData calibration = chCalData[channelIndex];
    const ChannelCalibrationData referenceCalibration = chCalData[0];

    calibration.K1     = referenceCalibration.K1;
    calibration.K2     = outputFit.slope;
    calibration.offset = outputFit.intercept - (calibration.K1 * kReferenceVespVoltage);
    calibration.mADC   = adcFit.slope;
    calibration.bADC   = adcFit.intercept;
    calibration.mDAC   = referenceCalibration.mDAC;
    calibration.bDAC   = referenceCalibration.bDAC;

    applyCalibrationToChannel(channelIndex, calibration);
    printCalibrationData(channelIndex, calibration);
    return true;
}

void runCalibrationRoutine()
{
    Serial.println("\n=== Shared-Reference Calibration Routine ===");
    Serial.println("Tie all LP channel outputs together and keep only one channel active at a time.");
    Serial.println("CH1 will be used as the measurement reference.");

    // Upper target is 10.0 V (not 11.0 V) to stay below the default 11 V
    // overvoltage limit, leaving headroom for any initial calibration overshoot.
    const float calibrationTargets[] = {1.0f, 3.0f, 5.0f, 8.0f, 10.0f};
    const uint8_t calibrationTargetCount = sizeof(calibrationTargets) / sizeof(calibrationTargets[0]);

    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        testChannels[i]->resetStatus();
    }

    setAllChannelsHighImpedance();

    for (uint8_t channelIndex = 1; channelIndex < kNumTestChannels; channelIndex++)
    {
        Serial.printf("\n--- Calibrating CH%u against CH1 ---\n", channelIndex + 1);
        calibrateChannelWithReference(channelIndex, calibrationTargets, calibrationTargetCount);
    }

    printCalibrationBlockForPaste();
    Serial.println("\n=== Calibration routine complete ===");
}

void readAllChannels()
{
    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        channelReadings[i].voltage = testChannels[i]->readVoltage();
        channelReadings[i].current = testChannels[i]->readCurrent();
    }
}

void printAllChannelsToSerial()
{
    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        Serial.printf("CH%u: %.3f V | %.5f A\n", i + 1, channelReadings[i].voltage, channelReadings[i].current);
    }
}

void drawChannelPage(uint8_t firstChannel)
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x8_mr);

    for (uint8_t row = 0; row < 8; row++)
    {
        uint8_t channelIndex = firstChannel + row;
        if (channelIndex >= kNumTestChannels)
        {
            break;
        }

        char line[24];
        snprintf(line, sizeof(line), "%u:%5.2fV %5.3fA", channelIndex + 1, channelReadings[channelIndex].voltage, channelReadings[channelIndex].current);
        u8g2.drawStr(0, (row + 1) * 8, line);
    }

 

    u8g2.sendBuffer();
}

void displayData(String data)
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 20, "LVLP Channel Test");
    u8g2.drawStr(0, 40, data.c_str());
    u8g2.sendBuffer();
}

void setup()
{
    Serial.begin(115200);
    while (!Serial)
    {
        delay(10);
    }
    Serial.println("\n--- LVLPChannel Test Initialize ---");

    if (!initializeMCP4728())
    {
        Serial.println("MCP4728 Initialization Failed!");
    }

    if (!initializeMCP3208())
    {
        Serial.println("MCP3208 Initialization Failed!");
    }

    Serial.println("Starting SSD1309 Display Test...");
    initializeSSD1309();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);

    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllLow();

    initializeWS2812B();
    initializeEncoder(-100, 100, true);

    bool runCalibrationOnBoot = !digitalRead(pinEncoderSw);

    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        lvlpChannels[i].init();
    }

    if (runCalibrationOnBoot)
    {
        runCalibrationRoutine();
    }
    else
    {
        Serial.println("Initialization complete. Starting test cycle on all channels.");
         sr.set(8,LOW); 
         delay(1000);
        for (uint8_t i = 0; i < kNumTestChannels; i++)
        {
            testChannels[i]->setLimits(10, 0.07);
            testChannels[i]->setMode(MODE_HIGH_IMPEDANCE);
            testChannels[i]->setOutputVoltage(0);

        }
        testChannels[0]->setMode(MODE_VOLTAGE_SOURCE);
        testChannels[0]->setOutputVoltage(1.5);
         testChannels[1]->setMode(MODE_VOLTAGE_SOURCE);
        testChannels[1]->setOutputVoltage(1.5);
        delay(1000);
        sr.set(8,HIGH); 
    }

    delay(1000);
}

void loop()
{
    bool botEncoder = digitalRead(pinEncoderSw);
    static int cont=0;
    if (!botEncoder)
    {
        // for (uint8_t i = 0; i < kNumTestChannels; i++)
        // {
        //     if(testChannels[i]->getStatus()!=STATUS_NORMAL)
        //     {
        //         testChannels[i]->resetStatus();
        //     }
        // }

        if(cont%2==0)
        {
        testChannels[0]->setMode(MODE_VOLTAGE_SOURCE);
        testChannels[0]->setOutputVoltage(4);
         testChannels[1]->setMode(MODE_VOLTAGE_SOURCE);
        testChannels[1]->setOutputVoltage(4);
        delay(2);
        }
        else
        {
                       testChannels[0]->setMode(MODE_VOLTAGE_SOURCE);
        testChannels[0]->setOutputVoltage(1.5);
         testChannels[1]->setMode(MODE_VOLTAGE_SOURCE);
        testChannels[1]->setOutputVoltage(1.5);
        delay(2);
        }
   
        cont++;
        
    }
    else if(botEncoder)

    for (uint8_t i = 0; i < kNumTestChannels; i++)
    {
        testChannels[i]->update();
    }

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 1000)
    {
        lastPrint = millis();

        readAllChannels();

        for (uint8_t i = 0; i < kNumTestChannels; i++)
        {
            //testChannels[i]->printDebugInfo();
        }

        //printAllChannelsToSerial();
        drawChannelPage(0);
    }
}
