// Outdoor temperature from Open-Meteo (free, no account or key), for when no
// outdoor sensor is wired or as a fallback when it fails.
#pragma once

#include <Arduino.h>

namespace weather {

void begin();
float tempC();  // NAN when turned off, not fetched yet, or the last reading is over an hour old

}  // namespace weather
