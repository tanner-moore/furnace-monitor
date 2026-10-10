// Decodes the White-Rodgers 50A51 diagnostic LED from a light sensor reading.
//
// The board shows a slow or fast steady blink when healthy, stays lit for an
// internal fault, and repeats groups of N flashes (separated by a long pause)
// for fault code N. We count flashes per group and report a code once the same
// count is seen in two groups in a row.
//
// Feed it ~20 samples per second. The timing thresholds are first guesses and
// should be checked against the real LED once the sensor is fitted.
#pragma once

#include <cstdint>

namespace furnace {

struct FlashSettings {
  uint32_t groupGapMs = 1500;  // dark gap that ends a flash group
  uint32_t steadyOnMs = 5000;  // lit this long = steady on
  uint32_t staleMs = 20000;    // no flashes for this long = back to normal/unknown
};

class FlashDecoder {
 public:
  static constexpr int kCodeNormal = 0;
  static constexpr int kCodeSteadyOn = 99;

  using Settings = FlashSettings;

  explicit FlashDecoder(const Settings& s = Settings()) : s_(s) {}

  // lit: LED is on (caller applies the voltage threshold).
  void sample(bool lit, uint32_t nowMs);

  // 0 = normal or unknown, 2..9 = flash code, 99 = steady on.
  int code() const { return code_; }

  // Length of the most recent complete lit and dark periods, for checking the
  // timing thresholds against the real LED (bench page).
  uint32_t lastOnMs() const { return lastOnMs_; }
  uint32_t lastOffMs() const { return lastOffMs_; }

  // Human readable meaning of a 50A51 code.
  static const char* describe(int code);

 private:
  void endGroup();

  Settings s_;
  bool started_ = false;
  bool lit_ = false;
  uint32_t edgeMs_ = 0;      // time of last transition
  uint32_t lastFlashMs_ = 0; // time of last rising edge
  int count_ = 0;            // flashes in the current group
  int lastGroup_ = -1;       // count of the previous complete group
  int code_ = kCodeNormal;
  uint32_t lastOnMs_ = 0;
  uint32_t lastOffMs_ = 0;
};

}  // namespace furnace
