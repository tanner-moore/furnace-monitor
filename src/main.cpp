// Furnace monitor for a KinCony CO16.
//
// MONITOR ONLY. This firmware never energizes the CO16's relays and offers no
// way to control the furnace. Every furnace signal reaches the board through
// isolation: interface relays, clamp-on current transformers, RTD probes and an
// optical sensor on the control board's LED.
#include <Arduino.h>
#include <LittleFS.h>

#include "config.h"
#include "display.h"
#include "ha_mqtt.h"
#include "monitor.h"
#include "net.h"
#include "web.h"

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) log_e("LittleFS mount failed");
  if (!config.load()) log_w("no saved settings, using defaults");
  // Apply the time zone before anything reads local time (day counters, SD log
  // file names, the screen). net::begin() sets it again along with NTP.
  setenv("TZ", config.tz.c_str(), 1);
  tzset();

  monitor::begin();  // also forces the relays off
  monitor::logEvent("boot");

  net::begin();
  ha_mqtt::begin();
  web::begin();
  display::begin();

  // Restart if the main loop stalls.
  enableLoopWDT();
}

void loop() {
  net::loop();
  ha_mqtt::loop();
  web::loop();
  delay(10);
}
