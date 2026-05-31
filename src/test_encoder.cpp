#include <Arduino.h>
#include "hardwareIOSetup.h"
#include <ESP32RotaryEncoder.h>

const uint8_t DI_ENCODER_A   = pinEncoderD1;  // 17
const uint8_t DI_ENCODER_B   = pinEncoderD2;  // 18
const int8_t  DI_ENCODER_SW  = pinEncoderSw;  // 21

// We pass -1 for the switch pin so the library ignores it, and we can manage the button manually avoiding library ISR bugs.
RotaryEncoder rotaryEncoder( DI_ENCODER_A, DI_ENCODER_B, -1, -1 );

void knobCallback( long value )
{
    Serial.printf( "Value: %ld\n", value );
}

void setup()
{
    Serial.begin( 115200 );
    delay(1000); // Give serial monitor time to connect
    Serial.println("Starting Encoder Test...");

    // FLOATING tells the library to use INPUT_PULLUP internally for A/B pins
    rotaryEncoder.setEncoderType( EncoderType::FLOATING );
    rotaryEncoder.setBoundaries( -100, 100, false );
    
    rotaryEncoder.onTurned( &knobCallback );
    
    // We pass `false` here so the library timer task isn't used
    rotaryEncoder.begin(true);

    // Setup the button pin manually with pull-up
    pinMode(DI_ENCODER_SW, INPUT_PULLUP);
}

void loop()
{
   
}
