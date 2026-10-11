#include "sdp8xx.h"

#include <Wire.h>

namespace sdp8xx {

static constexpr uint16_t kStartDpAverage = 0x3615;  // continuous, differential pressure, average till read
static constexpr uint16_t kStop = 0x3FF9;
static constexpr uint32_t kRetryMs = 10000;

struct Sensor {
  bool running = false;
  uint32_t retryAtMs = 0;
};
static Sensor sensors[2];

uint8_t addressFor(const String& type) {
  if (type == "sdp810") return kAddrSdp810;
  if (type == "sdp811") return kAddrSdp811;
  return 0;
}

// CRC-8, polynomial 0x31, initial value 0xFF.
static uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

static bool command(uint8_t addr, uint16_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)cmd);
  return Wire.endTransmission() == 0;
}

float readPa(uint8_t addr) {
  if (addr != kAddrSdp810 && addr != kAddrSdp811) return NAN;
  Sensor& s = sensors[addr - kAddrSdp810];
  const uint32_t now = millis();
  if (!s.running) {
    if ((int32_t)(now - s.retryAtMs) < 0) return NAN;
    s.retryAtMs = now + kRetryMs;
    command(addr, kStop);  // it may still be running from before a restart
    delay(1);
    if (!command(addr, kStartDpAverage)) return NAN;
    delay(20);  // first result after 8 ms, full accuracy after 20 ms
    s.running = true;
  }
  // Pressure, temperature and scale factor, each a 16-bit word followed by its CRC.
  uint8_t b[9];
  if (Wire.requestFrom(addr, (uint8_t)sizeof b) != sizeof b) {
    s.running = false;
    return NAN;
  }
  for (auto& x : b) x = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 6, 2) != b[8]) return NAN;
  const int16_t raw = (int16_t)((b[0] << 8) | b[1]);
  const int16_t scale = (int16_t)((b[6] << 8) | b[7]);  // 60 per Pa on the 500 Pa models
  if (scale <= 0) return NAN;
  return (float)raw / scale;
}

}  // namespace sdp8xx
