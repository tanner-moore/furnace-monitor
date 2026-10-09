// The acquisition task: reads the CO16, runs the furnace model and keeps
// history and an event log. Everything else reads from here.
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
};

struct Event {
  uint32_t seq;
  time_t epoch;
  char text[72];
};

// Starts the acquisition task. Call after config is loaded.
void begin();

// Copy of the latest state.
Snapshot snapshot();

// Writes the state message shared by MQTT, /api/state and the WebSocket.
void stateJson(const Snapshot& s, JsonObject o);

// Events newer than afterSeq, oldest first, at most max. Returns count copied.
size_t eventsAfter(uint32_t afterSeq, Event* out, size_t max);

// Writes history samples from the last `hours` as compact arrays, at most
// maxPoints rows (older samples are skipped evenly).
void historyJson(uint32_t hours, size_t maxPoints, JsonObject o);

// Re-reads model settings from config (after the settings page saves).
void applySettings();

// Logs a free-form event (network up, firmware updated, ...).
void logEvent(const char* fmt, ...);

}  // namespace monitor
