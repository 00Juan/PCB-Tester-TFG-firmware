#include <Arduino.h>
#include "hardwareIOSetup.h"

// SSD1309 display usually works best with the NONAME0 configuration from U8g2 over HW I2C.
// We use the HW I2C constructor, which will map to the ESP32's default Wire instance.

void setup(void) {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("Starting SSD1309 Display Test...");
initializeSSD1309();
}

void loop(void) {
  // First frame: Hello World
  u8g2.clearBuffer();                   // clear the internal memory
  u8g2.setFont(u8g2_font_ncenB08_tr);   // choose a suitable font
  u8g2.drawStr(0, 20, "Hello, World!"); // write something to the internal memory
  u8g2.drawStr(0, 40, "Controllar:");
  u8g2.drawStr(0, 60, "SSD1309 128x64");
  u8g2.sendBuffer();                    // transfer internal memory to the display
  delay(2000);

  // Second frame: Graphics
  u8g2.clearBuffer();
  u8g2.drawRFrame(10, 10, 108, 44, 4);  // draw a rounded frame
  u8g2.drawCircle(64, 32, 10);          // draw a circle
  u8g2.sendBuffer();
  delay(2000);
}
