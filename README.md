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
 "age_seconds":2,"uptime_seconds":431}
```

- **Apple Home**, natively — CO2 (with a "CO2 detected" alert), temperature,
  humidity and battery. No Homebridge needed.
- **JSON API**, **Prometheus metrics** and a status page.
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
| `/api/data` | JSON reading, CORS-enabled |
| `/metrics` | Prometheus text format |
| `/` | status page, with the Apple Home setup code |
| `/setup` | change Wi-Fi credentials |

`co2` is `null` while the sensor warms up after power-on. `battery` is 0–4
bars, as on the device.

## Apple Home

In the Home app: **Add Accessory → More options**, pick *HTRAM CO2 Monitor*,
enter the setup code from the status page.

- **CO2 sensor** — ppm, peak, and *CO2 detected* above 1000 ppm (where the
  device turns yellow; clears below 900). Change it with
  `-DHTRAM_CO2_ALERT_PPM=1200`.
- **Temperature**, **humidity**, **battery** (level, charging, low).
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
| `HTRAM_CO2_ALERT_PPM` | 1000 | "CO2 detected" threshold |
| `HTRAM_OTA_PASS` | `htram-ota` | OTA password — **change it** |
| `HTRAM_HOMESPAN_LOG` | 0 | HomeSpan log level on UART0 (1–2 to debug HomeKit) |
| `HTRAM_RTS_PIN` | -1 | enable RTS flow control on the AT link if a board needs it |

## Repository

```
src/                    firmware (PlatformIO, Arduino-ESP32 2.0, HomeSpan 1.9)
tools/c3-uart-bridge/   ESP32-C3 USB-serial bridge, IO0 control, AT sniffer, BLE client
tools/*.py              frame builder, flash dumpers, boot-mode control, mDNS test
docs/                   hardware, protocol, flashing, networking notes
```

The firmware uses 95% of the default 1.3 MB app partition. A larger partition
table is on the to-do list.

## Disclaimer

Not affiliated with or endorsed by Honeywell. "Honeywell" and "HTRAM" are
trademarks of their owners. You are modifying your own hardware at your own
risk; keep your backup. The monitor estimates transmission risk from CO2 — it
does not detect viruses.

## License

MIT.
