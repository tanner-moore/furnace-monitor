#pragma once
#include <Arduino.h>

// Sensirion SDP8xx differential pressure sensors on the CO16's I2C bus. The SDP810 answers at
// 0x25 and the SDP811 at 0x26, so one of each can share the bus. Call only from the task that
// owns the I2C bus.
namespace sdp8xx {

constexpr uint8_t kAddrSdp810 = 0x25;
constexpr uint8_t kAddrSdp811 = 0x26;

// The sensor address for a pressure sensor type setting ("sdp810", "sdp811"), 0 for a 0-10 V
// transmitter.
uint8_t addressFor(const String& type);

// The average differential pressure since the last call, in Pa. Starts the sensor's continuous
// mode on first use and again after it stops answering. NAN when it doesn't answer.
float readPa(uint8_t addr);

}  // namespace sdp8xx
