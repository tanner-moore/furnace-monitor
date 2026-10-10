# furnace-monitor

Custom firmware for a KinCony CO16 (ESP32-S3) that watches a Trane TUD120R954H4 gas furnace with a White-Rodgers 50A51-495 two-stage control. It works out the furnace phase, flame, cycle counts, runtimes and faults, serves a live web page, and publishes everything to Home Assistant over MQTT with auto-discovery.

**Monitor only.** The firmware forces the CO16's 16 relays off at boot and never addresses them again (CI checks this). No MQTT topic or web endpoint can switch anything; the only command the board accepts is the "filter changed" button, which resets a counter. Every furnace signal reaches the board through isolation. This is not a safety device and does not replace a CO alarm.

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
| CO alarm (optional) | Relay contact of a CO alarm, e.g. System Sensor CO1224TR on 12/24 VDC | DI 3 (set `diCo` = 3) |

Inputs left at 0 in settings are treated as not fitted. For the CO alarm, set `coOnOpen` if its contact opens on alarm. It adds CO alerts to Home Assistant; it does not replace the CO alarms the house already needs.

## What it tracks

- **Phase, flame and faults** from the heat calls, gas valve, motor currents, temperatures and the control board LED. A failed ignition is caught even though the 50A51 opens the valve for a few seconds on each trial: a valve opening that never proves flame counts as a failed trial (not a burner cycle), and the second one in a call raises `ignition_failure`.
- **Per-cycle health**: time to ignition, settled temperature rise on low and high fire, peak flue temperature, and average inducer and blower current. Trends in these show a clogging filter or a tiring motor long before anything fails.
- **Totals** (cycles, failed trials, burner, high-fire and blower hours) that survive restarts.
- **Air filter**: blower hours since the last change, against `filterLifeHours` (default 500). Press "Filter changed" on the web page or in Home Assistant after replacing it.

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
| `furnace/co16/cycle` | no | JSON figures for each completed burner cycle |
| `furnace/co16/filter_reset` | (subscribed) | `PRESS` from the Home Assistant "Filter changed" button |
| `homeassistant/device/furnace_co16/config` | yes | Device discovery (Home Assistant 2024.11 or later) |

The board subscribes only to `homeassistant/status` (to resend discovery when Home Assistant restarts) and the filter reset topic.

## Web API

`GET /api/state`, `GET /api/history?hours=24` (up to 720), `GET /api/cycles`, `GET /api/events`, `GET /api/bench`, `POST /api/filter/reset` (login), `GET`/`POST /api/config` (login), `POST /update` (login), WebSocket `/ws` (live state every second).

## SD card and history

With a card in the CO16's slot (FAT32), the board writes one-minute averages to `/log/YYYY-MM-DD.csv`, burner cycles to `/cycles.csv` and events to `/events.csv`. On start it reads the last 30 days back, so the History tab (24 h, 7 days, 30 days), the Cycles tab and the event log carry on across restarts. Without a card everything works, but history starts again at each restart.

## Bench tab

For first power-up and calibration: all 16 digital inputs, all 16 analog input voltages, each PT100 channel's resistance and fault bits, the control board LED's measured flash timing, and which on-board chips answered. Type a clamp meter reading into the CT helper and it gives the `AmpsPerVolt` value to save in Settings.

## Screen

The built-in ST7789 shows the phase, temperatures, any alert, today's cycles, filter life and the board's address. The panel size is a first guess (`displayWidth`/`displayHeight` 240 x 240, `displayRotation`); adjust in Settings if the picture is offset or cut off.

## Layout

- `lib/furnace_core/`: furnace model and LED flash decoder, plain C++ with host unit tests in `test/`
- `src/co16_io.*`: drivers for the XL9555 inputs, ADS1115 ADCs, MAX31865 RTD reader and the relay all-off
- `src/monitor.*`: acquisition task, history (24 h of 5 s samples and 30 days of minute averages in PSRAM), burner cycles and event log
- `src/sdlog.*`: SD card logging and reading it back at boot
- `src/display.*`, `src/st7789.*`: the built-in screen and a small ST7789 driver
- `src/ha_mqtt.*`, `src/web.*`, `src/net.*`: MQTT with discovery, web server, Ethernet / Wi-Fi / NTP
- `data/index.html`: the dashboard

## Not done yet

- Checking on real hardware (the Bench tab is there for this): the PT100 mux channel order, the LED flash timings, the CT scaling and the screen size are first guesses until the board and sensors are in hand
