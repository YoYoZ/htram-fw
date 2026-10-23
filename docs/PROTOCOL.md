# Protocol

Two layers: the **AT link** between the GD32 and the ESP32, and the
**Honeywell frames** the GD32 exchanges with a Bluetooth client through it.
Everything here was observed on a live device; command names come from the
manufacturer's Android app (`com.honeywell.sps.airmonitor` 2.9.1), studied for
interoperability. No app code is included in this repository.

## AT link (GD32 ↔ ESP32)

UART on GPIO16 (GD32 → ESP32) and GPIO17 (ESP32 → GD32), 115200 8N1. The stock
ESP-AT build had RTS flow control on GPIO14; nothing observed so far needs it.

### Boot

As soon as the ESP32 prints `ready`, the GD32 configures it as a BLE
peripheral and starts advertising. Wi-Fi is switched off.

```
ESP32  ready
GD32   ATE0                         (ESP32 echoes it, then OK)
GD32   AT+CWMODE=0                  Wi-Fi off
GD32   AT+BLEINIT=2                 BLE server
GD32   AT+BLEGATTSSRVCRE            create services from the GATT table
GD32   AT+BLEGATTSSRVSTART
GD32   AT+BLEADVDATA="0201061309485452414d2d..."
GD32   AT+BLENAME="HTRAM-RM12xxxxxxxx"     the unit's serial number
GD32   AT+BLESECPARAM=1,0,16,3,3
GD32   AT+BLEADVSTART
```

Every command is answered with `\r\nOK\r\n`.

**The GD32 runs this sequence only when it powers the ESP32 up.** A `ready`
from a module that restarted on its own (Wi-Fi setup, OTA update) is ignored.

### A client connecting

```
ESP32  +BLECONN:0,"aa:bb:cc:dd:ee:ff"
GD32   AT+BLECFGMTU?        ->  +BLECFGMTU:0,23   OK
GD32   AT+BLEENC=0,3        ->  OK
ESP32  +BLESECKEYTYPE:0,16 / 32 / 1 / 2
ESP32  +BLEAUTHCMPL:0,0
ESP32  +BLECFGMTU:0,247
ESP32  +WRITE:0,2,1,1,2,<01 00>        client enables notifications on Read-Data
```

### A request and its reply

```
ESP32  +WRITE:0,2,2,,11,<11 bytes>     client wrote a frame to Write-Data
GD32   AT+BLEGATTSNTFY=0,2,1,16        "I am about to notify 16 bytes"
ESP32  >                               go ahead
GD32   <16 bytes>                      the reply frame
ESP32  OK
```

The GD32 answers within ~20–50 ms. It never sends anything unprompted.

## Bluetooth service

From the ESP-AT GATT table:

| Service / characteristic | UUID | Properties |
| --- | --- | --- |
| Device Information | `180A` | Manufacturer "Honeywell", model "IAQ CO2", FW "V1.00" |
| **Honeywell service** | `FC247940-6E08-11E4-80FC-0002A5D5C51B` | |
| Read-Data (replies) | `F833D6C0-6E0B-11E4-9136-0002A5D5C51B` | read, notify |
| Write-Data (requests) | `3D115840-6E0B-11E4-B24F-0002A5D5C51B` | write, write without response |
| Binding-Status | `90178A02-5D4A-11E6-8B77-86F30CA893D3` | read, write |
| Battery | `180F` | |

No characteristic requires encryption. Reading Read-Data directly returns
uninitialised memory; data only arrives as notifications.

## Frames

```
7b 41 00 LL CH CL <payload...> RH RL 7d
```

| Bytes | Meaning |
| --- | --- |
| `7b` | start |
| `41 00` | protocol version |
| `LL` | bytes after this one, including CRC and end marker (= frame length − 4) |
| `CH CL` | command; replies set bit 8 (`40 44` → `41 44`) |
| `RH RL` | CRC-16/BUYPASS (poly `0x8005`, init `0`, no reflection) over `7b`…payload, big-endian |
| `7d` | end |

[`tools/htram_frame.py`](../tools/htram_frame.py) builds and checks frames.

### Reading (verified)

Request, always the same — the app sends it every 5 s:

```
7b 41 00 07 40 44 02 00 fc 3e 7d
```

Reply:

```
7b 41 00 0c 41 44 02 04 b0 15 39 04 00 dd ab 7d
                        └─┬─┘ │  │  │  └─ charging (0/1)
                          │   │  │  └──── battery, 0-4 bars
                          │   │  └─────── relative humidity, %
                          │   └────────── temperature, °C, signed
                          └────────────── CO2 ppm, big-endian; 65534 = warming up
```

Offsets as the app parses them: bytes 7–8, 9, 10, 11, 12.

### Mode switch (verified)

| Frame | Effect |
| --- | --- |
| `7b 41 00 07 74 58 01 01 e7 81 7d` | Wi-Fi mode — GD32 replies `7558 01`, sends `AT+CWMODE=0`, then waits for credentials |
| `7b 41 00 07 74 58 01 00 67 84 7d` | back to Bluetooth mode |
| `7b 41 00 06 74 00 02 fa 6b 7d` | network status; reply byte 7: `0` idle, `2` connecting, `3` connected, `4` failed |

### Seen in the app, not tested

| Command | Purpose |
| --- | --- |
| `7460` | Wi-Fi credentials: 22 zero bytes, password length + password padded to 64, SSID padded to 33, 33 zero bytes |
| `20b0` | cloud MQTT URL, AES key and IV (two variants, chosen by device SKU) |
| `4143` | alarm / alert thresholds |
| `2120`, `2121`, `2123`, `216e` | SKU, firmware version and related queries |
| `2193` | history log |

In Wi-Fi mode the stock firmware publishes readings to whatever MQTT broker
the app configured, encrypted with the key it was given — the cloud address is
not built into the device.
