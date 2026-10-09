#include "web.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>

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
    if (req->hasParam("hours")) hours = constrain(req->getParam("hours")->value().toInt(), 1, 24);
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
