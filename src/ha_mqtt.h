// MQTT publishing with Home Assistant device discovery. Publishes only; the one
// subscription is Home Assistant's birth message, so nothing can be controlled.
#pragma once

namespace ha_mqtt {

void begin();
void loop();
bool connected();

}  // namespace ha_mqtt
