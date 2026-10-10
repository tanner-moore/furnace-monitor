#include "ha_mqtt.h"

#include <ArduinoJson.h>
#include <NetworkClient.h>
#include <PubSubClient.h>

#include "config.h"
#include "monitor.h"
#include "net.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif

namespace ha_mqtt {

static constexpr uint32_t kPeriodicMs = 30000;
static constexpr const char* kNodeId = "furnace_co16";

static NetworkClient tcp;
static PubSubClient mqtt(tcp);
static String topicAvail, topicState, topicEvent, topicCycle, topicFilterReset, topicDiscovery, topicHaStatus;
static bool discoveryPending = false;
static uint32_t lastAttemptMs = 0, lastPublishMs = 0;
static uint32_t lastEventSeq = 0;
static uint32_t lastCycleSeq = 0;
static String lastStateJson;

// --- discovery ---------------------------------------------------------------

static JsonObject component(JsonObject cmps, const char* key, const char* platform, const char* name) {
  JsonObject c = cmps[key].to<JsonObject>();
  c["p"] = platform;
  c["name"] = name;
  c["uniq_id"] = String(kNodeId) + "_" + key;
  return c;
}

static void binary(JsonObject cmps, const char* key, const char* name, const char* devClass) {
  JsonObject c = component(cmps, key, "binary_sensor", name);
  c["val_tpl"] = String("{{ 'ON' if value_json.") + key + " else 'OFF' }}";
  if (devClass) c["dev_cla"] = devClass;
}

static JsonObject sensor(JsonObject cmps, const char* key, const char* name, const char* unit, const char* devClass,
                         const char* stateClass) {
  JsonObject c = component(cmps, key, "sensor", name);
  c["val_tpl"] = String("{{ value_json.") + key + " }}";
  if (unit) c["unit_of_meas"] = unit;
  if (devClass) c["dev_cla"] = devClass;
  if (stateClass) c["stat_cla"] = stateClass;
  return c;
}

static void publishDiscovery() {
  JsonDocument doc;
  JsonObject dev = doc["dev"].to<JsonObject>();
  dev["ids"] = kNodeId;
  dev["name"] = "Furnace";
  dev["mf"] = "KinCony";
  dev["mdl"] = "CO16 furnace monitor";
  dev["sw"] = FIRMWARE_VERSION;
  dev["cu"] = String("http://") + net::ipString() + "/";
  doc["o"]["name"] = "furnace-monitor";
  doc["stat_t"] = topicState;
  doc["avty_t"] = topicAvail;

  JsonObject cmps = doc["cmps"].to<JsonObject>();
  binary(cmps, "heat_call", "Heat call", "running");
  binary(cmps, "heat_call_2", "Heat call stage 2", "running");
  binary(cmps, "fan_call", "Fan call", "running");
  binary(cmps, "burner", "Burner", "running");
  binary(cmps, "high_fire", "High fire", "running");
  binary(cmps, "flame", "Flame", "heat");
  binary(cmps, "inducer", "Inducer", "running");
  binary(cmps, "blower", "Blower", "running");

  JsonObject problem = component(cmps, "problem", "binary_sensor", "Furnace problem");
  problem["val_tpl"] = "{{ 'ON' if value_json.problem else 'OFF' }}";
  problem["dev_cla"] = "problem";
  problem["json_attr_t"] = topicState;
  problem["json_attr_tpl"] = "{{ {'alerts': value_json.alerts, 'board_status': value_json.board_status} | tojson }}";

  JsonObject phase = sensor(cmps, "phase", "Phase", nullptr, "enum", nullptr);
  JsonArray opts = phase["options"].to<JsonArray>();
  for (int p = 0; p <= (int)furnace::Phase::Lockout; p++) opts.add(furnace::phaseName((furnace::Phase)p));

  sensor(cmps, "t_supply", "Supply temperature", "°C", "temperature", "measurement");
  sensor(cmps, "t_return", "Return temperature", "°C", "temperature", "measurement");
  sensor(cmps, "t_flue", "Flue temperature", "°C", "temperature", "measurement");
  // A temperature difference must not get the absolute-temperature unit conversion.
  sensor(cmps, "delta_t", "Temperature rise", "°C", nullptr, "measurement");
  sensor(cmps, "i_inducer", "Inducer current", "A", "current", "measurement");
  sensor(cmps, "i_blower", "Blower current", "A", "current", "measurement");
  sensor(cmps, "cycles_today", "Burner cycles today", nullptr, nullptr, "total_increasing");
  sensor(cmps, "burner_min_today", "Burner runtime today", "min", "duration", "total_increasing");
  sensor(cmps, "high_fire_min_today", "High fire runtime today", "min", "duration", "total_increasing");
  sensor(cmps, "last_cycle_s", "Last burner cycle", "s", "duration", nullptr);

  // Health of the last burner cycle. A slowly falling rise or a rising motor
  // current is the early sign of a dirty filter or a tired motor.
  sensor(cmps, "cycle_ignition_s", "Time to ignition", "s", "duration", "measurement");
  sensor(cmps, "cycle_rise_low", "Low fire temperature rise", "°C", nullptr, "measurement");
  sensor(cmps, "cycle_rise_high", "High fire temperature rise", "°C", nullptr, "measurement");
  sensor(cmps, "cycle_flue_max", "Peak flue temperature", "°C", "temperature", "measurement");
  sensor(cmps, "cycle_inducer_a", "Inducer current per cycle", "A", "current", "measurement");
  sensor(cmps, "cycle_blower_a", "Blower current per cycle", "A", "current", "measurement");
  sensor(cmps, "total_cycles", "Burner cycles", nullptr, nullptr, "total_increasing");
  sensor(cmps, "total_failed_ignitions", "Failed ignition trials", nullptr, nullptr, "total_increasing");
  sensor(cmps, "total_burner_h", "Burner runtime", "h", "duration", "total_increasing");
  sensor(cmps, "total_high_fire_h", "High fire runtime", "h", "duration", "total_increasing");
  sensor(cmps, "total_blower_h", "Blower runtime", "h", "duration", "total_increasing");

  // Air filter.
  sensor(cmps, "filter_h", "Filter blower hours", "h", "duration", "measurement");
  sensor(cmps, "filter_pct", "Filter life left", "%", nullptr, "measurement");
  binary(cmps, "filter_due", "Filter change due", "problem");
  JsonObject reset = component(cmps, "filter_reset", "button", "Filter changed");
  reset["cmd_t"] = topicFilterReset;
  reset["pl_prs"] = "PRESS";

  if (config.diCo) binary(cmps, "co_alarm", "Carbon monoxide", "carbon_monoxide");

  JsonObject board = sensor(cmps, "board_status", "Control board status", nullptr, nullptr, nullptr);
  board["val_tpl"] = "{{ value_json.board_status }}";
  board["ent_cat"] = "diagnostic";
  JsonObject uptime = sensor(cmps, "uptime_s", "Uptime", "s", "duration", nullptr);
  uptime["ent_cat"] = "diagnostic";
  JsonObject sd = component(cmps, "sd", "binary_sensor", "SD card logging");
  sd["val_tpl"] = "{{ 'ON' if value_json.sd else 'OFF' }}";
  sd["dev_cla"] = "running";
  sd["ent_cat"] = "diagnostic";

  String payload;
  serializeJson(doc, payload);
  if (!mqtt.publish(topicDiscovery.c_str(), (const uint8_t*)payload.c_str(), payload.length(), true))
    log_e("discovery publish failed (%u bytes)", payload.length());
}

// --- state and events --------------------------------------------------------

static void publishState(bool force) {
  JsonDocument doc;
  const monitor::Snapshot s = monitor::snapshot();
  monitor::stateJson(s, doc.to<JsonObject>());
  doc["link"] = net::linkName();
  doc["ip"] = net::ipString();
  doc.remove("uptime_s");  // changes every second; sent only with the periodic publish
  String withoutUptime;
  serializeJson(doc, withoutUptime);
  if (!force && withoutUptime == lastStateJson) return;
  lastStateJson = withoutUptime;
  doc["uptime_s"] = s.uptimeS;
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topicState.c_str(), payload.c_str());
  lastPublishMs = millis();
}

static void publishEvents() {
  monitor::Event ev[8];
  size_t n;
  while ((n = monitor::eventsAfter(lastEventSeq, ev, 8)) > 0) {
    for (size_t i = 0; i < n; i++) {
      JsonDocument doc;
      doc["seq"] = ev[i].seq;
      doc["time"] = (uint32_t)ev[i].epoch;
      doc["text"] = ev[i].text;
      String payload;
      serializeJson(doc, payload);
      mqtt.publish(topicEvent.c_str(), payload.c_str());
      lastEventSeq = ev[i].seq;
    }
  }
}

static void publishCycles() {
  monitor::CycleRecord c[4];
  size_t n;
  while ((n = monitor::cyclesAfter(lastCycleSeq, c, 4)) > 0) {
    for (size_t i = 0; i < n; i++) {
      JsonDocument doc;
      const furnace::CycleStats& s = c[i].stats;
      doc["time"] = c[i].epoch;
      doc["seconds"] = s.seconds;
      doc["high_fire_s"] = s.highFireSeconds;
      doc["ignition_s"] = s.ignitionSeconds;
      auto put = [&](const char* k, float v) {
        if (isnan(v)) doc[k] = nullptr; else doc[k] = roundf(v * 100) / 100;
      };
      put("rise_low", s.riseLowC);
      put("rise_high", s.riseHighC);
      put("flue_max", s.flueMaxC);
      put("inducer_a", s.inducerAmps);
      put("blower_a", s.blowerAmps);
      String payload;
      serializeJson(doc, payload);
      mqtt.publish(topicCycle.c_str(), payload.c_str());
      lastCycleSeq = c[i].seq;
    }
  }
}

// The only commands accepted: Home Assistant coming online (resend discovery)
// and the filter-changed button, which resets a counter. Nothing here can
// reach the furnace.
static void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  if (topicHaStatus == topic && len == 6 && memcmp(payload, "online", 6) == 0) discoveryPending = true;
  if (topicFilterReset == topic && len == 5 && memcmp(payload, "PRESS", 5) == 0) monitor::resetFilter();
}

static void connect() {
  mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);
  const String clientId = config.hostname;
  const bool ok = config.mqttUser.length()
                      ? mqtt.connect(clientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(),
                                     topicAvail.c_str(), 1, true, "offline")
                      : mqtt.connect(clientId.c_str(), nullptr, nullptr, topicAvail.c_str(), 1, true, "offline");
  if (!ok) return;
  monitor::logEvent("mqtt connected to %s", config.mqttHost.c_str());
  mqtt.publish(topicAvail.c_str(), "online", true);
  mqtt.subscribe(topicHaStatus.c_str());
  mqtt.subscribe(topicFilterReset.c_str());
  publishDiscovery();
  publishState(true);
}

void begin() {
  topicAvail = config.baseTopic + "/availability";
  topicState = config.baseTopic + "/state";
  topicEvent = config.baseTopic + "/event";
  topicCycle = config.baseTopic + "/cycle";
  topicFilterReset = config.baseTopic + "/filter_reset";
  topicDiscovery = config.discoveryPrefix + "/device/" + kNodeId + "/config";
  topicHaStatus = config.discoveryPrefix + "/status";
  mqtt.setBufferSize(12288);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMessage);
  // Don't replay events from before the first connection.
  monitor::Event dummy;
  while (monitor::eventsAfter(lastEventSeq, &dummy, 1)) lastEventSeq = dummy.seq;
}

void loop() {
  if (config.mqttHost.isEmpty() || !net::connected()) return;
  if (!mqtt.connected()) {
    if (millis() - lastAttemptMs < 10000) return;
    lastAttemptMs = millis();
    connect();
    return;
  }
  mqtt.loop();
  if (discoveryPending) {
    discoveryPending = false;
    publishDiscovery();
    publishState(true);
  }
  publishState(millis() - lastPublishMs >= kPeriodicMs);
  publishEvents();
  publishCycles();
}

bool connected() { return mqtt.connected(); }

}  // namespace ha_mqtt
