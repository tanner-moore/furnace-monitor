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

// One PT100 reading with the raw figures the bench page shows.
struct RtdReading {
  float ohms = NAN;
  float tempC = NAN;   // NAN on a fault or out of range
  uint8_t fault = 0;   // MAX31865 fault register, 0 = none
};

// Reads PT100 channel 1-4. Takes about 70 ms. Returns false on a fault or an
// out-of-range temperature (open, shorted or not fitted).
bool readRtd(uint8_t channel, RtdReading& out);

// Which on-board chips answered at begin() (inputs is re-checked on every read).
struct ChipStatus {
  bool relays = false;
  bool inputs = false;
  bool adc[4] = {false, false, false, false};
  bool rtd = false;
};
ChipStatus chipStatus();

// The MAX31865, display and SD card share one SPI bus. Hold this lock around
// any use of it. It is recursive, so nested takes from one task are fine.
void spiLock();
void spiUnlock();

}  // namespace co16
