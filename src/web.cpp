#include "web.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>

#include "co16_io.h"
#include "config.h"
#include "ha_mqtt.h"
#include "monitor.h"
#include "net.h"

namespace web {

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint32_t lastPushMs = 0;
static bool rebootPending = false;
static uint32_t rebootAtMs = 0;

static bool authorized(AsyncWebServerRequest* req) {
  if (req->authenticate(config.webUser.c_str(), config.webPass.c_str())) return true;
  req->requestAuthentication();
  return false;
}

static void sendJson(AsyncWebServerRequest* req, JsonDocument& doc) {
  AsyncResponseStream* res = req->beginResponseStream("application/json");
  serializeJson(doc, *res);
  req->send(res);
}

static String liveJson() {
  JsonDocument doc;
  monitor::stateJson(monitor::snapshot(), doc.to<JsonObject>());
  doc["link"] = net::linkName();
  doc["ip"] = net::ipString();
  doc["rssi"] = net::wifiRssi();
  doc["mqtt"] = ha_mqtt::connected();
  String out;
  serializeJson(doc, out);
  return out;
}

static void handleUpload(AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len,
                         bool final) {
  if (index == 0) {
    if (!req->authenticate(config.webUser.c_str(), config.webPass.c_str())) return;
    const bool fs = req->hasParam("target") && req->getParam("target")->value() == "fs";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, fs ? U_SPIFFS : U_FLASH)) Update.printError(Serial);
  }
  if (Update.isRunning() && Update.write(data, len) != len) Update.printError(Serial);
  if (final && Update.isRunning()) {
    if (Update.end(true)) {
      monitor::logEvent("update installed: %s", filename.c_str());
    } else {
      Update.printError(Serial);
    }
  }
}

void begin() {
  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*, size_t) {
    if (type == WS_EVT_CONNECT) client->text(liveJson());
  });
  server.addHandler(&ws);

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/json", liveJson());
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint32_t hours = 24;
    if (req->hasParam("hours")) hours = constrain(req->getParam("hours")->value().toInt(), 1, 720);
    JsonDocument doc;
    monitor::historyJson(hours, 720, doc.to<JsonObject>());
    sendJson(req, doc);
  });

  server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest* req) {
    static monitor::Event ev[200];
    const size_t n = monitor::eventsAfter(0, ev, 200);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = n; i-- > 0;) {  // newest first
      JsonObject e = arr.add<JsonObject>();
      e["seq"] = ev[i].seq;
      e["time"] = (uint32_t)ev[i].epoch;
      e["text"] = ev[i].text;
    }
    sendJson(req, doc);
  });

  server.on("/api/cycles", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument doc;
    monitor::cyclesJson(100, doc.to<JsonArray>());
    sendJson(req, doc);
  });

  // Resets the filter's blower-hours count. Changes nothing on the furnace.
  server.on("/api/filter/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!authorized(req)) return;
    monitor::resetFilter();
    req->send(200, "text/plain", "filter count reset");
  });

  // Raw readings for wiring checks and calibration. Reading it keeps bench mode
  // on (all 16 analog inputs and all 4 RTD channels) for 15 s.
  server.on("/api/bench", HTTP_GET, [](AsyncWebServerRequest* req) {
    const monitor::Bench b = monitor::bench();
    const co16::ChipStatus chips = co16::chipStatus();
    JsonDocument doc;
    doc["inputs"] = b.inputs;
    doc["inputs_ok"] = b.inputsOk;
    JsonArray ai = doc["ai_v"].to<JsonArray>();
    for (float v : b.aiVolts) {
      if (isnan(v)) ai.add(nullptr); else ai.add(roundf(v * 1000) / 1000);
    }
    JsonArray rtd = doc["rtd"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
      JsonObject r = rtd.add<JsonObject>();
      if (isnan(b.rtdOhms[i])) r["ohms"] = nullptr; else r["ohms"] = roundf(b.rtdOhms[i] * 100) / 100;
      if (isnan(b.rtdC[i])) r["c"] = nullptr; else r["c"] = roundf(b.rtdC[i] * 10) / 10;
      r["fault"] = b.rtdFault[i];
    }
    JsonObject led = doc["led"].to<JsonObject>();
    if (isnan(b.ledVolts)) led["v"] = nullptr; else led["v"] = roundf(b.ledVolts * 1000) / 1000;
    led["lit"] = b.ledLit;
    led["on_ms"] = b.ledOnMs;
    led["off_ms"] = b.ledOffMs;
    led["code"] = b.boardCode;
    JsonObject c = doc["chips"].to<JsonObject>();
    c["relays"] = chips.relays;
    c["inputs"] = chips.inputs;
    JsonArray adc = c["adc"].to<JsonArray>();
    for (bool ok : chips.adc) adc.add(ok);
    c["rtd"] = chips.rtd;
    c["sd"] = monitor::snapshot().sdOk;
    // Only the input assignments and scaling, not the full (login-only) settings.
    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["diW1"] = config.diW1;
    cfg["diW2"] = config.diW2;
    cfg["diG"] = config.diG;
    cfg["diMvl"] = config.diMvl;
    cfg["diMvh"] = config.diMvh;
    cfg["diCo"] = config.diCo;
    cfg["aiInducer"] = config.aiInducer;
    cfg["aiBlower"] = config.aiBlower;
    cfg["aiLed"] = config.aiLed;
    cfg["inducerAmpsPerVolt"] = config.inducerAmpsPerVolt;
    cfg["blowerAmpsPerVolt"] = config.blowerAmpsPerVolt;
    cfg["ledOnVolts"] = config.ledOnVolts;
    cfg["rtdSupply"] = config.rtdSupply;
    cfg["rtdReturn"] = config.rtdReturn;
    cfg["rtdFlue"] = config.rtdFlue;
    cfg["rtdSpare"] = config.rtdSpare;
    sendJson(req, doc);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authorized(req)) return;
    JsonDocument doc;
    config.toJson(doc.to<JsonObject>(), false);
    sendJson(req, doc);
  });

  auto* save = new AsyncCallbackJsonWebHandler("/api/config", [](AsyncWebServerRequest* req, JsonVariant& body) {
    if (!authorized(req)) return;
    config.fromJson(body.as<JsonObjectConst>());
    if (!config.save()) {
      req->send(500, "text/plain", "could not save settings");
      return;
    }
    monitor::applySettings();
    monitor::logEvent("settings saved, restarting");
    req->send(200, "text/plain", "saved; restarting");
    rebootPending = true;
    rebootAtMs = millis() + 1000;
  });
  save->setMethod(HTTP_POST);
  server.addHandler(save);

  server.on(
      "/update", HTTP_POST,
      [](AsyncWebServerRequest* req) {
        if (!authorized(req)) return;
        const bool ok = !Update.hasError();
        req->send(ok ? 200 : 500, "text/plain", ok ? "update ok; restarting" : "update failed");
        if (ok) {
          rebootPending = true;
          rebootAtMs = millis() + 1000;
        }
      },
      handleUpload);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "not found"); });
  server.begin();
}

void loop() {
  if (rebootPending && (int32_t)(millis() - rebootAtMs) >= 0) ESP.restart();
  if (millis() - lastPushMs < 1000) return;
  lastPushMs = millis();
  ws.cleanupClients();
  if (ws.count()) ws.textAll(liveJson());
}

}  // namespace web
