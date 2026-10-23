# htram-fw

Replacement Wi-Fi firmware for the **Honeywell HTRAM-V1-W** "Transmission Risk
Air Monitor" — the CO2 monitor Honeywell shipped in 2021 and then abandoned.

Its phone app is gone and its cloud no longer enrolls devices, so a perfectly
good CO2, temperature and humidity sensor is stuck showing numbers on its own
screen. This firmware replaces only the ESP32 radio module inside it and puts
the readings on your network:

```
$ curl http://htram.local/api/data
{"online":true,"valid":true,"warming_up":false,"co2":1200,"temperature":21,
 "humidity":57,"battery":4,"battery_percent":100,"charging":false,
 "age_seconds":2,"level":"yellow","activity":"low","yellow_ppm":800,
 "red_ppm":1100,"uptime_seconds":431}
```

- **Apple Home**, natively — CO2 (with a "CO2 detected" alert that matches the
  device's yellow light), temperature, humidity, battery, and switches for the
  alarm sound and display. No Homebridge needed.
- **Everything the retired app could do:** the "transmission risk" level,
  activity presets and custom thresholds, display auto-off, alarm sound, °C/°F,
  and the device's **built-in history log** (one record per minute, ~90 days).
- **Clock sync** from network time — the device has no clock battery and loses
  the time on every power loss.
- **JSON API**, **CSV history export**, **Prometheus metrics**, a status page
  with a 24-hour chart, and a settings page.
- **Over-the-air updates** after the first flash.
- No cloud, no account, no MQTT.

The display, alarm, LEDs and buttons keep working exactly as before: they run
on the device's main controller, which is not touched.

## How it works

The monitor's brain is a GD32F150 microcontroller. It drives the ESP32 as a
plain Espressif ESP-AT modem over a serial link, and it only reports a reading
when a Bluetooth client asks for one — which is what the retired app did every
five seconds.

htram-fw takes the ESP-AT module's place on that link. It answers the GD32's
setup commands, presents a client on the Bluetooth service the GD32 creates,
asks for a reading on a timer, and serves the result over Wi-Fi.

Details: [hardware](docs/HARDWARE.md) · [protocol](docs/PROTOCOL.md) ·
[flashing](docs/FLASHING.md) · [Apple Home across subnets](docs/NETWORKING.md)

## Quick start

You need a 3.3 V USB-serial adapter (an ESP32-C3 SuperMini with
[`tools/c3-uart-bridge`](tools/c3-uart-bridge) works great), a fine soldering
iron, and [PlatformIO](https://platformio.org/).

1. **Solder** to the ESP32 module's edge pads: TXD0 (pin 35), RXD0 (pin 34),
   GND, and IO0 (pin 25). See [HARDWARE.md](docs/HARDWARE.md#esp32-wroom-32e-pins-that-matter).
2. **Back up** the stock firmware — [FLASHING.md](docs/FLASHING.md#back-up-first).
3. **Flash**: `pio run -e htram`, then [flash it](docs/FLASHING.md#first-flash).
4. **Power on** the monitor (hold its button two seconds). It opens a Wi-Fi
   network **HTRAM-Setup** (password `htram-setup`); join it, open
   <http://192.168.4.1/setup> and enter your network.
5. Open <http://htram.local>. The status page shows the readings and the Apple
   Home setup code.

The ESP32 on these units has no flash encryption or secure boot, and your
backup restores the original firmware at any time.

## Endpoints

| Path | |
| --- | --- |
| `GET /api/data` | current reading and risk level, CORS-enabled |
| `GET /api/settings` | the device's settings; `POST` to change them |
| `GET /api/history?minutes=1440` | the device's own log, JSON; add `&format=csv` |
| `GET /metrics` | Prometheus text format |
| `/` | status page with a 24 h chart and the Apple Home setup code |
| `/settings` | device settings form |
| `/setup` | change Wi-Fi credentials |
| `GET /api/raw?cmd=2021&payload=01` | send any protocol frame (developer aid) |

`co2` is `null` while the sensor warms up after power-on. `battery` is 0–4
bars, as on the device. `level` is the device's own traffic light: `green`,
`yellow` or `red` under the thresholds of the selected activity level.

### The "transmission risk"

There is no hidden calculation: the monitor compares CO2 against thresholds
that depend on how active the room is. More people breathing harder means CO2
signals shared air sooner, so the alarm comes earlier.

| Activity | Examples | Yellow | Red |
| --- | --- | --- | --- |
| `low` | classrooms, libraries, cinemas | 800 ppm | 1100 ppm |
| `medium` | restaurants, offices, clinics | 700 ppm | 1000 ppm |
| `high` | gyms, choirs, recreation centres | 500 ppm | 800 ppm |
| `custom` | your own | 500–1100 | ≥ yellow + 100, up to 2000 |

### Changing settings

```bash
curl -X POST http://htram.local/api/settings -d activity=medium
curl -X POST http://htram.local/api/settings -d "yellow=900&red=1300"   # custom
curl -X POST http://htram.local/api/settings -d screen_off=120          # 0 = always on
curl -X POST http://htram.local/api/settings -d alarm_sound=0
curl -X POST http://htram.local/api/settings -d unit=F
```

### History

```bash
curl "http://htram.local/api/history?minutes=1440&format=csv" > today.csv
```

One record per minute, read straight from the device's flash (about 5 s per
day, up to 7 days per request). The device writes records in batches of 16, so
the newest can be up to 16 minutes old — `/api/data` has the live value. Each
record has a `time_trusted` flag: timestamps from before the clock was synced
after a power loss start at 2021-01-01 and are only right relative to each
other.

## Apple Home

In the Home app: **Add Accessory → More options**, pick *HTRAM CO2 Monitor*,
enter the setup code from the status page.

- **CO2 sensor** — ppm, peak, and *CO2 detected* from the device's yellow
  threshold for its activity level (clears 100 ppm below). Force a fixed level
  with `-DHTRAM_CO2_ALERT_PPM=1200`.
- **Temperature**, **humidity**, **battery** (level, charging, low).
- **Alarm Sound** and **Display Auto-Off** switches, kept in step with the
  device and the web settings.
- A **fault** flag when the main board stops answering or a reading is more
  than a minute old.

Each unit gets its own setup code, derived from its MAC address. HomeKit runs
on TCP 1201; port 80 stays with the JSON API.

**"No Response" right after pairing** almost always means your home hub (Apple
TV, HomePod) is on a different subnet and cannot see the accessory's Bonjour
announcements — see [NETWORKING.md](docs/NETWORKING.md).

## Home Assistant

```yaml
sensor:
  - platform: rest
    name: HTRAM CO2
    resource: http://htram.local/api/data
    scan_interval: 10
    value_template: "{{ value_json.co2 }}"
    unit_of_measurement: ppm
    device_class: carbon_dioxide
    json_attributes: [temperature, humidity, battery_percent]
```

## Build options

| Flag | Default | |
| --- | --- | --- |
| `HTRAM_CO2_ALERT_PPM` | 0 | fixed "CO2 detected" level; 0 = follow the device's yellow threshold |
| `HTRAM_OTA_PASS` | `htram-ota` | OTA password — **change it** |
| `HTRAM_RAW_API` | 1 | `/api/raw` protocol access; set 0 to disable |
| `HTRAM_HOMESPAN_LOG` | 0 | HomeSpan log level on UART0 (1–2 to debug HomeKit) |
| `HTRAM_RTS_PIN` | -1 | enable RTS flow control on the AT link if a board needs it |

## Repository

```
src/                    firmware (PlatformIO, Arduino-ESP32 2.0, HomeSpan 1.9)
  gd32_link.*           ESP-AT stand-in on the serial link to the main board
  device.*              settings, clock, history over the Honeywell protocol
  homekit.*             Apple Home accessory
  main.cpp              Wi-Fi setup, web API and pages
tools/c3-uart-bridge/   ESP32-C3 USB-serial bridge, IO0 control, AT sniffer, BLE client
tools/*.py              frame builder, flash dumpers, boot-mode control, mDNS and history probes
docs/                   hardware, protocol, flashing, networking notes
```

The firmware builds for the `min_spiffs` partition table (two 1.875 MB app
slots). Units first flashed with the default table need that changed once over
UART — see [FLASHING.md](docs/FLASHING.md#partition-table).

## Disclaimer

Not affiliated with or endorsed by Honeywell. "Honeywell" and "HTRAM" are
trademarks of their owners. You are modifying your own hardware at your own
risk; keep your backup. The monitor estimates transmission risk from CO2 — it
does not detect viruses.

## License

MIT.
