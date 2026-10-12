#include "config.h"

#include <LittleFS.h>

Config config;

static const char* kPath = "/config.json";
// Settings are written here first and renamed over kPath, so a power cut mid-save leaves the old
// file (or a complete new one) rather than a half-written one.
static const char* kTmpPath = "/config.tmp";

// Each field is listed once here and used for both directions.
#define CONFIG_FIELDS(X)                                   \
  X(hostname) X(wifiSsid) X(tz) X(ntpServer)               \
  X(mqttHost) X(mqttPort) X(mqttUser) X(baseTopic)         \
  X(discoveryPrefix) X(webUser)                            \
  X(diW1) X(diW2) X(diG) X(diMvl) X(diMvh)                 \
  X(filterLifeHours) X(sdLogging) X(display)               \
  X(displayWidth) X(displayHeight) X(displayRotation)      \
  X(aiInducer) X(aiBlower) X(aiLed)                        \
  X(inducerAmpsPerVolt) X(blowerAmpsPerVolt) X(ledOnVolts) \
  X(rtdSupply) X(rtdReturn) X(rtdFlue) X(rtdSpare)         \
  X(offsetSupply) X(offsetReturn) X(offsetFlue) X(offsetSpare) \
  X(boardPreset) X(aiCo) X(coPpmPerVolt) X(coAlertPpm) X(aiRoom) \
  X(roomLowC) X(roomHighC) X(spareName) X(tempUnit)        \
  X(aiOutdoor) X(outdoorLowC) X(outdoorHighC) X(weatherOn) \
  X(weatherLat) X(weatherLon) X(dashLayout) X(dashChart)      \
  X(aiPress1) X(aiPress2) X(aiPress3) X(press1Name) X(press2Name) X(press3Name) \
  X(press1LowPa) X(press1HighPa) X(press2LowPa) X(press2HighPa)    \
  X(press3LowPa) X(press3HighPa) X(pressUnit)                 \
  X(press1Type) X(press2Type) X(press3Type) X(filterPressSensor) X(filterMode) X(filterDirtyPa)

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

static bool readFile(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) log_e("%s parse failed: %s", path, err.c_str());
  return !err;
}

bool Config::load() {
  JsonDocument doc;
  // A complete temp file means the last save stopped just before its rename.
  if (!readFile(kPath, doc) && !readFile(kTmpPath, doc)) return false;
  fromJson(doc.as<JsonObjectConst>());
  return true;
}

bool Config::save() const {
  JsonDocument doc;
  toJson(doc.to<JsonObject>(), true);
  const size_t len = measureJson(doc);
  File f = LittleFS.open(kTmpPath, "w");
  if (!f) return false;
  const bool written = serializeJson(doc, f) == len;
  f.close();
  if (!written) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  if (LittleFS.rename(kTmpPath, kPath)) return true;
  // In case the file system won't rename over an existing file.
  LittleFS.remove(kPath);
  return LittleFS.rename(kTmpPath, kPath);
}

// Each input number may be used once. `used` has a bit per input; 0 means not fitted.
static bool claim(uint32_t& used, uint8_t n, uint8_t max, const char* kind, const char* what, String& why) {
  if (!n) return true;
  if (n > max) {
    why = String(what) + ": " + kind + " " + n + " doesn't exist (1-" + max + ")";
    return false;
  }
  if (used & (1u << n)) {
    why = String(what) + ": " + kind + " " + n + " is already used by another sensor";
    return false;
  }
  used |= 1u << n;
  return true;
}

static bool positive(float v, const char* what, String& why) {
  if (isfinite(v) && v > 0) return true;
  why = String(what) + " must be a number above 0";
  return false;
}

static bool range(float lo, float hi, const char* what, String& why) {
  if (isfinite(lo) && isfinite(hi) && lo < hi) return true;
  why = String(what) + ": the 0 V value must be below the 10 V value";
  return false;
}

bool Config::validate(String& why) const {
  uint32_t di = 0, ai = 0, rtd = 0;
  const bool ok =
      claim(di, diW1, 16, "DI", "Heat call W1", why) && claim(di, diW2, 16, "DI", "Heat call W2", why) &&
      claim(di, diG, 16, "DI", "Fan call G", why) && claim(di, diMvl, 16, "DI", "Gas valve low", why) &&
      claim(di, diMvh, 16, "DI", "Gas valve high", why) &&
      claim(ai, aiInducer, 16, "AI", "Inducer current", why) && claim(ai, aiBlower, 16, "AI", "Blower current", why) &&
      claim(ai, aiLed, 16, "AI", "Control board light", why) && claim(ai, aiCo, 16, "AI", "CO sensor", why) &&
      claim(ai, aiRoom, 16, "AI", "Room temperature", why) && claim(ai, aiOutdoor, 16, "AI", "Outdoor temperature", why) &&
      claim(ai, press1Type == "ai" ? aiPress1 : 0, 16, "AI", press1Name.c_str(), why) &&
      claim(ai, press2Type == "ai" ? aiPress2 : 0, 16, "AI", press2Name.c_str(), why) &&
      claim(ai, press3Type == "ai" ? aiPress3 : 0, 16, "AI", press3Name.c_str(), why) &&
      claim(rtd, rtdSupply, 4, "PT100", "Supply air", why) && claim(rtd, rtdReturn, 4, "PT100", "Return air", why) &&
      claim(rtd, rtdFlue, 4, "PT100", "Flue", why) && claim(rtd, rtdSpare, 4, "PT100", spareName.c_str(), why) &&
      positive(inducerAmpsPerVolt, "Inducer amps per volt", why) &&
      positive(blowerAmpsPerVolt, "Blower amps per volt", why) && positive(coPpmPerVolt, "CO ppm per volt", why) &&
      positive(ledOnVolts, "Control board light threshold", why) &&
      range(roomLowC, roomHighC, "Room temperature sensor", why) &&
      range(outdoorLowC, outdoorHighC, "Outdoor temperature sensor", why) &&
      range(press1LowPa, press1HighPa, press1Name.c_str(), why) &&
      range(press2LowPa, press2HighPa, press2Name.c_str(), why) &&
      range(press3LowPa, press3HighPa, press3Name.c_str(), why);
  return ok;
}
