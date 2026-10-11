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
  String tz = "PST8PDT,M3.2.0,M11.1.0";  // POSIX TZ string
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

  // Optional 0-10 V transmitters on spare analog inputs.
  uint8_t aiCo = 0;                 // CO sensor, ppm (AI 4 suggested)
  float coPpmPerVolt = 25.0f;       // e.g. 0-250 ppm sensor with 0-10 V output
  float coAlertPpm = 35.0f;         // CO alert at or above this level
  uint8_t aiRoom = 0;               // room temperature sensor (AI 5 suggested)
  float roomLowC = 0.0f, roomHighC = 50.0f;  // what the room sensor reads at 0 V and at 10 V
  uint8_t aiOutdoor = 0;            // outdoor temperature sensor (AI 6 suggested)
  float outdoorLowC = -40.0f, outdoorHighC = 60.0f;  // what the outdoor sensor reads at 0 V and at 10 V

  // Outdoor temperature from Open-Meteo, used when no outdoor sensor is wired or it fails.
  bool weatherOn = false;
  float weatherLat = 0.0f, weatherLon = 0.0f;  // decimal degrees; west and south are negative

  // PT100 channels (1-4, 0 = not fitted) and calibration offsets
  uint8_t rtdSupply = 1, rtdReturn = 2, rtdFlue = 3, rtdSpare = 0;
  float offsetSupply = 0, offsetReturn = 0, offsetFlue = 0, offsetSpare = 0;
  String spareName = "Spare";  // shown on the dashboard and in Home Assistant

  // Control board preset last picked on the settings page. Only the page uses it: it decides
  // which timing values count as defaults there.
  String boardPreset = "generic";

  // Temperature units for the web page and the screen: "F" or "C". MQTT always sends °C and
  // Home Assistant converts to its own units.
  String tempUnit = "C";

  // Air filter: blower hours between changes.
  uint16_t filterLifeHours = 500;

  // SD card logging (minute samples, cycles and events) and the built-in screen.
  bool sdLogging = true;
  bool display = true;
  uint16_t displayWidth = 240, displayHeight = 240;  // ST7789 panel size, to confirm on the board
  uint8_t displayRotation = 0;                      // 0-3

  furnace::Settings model;

  bool load();
  bool save() const;
  void toJson(JsonObject o, bool includeSecrets) const;
  void fromJson(JsonObjectConst o);
};

extern Config config;
