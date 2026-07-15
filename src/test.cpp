#include <Arduino.h>

#include "hardwareIOSetup.h"

constexpr uint8_t kNumChannels = 8;
constexpr uint8_t kChannelsPerPage = 4;
constexpr float kFixedVoltage = 9.0f;
constexpr float kVoltageLimit = 11.0f;
constexpr float kCurrentLimit = 0.03f;
constexpr uint32_t kRefreshPeriodMs = 1000;

void configureChannelAsFixedVoltage(LVLPChannel &channel)
{
    channel.setLimits(kVoltageLimit, kCurrentLimit);
    channel.setMode(MODE_VOLTAGE_SOURCE);
    channel.setOutputVoltage(kFixedVoltage);
}

void initializeHardware()
{
    initializeMCP4728();
    initializeMCP3208();
    initializeSSD1309();
    initializeWS2812B();

    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW);
    sr.setAllLow();

    for (uint8_t index = 0; index < 4; index++)
    {
        lvlpChannels[index].init();
        configureChannelAsFixedVoltage(lvlpChannels[index]);
    }
    for (uint8_t index = 4; index < kNumChannels; index++)
    {
        lvlpChannels[index].init();
        lvlpChannels[index].setMode(MODE_HIGH_IMPEDANCE);
    }

}

void updateChannels()
{
    for (uint8_t index = 0; index < kNumChannels; index++)
    {
        lvlpChannels[index].update();
    }
}

void printChannelSummaryToSerial()
{
    Serial.println("\n=== LVLP fixed-voltage summary ===");
    for (uint8_t index = 0; index < kNumChannels; index++)
    {
        LVLPChannel &channel = lvlpChannels[index];
        const char *modeText = (channel.getMode() == MODE_VOLTAGE_SOURCE) ? "VS" : "--";
        const char *statusText = (channel.getStatus() == STATUS_NORMAL) ? "OK" : "FAIL";

        Serial.printf("CH%u | %s | set=%.2f V | out=%.2f V | i=%.4f A | %s\n",
                      index + 1,
                      modeText,
                      kFixedVoltage,
                      channel.readVoltage(),
                      channel.readCurrent(),
                      statusText);
    }
}

void drawChannelSummaryPage(uint8_t firstChannel)
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x8_mr);
    u8g2.drawStr(0, 8, "LVLP fixed voltage");

    for (uint8_t row = 0; row < kChannelsPerPage; row++)
    {
        const uint8_t channelIndex = firstChannel + row;
        if (channelIndex >= kNumChannels)
        {
            break;
        }

        LVLPChannel &channel = lvlpChannels[channelIndex];
        const char *statusText = (channel.getStatus() == STATUS_NORMAL) ? "OK" : "FL";
        char line[32];
        snprintf(line, sizeof(line), "CH%u %s %.2f %.2f %.3f%s",
                 channelIndex + 1,
                 statusText,
                 kFixedVoltage,
                 channel.readVoltage(),
                 channel.readCurrent(),
                 "A");
        u8g2.drawStr(0, 20 + (row * 12), line);
    }

    u8g2.sendBuffer();
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n--- LVLP fixed-voltage test ---");
    initializeHardware();

    Serial.printf("All LVLP channels set to %.2f V.\n", kFixedVoltage);
}

void loop()
{
    updateChannels();

    static uint32_t lastRefresh = 0;
    static uint8_t page = 0;

    const uint32_t now = millis();
    if (now - lastRefresh >= kRefreshPeriodMs)
    {
        lastRefresh = now;

        printChannelSummaryToSerial();
        drawChannelSummaryPage(page * kChannelsPerPage);

        page = (page + 1) % 2;
    }
}