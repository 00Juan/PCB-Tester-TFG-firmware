#include <Arduino.h>

#include "hardwareIOSetup.h"

#include <ShiftRegister74HC595.h>

// create a global shift register object
// parameters: <number of shift registers> (data pin, clock pin, latch pin)
ShiftRegister74HC595<2> sr(35, 38, 37);
 
void setup() { 
    Serial.begin(115200);
    pinMode(pinSrOe, OUTPUT);
    digitalWrite(pinSrOe,LOW);
    delay(1000);
    sr.setAllHigh(); // set all pins HIGH
      for (int i = 0; i < 4; i++) {
    
    sr.set(i, LOW); // set single pin HIGH
    delay(250); 
  }

    uint8_t pinValues[2] = { B00000011,B00000101 }; //CH1=CH2=CH9=CH11=1  el resto a 0
  sr.setAll(pinValues); 
  delay(1000);
  


}

void loop() {
      


  // setting all pins at the same time to either HIGH or LOW

  
  //sr.setAllLow(); // set all pins LOW
//   delay(500); 
  

//   // setting single pins

  
//   // set all pins at once
//   uint8_t pinValues[] = { B10101010 }; 
//   sr.setAll(pinValues); 
//   delay(1000);

  
//   // read pin (zero based, i.e. 6th pin)
//   uint8_t stateOfPin5 = sr.get(5);
//   sr.set(6, stateOfPin5);


//   // set pins without immediate update
//   sr.setNoUpdate(0, HIGH);
//   sr.setNoUpdate(1, LOW);
//   // at this point of time, pin 0 and 1 did not change yet
//   sr.updateRegisters(); // update the pins to the set values
}


