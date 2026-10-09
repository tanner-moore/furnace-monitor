// Network bring-up: W5500 Ethernet first, Wi-Fi as a fallback, and a setup
// access point when neither connects. Also starts NTP.
#pragma once

#include <Arduino.h>

namespace net {

void begin();
void loop();

bool connected();
String ipString();
const char* linkName();  // "eth", "wifi", "ap" or "none"
int wifiRssi();          // 0 when not on Wi-Fi

}  // namespace net
