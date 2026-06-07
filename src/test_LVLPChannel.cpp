#include <Arduino.h>
#include "hardwareIOSetup.h"

// Define a test channel, e.g., Channel 1, which supports PWM
LVLPChannel *testChannel = &lvlpChannels[0];

unsigned long lastUpdate = 0;
int state = 0;

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

    // Initialize hardware peripherals
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
    u8g2.clearBuffer();                 // clear the internal memory
    u8g2.setFont(u8g2_font_ncenB08_tr); // choose a suitable font

    // SSR initialization
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe, LOW); // enable output
    sr.setAllHigh();

    // Initialize all channels
    for (int i = 0; i < 8; i++)
    {
        lvlpChannels[i].init();
    }

    Serial.println("Initialization complete. Starting test cycle on CH1.");
}

void loop()
{
    // Continuously call update for the regulation loop
    testChannel->update();
    testChannel->setMode(MODE_VOLTAGE_SOURCE);
    testChannel->setOutputVoltage(5); //5V (4.974)  1V(0.962)   10V(9.98)
   //testChannel->setDACOutput(700);

    float dacVoltage = testChannel->dacVoltageAttribute;
    float dacValue = testChannel->dacValueAttribute;
    Serial.println(dacVoltage);
    String dacVoltageStr = String(dacVoltage, 5);
    String dacValueStr = String(dacValue, 5);

    uint32_t sum = 0;
    for (int i = 0; i < 1000; i++)
    {
        sum += testChannel->readMCP3208Value();
        delay(1);
    }

    // Print measurements every 1 second
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 1000)
    {
        lastPrint = millis();

        float vOut = testChannel->readVoltage();
        float current = testChannel->readCurrent();

        Serial.printf("Mode: %d | Voltage: %.3f V || Voltage Expected: %.3f V | Current: %.3f A\n",
                      testChannel->getMode(), vOut, testChannel->calculateExpectedOutputVoltage(testChannel->dacValueAttribute), current);

        u8g2.clearBuffer();
        // u8g2.drawStr(0, 20, dacVoltageStr.c_str());
        // u8g2.drawStr(0, 40, dacValueStr.c_str());
        u8g2.setCursor(0,20);
        u8g2.print(vOut);
        u8g2.setCursor(0,40);
        u8g2.print(current,5);
        u8g2.print(" A");
        
        u8g2.sendBuffer();
    }
}

// Serial.println(sum/1000);

// if (millis() - lastUpdate > 10000) {
//     lastUpdate = millis();
//     state = (state + 1) % 4;

//     Serial.println("\n==================================");
//     switch (state) {
//         case 0:
//             Serial.println("State: VOLTAGE SOURCE (Target: 3.3V)");
//             testChannel->setMode(MODE_VOLTAGE_SOURCE);
//             testChannel->setOutputVoltage(3.3);
//             break;
//         case 1:
//             Serial.println("State: CURRENT SOURCE (Target: 0.1A)");
//             testChannel->setMode(MODE_CURRENT_SOURCE);
//             testChannel->setOutputCurrent(0.1);
//             break;
//         case 2:
//             Serial.println("State: PWM GENERATOR (50% Duty @ 1kHz)");
//             if (testChannel->setMode(MODE_PWM_GENERATOR)) {
//                 testChannel->setPwm(128, 1000);
//             } else {
//                 Serial.println("PWM mode not supported on this channel!");
//             }
//             break;
//         case 3:
//             Serial.println("State: HIGH IMPEDANCE (Disconnected)");
//             testChannel->setMode(MODE_HIGH_IMPEDANCE);
//             break;
//     }
//     Serial.println("==================================");
// }

// if (millis() - lastUpdate > 10000) {
//     lastUpdate = millis();
//     state = (state + 1) % 4;

//     Serial.println("\n==================================");
//     switch (state) {
//         case 0:
//             Serial.println("State: VOLTAGE SOURCE (Target: 3.3V)");
//             testChannel->setMode(MODE_VOLTAGE_SOURCE);
//             testChannel->setOutputVoltage(3.3);
//             break;
//         case 1:
//            Serial.println("State: VOLTAGE SOURCE (Target: 3.3V)");
//             testChannel->setMode(MODE_VOLTAGE_SOURCE);
//             testChannel->setOutputVoltage(10);
//             break;
//         case 2:
//             Serial.println("State: PWM GENERATOR (50% Duty @ 1kHz)");
//             if (testChannel->setMode(MODE_PWM_GENERATOR)) {
//                 testChannel->setPwm(128, 1000);
//             } else {
//                 Serial.println("PWM mode not supported on this channel!");
//             }
//             break;
//         case 3:
//             Serial.println("State: HIGH IMPEDANCE (Disconnected)");
//             testChannel->setMode(MODE_HIGH_IMPEDANCE);
//             break;
//     }
//     Serial.println("==================================");
// }