// The CO16's built-in ST7789 screen: phase, temperatures, alerts and the
// board's address, redrawn once a second.
#pragma once

namespace display {

// Starts the display task if the screen is enabled in settings.
void begin();

}  // namespace display
