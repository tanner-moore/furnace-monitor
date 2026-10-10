// SD card log: one CSV row per minute in a file per local day, plus burner
// cycles and events. At boot the monitor reads recent rows back so history and
// the event log survive a restart. Optional: everything is skipped when no card
// is fitted or logging is off.
#pragma once

#include <Arduino.h>

#include "furnace_model.h"

namespace sdlog {

// Averages over one minute. NaN where a sensor is not fitted.
struct MinuteRow {
  uint32_t epoch = 0;
  float supplyC = NAN, returnC = NAN, flueC = NAN, inducerAmps = NAN, blowerAmps = NAN;
  uint8_t flags = 0;  // OR of the history flag bits seen during the minute
  uint8_t phase = 0;
};

struct CycleRow {
  uint32_t epoch = 0;  // when the cycle ended
  furnace::CycleStats stats;
};

// Mounts the card. Call after co16::begin() (which starts the SPI bus).
bool begin();
bool ok();

// Boot-time reads, before the other tasks start. Rows come oldest first.
void readMinutes(uint32_t sinceEpoch, void (*fn)(const MinuteRow&));
void readCycles(size_t maxRows, void (*fn)(const CycleRow&));
void readEvents(size_t maxRows, void (*fn)(uint32_t epoch, const char* text));

// Queue a row for the writer task. Never blocks; drops the row if the queue is full.
void logMinute(const MinuteRow& r);
void logCycle(const CycleRow& r);
void logEvent(uint32_t epoch, const char* text);

}  // namespace sdlog
