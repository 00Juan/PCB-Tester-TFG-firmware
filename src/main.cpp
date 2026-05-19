// /*********
//   Rui Santos
//   Complete project details at https://randomnerdtutorials.com  
// *********/
#include <Arduino.h>
#include "hardwareIOSetup.h"
#include <Adafruit_MCP4728.h>
 
Adafruit_MCP4728 mcp;
void setup() {


  Serial.begin(115200);
  delay(1000); // Wait for serial monitor to open
  initializeI2C(10000);
  scanI2CDevices();


  // Try to initialize!
  if (!mcp.begin()) {
    Serial.println("Failed to find MCP4728 chip");
    while (1) {
      delay(10);
    }
  }
  
}
 
void loop() {

}



