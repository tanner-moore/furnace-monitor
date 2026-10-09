// Minimal DS3231 access so timestamps are right before NTP syncs.
// The RTC holds UTC. Call only from the task that owns the I2C bus.
#pragma once

#include <time.h>

namespace rtc {

// Reads the RTC; returns 0 if it is missing or was never set.
time_t readUtc();

// Writes the current UTC time.
bool writeUtc(time_t t);

}  // namespace rtc
