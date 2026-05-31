#include <Arduino.h>
#include "hardwareIOSetup.h"
#include <ESP32RotaryEncoder.h>

const uint8_t DI_ENCODER_A   = pinEncoderD1;  // 17
const uint8_t DI_ENCODER_B   = pinEncoderD2;  // 18
const int8_t  DI_ENCODER_SW  = pinEncoderSw;  // 21



void knobCallback( long value )
{
    Serial.printf( "Value: %ld\n", value );
}

void setup()
{
    Serial.begin( 115200 );
    delay(1000); // Give serial monitor time to connect
    Serial.println("Starting Encoder Test...");

    initializeEncoder(-100,100,true);    
    rotaryEncoder.onTurned( &knobCallback );

}

void loop()
{
   
}
