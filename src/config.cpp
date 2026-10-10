#include "config.h"

#include <LittleFS.h>

Config config;

static const char* kPath = "/config.json";

// Each field is listed once here and used for both directions.
#define CONFIG_FIELDS(X)                                   \
  X(hostname) X(wifiSsid) X(tz) X(ntpServer)               \
  X(mqttHost) X(mqttPort) X(mqttUser) X(baseTopic)         \
  X(discoveryPrefix) X(webUser)                            \
  X(diW1) X(diW2) X(diG) X(diMvl) X(diMvh) X(diCo)       \
  X(coOnOpen) X(filterLifeHours) X(sdLogging) X(display)   \
  X(displayWidth) X(displayHeight) X(displayRotation)      \
  X(displayFahrenheit)                                     \
  X(aiInducer) X(aiBlower) X(aiLed)                        \
  X(inducerAmpsPerVolt) X(blowerAmpsPerVolt) X(ledOnVolts) \
  X(rtdSupply) X(rtdReturn) X(rtdFlue) X(rtdSpare)         \
  X(offsetSupply) X(offsetReturn) X(offsetFlue) X(offsetSpare)

#define SECRET_FIELDS(X) X(wifiPass) X(mqttPass) X(webPass)

#define MODEL_FIELDS(X)                                                    \
  X(inducerOnAmps) X(inducerHighAmps) X(blowerOnAmps) X(flameConfirmS)     \
  X(ignitionTimeoutS) X(shortCycleS) X(blowerDelayS) X(settleS)            \
  X(minDeltaTC) X(maxSupplyC) X(minFlueRiseC) X(ignitionTrials)          \
  X(riseSettleS)

void Config::toJson(JsonObject o, bool includeSecrets) const {
#define OUT(f) o[#f] = f;
  CONFIG_FIELDS(OUT)
  if (includeSecrets) { SECRET_FIELDS(OUT) }
#undef OUT
  JsonObject m = o["model"].to<JsonObject>();
#define OUTM(f) m[#f] = model.f;
  MODEL_FIELDS(OUTM)
#undef OUTM
}

void Config::fromJson(JsonObjectConst o) {
#define IN(f) \
  if (!o[#f].isNull()) f = o[#f].as<decltype(f)>();
  CONFIG_FIELDS(IN)
#undef IN
  // Secrets left blank on the settings page keep their current value.
#define INS(f) \
  if (o[#f].is<const char*>() && strlen(o[#f].as<const char*>()) > 0) f = o[#f].as<String>();
  SECRET_FIELDS(INS)
#undef INS
  JsonObjectConst m = o["model"];
  if (!m.isNull()) {
#define INM(f) \
  if (!m[#f].isNull()) model.f = m[#f].as<decltype(model.f)>();
    MODEL_FIELDS(INM)
#undef INM
  }
}

bool Config::load() {
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    log_e("config parse failed: %s", err.c_str());
    return false;
  }
  fromJson(doc.as<JsonObjectConst>());
  return true;
}

bool Config::save() const {
  JsonDocument doc;
  toJson(doc.to<JsonObject>(), true);
  File f = LittleFS.open(kPath, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}
