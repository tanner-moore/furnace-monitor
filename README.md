# furnace-monitor

Custom firmware for a KinCony CO16 (ESP32-S3) that watches a Trane TUD120R954H4 gas furnace with a White-Rodgers 50A51-495 two-stage control. It works out the furnace phase, flame, cycle counts, runtimes and faults, serves a live web page, and publishes everything to Home Assistant over MQTT with auto-discovery.

**Monitor only.** The firmware forces the CO16's 16 relays off at boot and never addresses them again (CI checks this). No MQTT topic or web endpoint can switch anything. Every furnace signal reaches the board through isolation. This is not a safety device and does not replace a CO alarm.

The full build plan (sensors, wiring, safety notes) is in the project's [build plan](https://claude.ai/code/artifact/326b1ad6-307e-4024-a8b6-6d7a18347500).

## Inputs (defaults, changeable on the Settings page)

| Signal | Pickup | CO16 input |
| --- | --- | --- |
| Heat call stage 1 (W1) | 24 VAC interface relay, W1 to C | DI 1 |
| Heat call stage 2 (W2) | 24 VAC interface relay, W2 to C | DI 2 |
| Fan call (G) | 24 VAC interface relay, G to C | DI 4 |
| Gas valve low fire (MVL) | 24 VAC interface relay, MVL to MV COM | DI 5 |
| Gas valve high fire (MVH) | 24 VAC interface relay, MVH to MV COM | DI 6 |
| Inducer current | Split-core CT, 0-10 V out (default 0.5 A/V) | AI 1 |
| Blower current | Split-core CT, 0-10 V out (default 2 A/V) | AI 2 |
| Control board LED | Light-to-voltage sensor over the 50A51 LED | AI 3 |
| Supply / return / flue air | PT100, 3-wire | PT100 1 / 2 / 3 |

DI 3 is spare (no cooling). Inputs left at 0 in settings are treated as not fitted.

## Build and flash

```sh
pip install platformio "click<8.2"   # esptool in this platform needs click below 8.2
pio test -e native            # furnace model and LED decoder unit tests
pio run -e co16 -t upload     # firmware over USB-C (hold the download button if needed)
pio run -e co16 -t uploadfs   # web page
```

After the first flash, updates can go through the web page (Settings, Firmware update) using `firmware.bin` and `littlefs.bin` from `.pio/build/co16/` or from the CI build artifacts.

## First start

1. Plug Ethernet into the CO16. It gets an address by DHCP and answers at `http://furnace-co16.local/`.
2. With no Ethernet (and no Wi-Fi configured) it opens a `furnace-setup` access point after a minute; browse to `http://192.168.4.1/`.
3. Open Settings (default login `admin` / `furnace`, change it), set the MQTT broker, and save. The board restarts and appears in Home Assistant as **Furnace**.

## MQTT

| Topic | Retained | Payload |
| --- | --- | --- |
| `furnace/co16/availability` | yes | `online` / `offline` (last will) |
| `furnace/co16/state` | no | JSON with every value, on change and every 30 s |
| `furnace/co16/event` | no | JSON `{seq, time, text}` for phase changes and alerts |
| `homeassistant/device/furnace_co16/config` | yes | Device discovery (Home Assistant 2024.11 or later) |

The board subscribes only to `homeassistant/status` so it can resend discovery when Home Assistant restarts.

## Web API

`GET /api/state`, `GET /api/history?hours=24`, `GET /api/events`, `GET`/`POST /api/config` (login), `POST /update` (login), WebSocket `/ws` (live state every second).

## Layout

- `lib/furnace_core/`: furnace model and LED flash decoder, plain C++ with host unit tests in `test/`
- `src/co16_io.*`: drivers for the XL9555 inputs, ADS1115 ADCs, MAX31865 RTD reader and the relay all-off
- `src/monitor.*`: acquisition task, history (24 h in PSRAM) and event log
- `src/ha_mqtt.*`, `src/web.*`, `src/net.*`: MQTT with discovery, web server, Ethernet / Wi-Fi / NTP
- `data/index.html`: the dashboard

## Not done yet

- Display output on the built-in ST7789 screen
- SD card CSV logging
- Checking on real hardware: the PT100 mux channel order, the LED flash timings and the CT scaling are first guesses until the sensors are fitted
