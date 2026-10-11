# furnace-monitor

Custom firmware for a KinCony CO16 (ESP32-S3) that watches a Trane TUD120R954H4 gas furnace with a White-Rodgers 50A51-495 two-stage control. It works out the furnace phase, flame, cycle counts, runtimes and faults, serves a live web page, and publishes everything to Home Assistant over MQTT with auto-discovery.

**Monitor only.** The firmware forces the CO16's 16 relays off at boot and never addresses them again (CI checks this). No MQTT topic or web endpoint can switch anything; the only command the board accepts is the "filter changed" button, which resets a counter. Every furnace signal reaches the board through isolation. This is not a safety device and does not replace a CO alarm.

The full build plan (sensors, wiring, safety notes) is in the project's [build plan](https://claude.ai/code/artifact/326b1ad6-307e-4024-a8b6-6d7a18347500).

## Inputs (defaults, changeable on the Settings Wiring tab)

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
| CO level (optional) | CO transmitter with a 0-10 V output, in ppm (default 25 ppm/V, alert at 35 ppm) | AI 4 (tick CO sensor on the General tab) |
| Room temperature (optional) | Temperature transmitter with a 0-10 V output (default 0 to 50 °C) | AI 5 (tick Room temperature sensor on the General tab) |
| Outdoor temperature (optional) | Temperature transmitter with a 0-10 V output, outdoor rated (default -40 to 60 °C) | AI 6 (tick Outdoor temperature sensor on the General tab) |
| Pressures (optional, up to 3, renamable) | Differential pressure transmitters with a 0-10 V output, set up by default as filter pressure drop (0 to 1 in. w.c.) and total external static pressure, TESP (0 to 2 in. w.c.), plus a spare (0 to 1 in. w.c.) | AI 7, 8, 9 (tick Pressure sensor 1-3 on the General tab) |
| Spare probe (optional, renamable) | PT100, 3-wire | PT100 4 |

Untick any sensor that isn't fitted on the Settings General tab. That saves its input as 0, which the firmware treats as not fitted, and greys out its settings on the other tabs. The CO sensor adds CO level and alerts to Home Assistant; it does not replace the CO alarms the house already needs.

The CO16 has four PT100 channels, and supply, return and flue use three of them, so the room sensor goes on an analog input instead. Both transmitters need a 0-10 V output; if the CO16 has a range jumper for its analog inputs, set those inputs to 0-10 V.

Outdoor temperature can also come from [Open-Meteo](https://open-meteo.com/), a free weather service that needs no account. Tick "Outdoor temperature from Open-Meteo" on the General tab and enter your latitude and longitude on the Network tab. The board fetches the current temperature every 15 minutes. When an outdoor sensor is also fitted, the sensor is used and Open-Meteo fills in if it fails. Both are off by default.

Pressure sensors only read air pressure through tubes to static-pressure tips in the ductwork. For filter pressure drop, put one tip in the return just before the filter (high port) and one just after it (low port). For TESP, put one tip in the supply plenum just above the furnace, before any cooling coil (high port), and one in the return between the filter and the furnace (low port). Nothing connects to the furnace's own pressure-switch hoses. Gas pressure is deliberately not offered, because measuring it permanently means adding a fitting to the gas valve, and that is a job for a gas fitter, not a monitor.

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

After the first flash, updates can go through the web page (Settings, Login and firmware tab) using `firmware.bin` and `littlefs.bin` from `.pio/build/co16/` or from the CI build artifacts.

## First start

1. Plug Ethernet into the CO16. It gets an address by DHCP and answers at `http://furnace-co16.local/`.
2. With no Ethernet (and no Wi-Fi configured) it opens a `furnace-setup` access point after a minute; browse to `http://192.168.4.1/`.
3. Open Settings (default login `admin` / `furnace`; change it on the Login and firmware tab), pick your time zone on the Network tab, set the MQTT broker on the Home Assistant tab, and save. The board restarts and appears in Home Assistant as **Furnace**.

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

`GET /api/state`, `GET`/`POST /api/layout` (saving needs login), `GET /api/history?hours=24` (up to 720), `GET /api/cycles`, `GET /api/events`, `GET /api/bench`, `POST /api/filter/reset` (login), `GET`/`POST /api/config` (login), `POST /update` (login), WebSocket `/ws` (live state every second).

## SD card and history

With a card in the CO16's slot (FAT32), the board writes one-minute averages to `/log/YYYY-MM-DD.csv`, burner cycles to `/cycles.csv` and events to `/events.csv`. On start it reads the last 30 days back, so the History tab (24 h, 7 days, 30 days), the Cycles tab and the event log carry on across restarts. Without a card everything works, but history starts again at each restart.

## Live tab layout

Press "Customize dashboard" under the Live tab to hide tiles, show hidden ones (such as the 6-hour temperature chart), and reorder them by dragging or with the arrow buttons. While customizing, the chart widget also lets you pick its time span and which lines and shading it shows: any temperature, the rise, the motor currents, CO, the pressures, and when the burner, high fire, blower or heat call was on. The History tab has the same line choices, remembered in each browser. Saving needs the settings login. The layout is stored on the board, so every browser sees the same layout, and saving it doesn't restart the board. "Reset to default" goes back to the standard layout.

## Bench tab

For first power-up and calibration: all 16 digital inputs, all 16 analog input voltages, each PT100 channel's resistance and fault bits, the control board LED's measured flash timing, and which on-board chips answered. Type a clamp meter reading into the CT helper and it gives the `AmpsPerVolt` value to save on the Settings Calibration tab.

## Screen

The built-in ST7789 shows the phase, temperatures, any alert, today's cycles, filter life and the board's address. The panel size is a first guess (`displayWidth`/`displayHeight` 240 x 240, `displayRotation`); adjust on the Settings Screen tab if the picture is offset or cut off.

## Layout

- `lib/furnace_core/`: furnace model and LED flash decoder, plain C++ with host unit tests in `test/`
- `tools/sim/`: simulator that serves the web page with made-up furnace data (`node tools/sim/server.mjs`)
- `src/co16_io.*`: drivers for the XL9555 inputs, ADS1115 ADCs, MAX31865 RTD reader and the relay all-off
- `src/monitor.*`: acquisition task, history (24 h of 5 s samples and 30 days of minute averages in PSRAM), burner cycles and event log
- `src/sdlog.*`: SD card logging and reading it back at boot
- `src/display.*`, `src/st7789.*`: the built-in screen and a small ST7789 driver
- `src/ha_mqtt.*`, `src/web.*`, `src/net.*`: MQTT with discovery, web server, Ethernet / Wi-Fi / NTP
- `src/weather.*`: outdoor temperature from Open-Meteo
- `data/index.html`: the dashboard

## Not done yet

- Checking on real hardware (the Bench tab is there for this): the PT100 mux channel order, the LED flash timings, the CT scaling and the screen size are first guesses until the board and sensors are in hand
