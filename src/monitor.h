// The acquisition task: reads the CO16, runs the furnace model and keeps
// history, burner cycles and an event log. Everything else reads from here.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "furnace_model.h"

namespace monitor {

struct Snapshot {
  uint32_t uptimeS = 0;
  time_t epoch = 0;  // 0 until the clock is known
  furnace::Inputs in;
  furnace::State st;
  float spareC = NAN;
  float ledVolts = NAN;
  uint16_t rawInputs = 0;  // DI1 = bit 0
  bool inputsOk = false;
  bool sdOk = false;
  uint32_t filterChangedEpoch = 0;  // 0 = never recorded
  uint32_t lastCycleEpoch = 0;      // when lastCycle ended, 0 = none since boot
};

struct Event {
  uint32_t seq;
  time_t epoch;
  char text[72];
};

struct CycleRecord {
  uint32_t seq;    // 1, 2, ... since boot (restored cycles are 0)
  uint32_t epoch;  // when the cycle ended
  furnace::CycleStats stats;
};

// Raw readings for checking the wiring and calibration on the bench.
struct Bench {
  uint16_t inputs = 0;
  bool inputsOk = false;
  float aiVolts[16];
  float rtdOhms[4];
  float rtdC[4];
  uint8_t rtdFault[4];
  float ledVolts = NAN;
  bool ledLit = false;
  uint32_t ledOnMs = 0, ledOffMs = 0;
  int boardCode = 0;
};

// Starts the acquisition task. Call after config is loaded.
void begin();

// Copy of the latest state.
Snapshot snapshot();

// Writes the state message shared by MQTT, /api/state and the WebSocket.
void stateJson(const Snapshot& s, JsonObject o);

// Events newer than afterSeq, oldest first, at most max. Returns count copied.
size_t eventsAfter(uint32_t afterSeq, Event* out, size_t max);

// Writes history samples from the last `hours` (up to 30 days) as compact
// arrays, at most maxPoints rows (older samples are skipped evenly).
void historyJson(uint32_t hours, size_t maxPoints, JsonObject o);

// Recent burner cycles, newest first, at most max.
void cyclesJson(size_t max, JsonArray a);

// Burner cycles with seq above afterSeq, oldest first, for MQTT.
size_t cyclesAfter(uint32_t afterSeq, CycleRecord* out, size_t max);

// The filter was replaced: restart the blower-hours count.
void resetFilter();

// Latest bench readings. Calling it keeps bench mode on (every analog input
// and RTD channel is read) for the next 15 seconds.
Bench bench();

// Re-reads model settings from config (after the settings page saves).
void applySettings();

// Logs a free-form event (network up, firmware updated, ...).
void logEvent(const char* fmt, ...);

}  // namespace monitor
