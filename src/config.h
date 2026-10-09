// Persistent settings, stored as /config.json on LittleFS and edited from the web page.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "furnace_model.h"

struct Config {
  // Network
  String hostname = "furnace-co16";
  String wifiSsid;      // optional fallback when Ethernet has no link
  String wifiPass;
  String tz = "CST6CDT,M3.2.0,M11.1.0";  // POSIX TZ string
  String ntpServer = "pool.ntp.org";

  // MQTT
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPass;
  String baseTopic = "furnace/co16";
  String discoveryPrefix = "homeassistant";

  // Web page login for settings and firmware updates
  String webUser = "admin";
  String webPass = "furnace";

  // Digital inputs (1-16, 0 = not wired)
  uint8_t diW1 = 1, diW2 = 2, diG = 4, diMvl = 5, diMvh = 6;

  // Analog inputs (1-16, 0 = not wired) and scaling
  uint8_t aiInducer = 1, aiBlower = 2, aiLed = 3;
  float inducerAmpsPerVolt = 0.5f;  // e.g. 0-5 A CT with 0-10 V output
  float blowerAmpsPerVolt = 2.0f;   // e.g. 0-20 A CT with 0-10 V output
  float ledOnVolts = 2.0f;          // light sensor threshold

  // PT100 channels (1-4, 0 = not fitted) and calibration offsets
  uint8_t rtdSupply = 1, rtdReturn = 2, rtdFlue = 3, rtdSpare = 0;
  float offsetSupply = 0, offsetReturn = 0, offsetFlue = 0, offsetSpare = 0;

  furnace::Settings model;

  bool load();
  bool save() const;
  void toJson(JsonObject o, bool includeSecrets) const;
  void fromJson(JsonObjectConst o);
};

extern Config config;
