// Drivers for the CO16's on-board chips. Read-only toward the furnace: the only
// write to the relay expander is the all-off at boot.
#pragma once

#include <Arduino.h>

namespace co16 {

// Initializes I2C, SPI and every input chip, and forces all relays off.
void begin();

// Bitmask of the 16 dry-contact inputs, bit 0 = DI1. 1 = contact closed.
// Returns false if the XL9555 did not answer.
bool readInputs(uint16_t& bits);

// Terminal voltage (0-10 V) of analog input 1-16, or NAN if the ADC failed.
float readAnalogVolts(uint8_t input);

// Temperature in degrees C of PT100 channel 1-4, or NAN on an RTD fault
// (open, shorted or out of range). Takes about 70 ms.
float readRtdC(uint8_t channel);

}  // namespace co16
