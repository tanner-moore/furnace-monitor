#include "net.h"

#include <ESPmDNS.h>
#include <ETH.h>
#include <WiFi.h>

#include "config.h"
#include "monitor.h"
#include "pins.h"

namespace net {

static constexpr uint32_t kEthGraceMs = 15000;   // wait this long for Ethernet before trying Wi-Fi
static constexpr uint32_t kSetupApAfterMs = 60000;

static volatile bool ethUp = false;
static volatile bool wifiUp = false;
static bool wifiStarted = false;
static bool apStarted = false;
static bool servicesStarted = false;
static uint32_t bootMs;

static void onEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethUp = true;
      monitor::logEvent("ethernet up, %s", ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_LOST_IP:
      if (ethUp) monitor::logEvent("ethernet down");
      ethUp = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiUp = true;
      monitor::logEvent("wifi up, %s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (wifiUp) monitor::logEvent("wifi down");
      wifiUp = false;
      break;
    default:
      break;
  }
}

void begin() {
  bootMs = millis();
  Network.onEvent(onEvent);
  ETH.setHostname(config.hostname.c_str());
  // The W5500 gets the second general-purpose SPI host; the first (FSPI) serves
  // the MAX31865, display and SD card.
  if (!ETH.begin(ETH_PHY_W5500, 1, PIN_ETH_CS, PIN_ETH_INT, PIN_ETH_RST, SPI3_HOST, PIN_ETH_SCK, PIN_ETH_MISO,
                 PIN_ETH_MOSI)) {
    log_e("W5500 init failed");
  }
  configTzTime(config.tz.c_str(), config.ntpServer.c_str());
}

void loop() {
  const uint32_t up = millis() - bootMs;

  if (!ethUp && !wifiStarted && config.wifiSsid.length() && up > kEthGraceMs) {
    wifiStarted = true;
    WiFi.setHostname(config.hostname.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
  }

  if (!connected() && !apStarted && up > kSetupApAfterMs) {
    apStarted = true;
    WiFi.mode(wifiStarted ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP("furnace-setup");
    monitor::logEvent("no network: setup access point 'furnace-setup' at %s", WiFi.softAPIP().toString().c_str());
  }

  if (connected() && !servicesStarted) {
    servicesStarted = true;
    if (MDNS.begin(config.hostname.c_str())) MDNS.addService("http", "tcp", 80);
  }
}

bool connected() { return ethUp || wifiUp; }

String ipString() {
  if (ethUp) return ETH.localIP().toString();
  if (wifiUp) return WiFi.localIP().toString();
  if (apStarted) return WiFi.softAPIP().toString();
  return "";
}

const char* linkName() {
  if (ethUp) return "eth";
  if (wifiUp) return "wifi";
  if (apStarted) return "ap";
  return "none";
}

int wifiRssi() { return wifiUp ? WiFi.RSSI() : 0; }

}  // namespace net
