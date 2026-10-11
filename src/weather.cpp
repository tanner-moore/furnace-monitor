#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "config.h"
#include "monitor.h"
#include "net.h"

namespace weather {

static constexpr uint32_t kEveryMs = 15 * 60 * 1000;
static constexpr uint32_t kRetryMs = 60 * 1000;
static constexpr uint32_t kStaleMs = 60 * 60 * 1000;

static volatile float lastC = NAN;
static volatile uint32_t lastOkMs = 0;

static bool locationSet() { return config.weatherLat != 0 || config.weatherLon != 0; }

static bool fetch() {
  char url[160];
  snprintf(url, sizeof url,
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m",
           config.weatherLat, config.weatherLon);
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) return false;
  const int code = http.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      JsonVariantConst t = doc["current"]["temperature_2m"];
      if (t.is<float>()) {
        lastC = t.as<float>();
        lastOkMs = millis();
        ok = true;
      }
    }
  }
  http.end();
  if (!ok) log_w("Open-Meteo fetch failed (HTTP %d)", code);
  return ok;
}

// Own task, so a slow or failed request never holds up the main loop.
static void task(void*) {
  bool wasOk = true;
  for (;;) {
    if (!config.weatherOn || !locationSet() || !net::connected()) {
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }
    const bool ok = fetch();
    if (ok != wasOk) monitor::logEvent(ok ? "outdoor temperature from Open-Meteo restored" : "Open-Meteo fetch failed");
    wasOk = ok;
    vTaskDelay(pdMS_TO_TICKS(ok ? kEveryMs : kRetryMs));
  }
}

void begin() {
  if (config.weatherOn) xTaskCreate(task, "weather", 8192, nullptr, 1, nullptr);
}

float tempC() {
  if (!config.weatherOn || !lastOkMs || millis() - lastOkMs > kStaleMs) return NAN;
  return lastC;
}

}  // namespace weather
