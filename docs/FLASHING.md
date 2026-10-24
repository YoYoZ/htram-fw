# Flashing

You need UART0 (module pins 34/35), GND, and a way to hold IO0 (pin 25) low
while the module powers up. See [HARDWARE.md](HARDWARE.md) for the pinout.

## USB-to-serial adapter

- **CP2102 or CH340, 3.3 V logic:** works as-is.
- **PL2303 clones:** avoid. Ours handled short commands but corrupted long
  transfers — every 4 KB block arrived a byte short, at any baud rate.
- **An ESP32-C3 SuperMini** makes an excellent adapter with the firmware in
  [`tools/c3-uart-bridge`](../tools/c3-uart-bridge) — see below.

Never feed 3.3 V from the adapter; power the monitor from its own battery or
USB.

## Back up first

```bash
python tools/htram_reset.py COM19 boot      # only with the C3 bridge
# power-cycle the monitor (hold its button 2 s)
esptool --chip esp32 --port COM19 --before no_reset --after no_reset read-flash 0 0x400000 htram-stock-backup.bin
```

If your adapter is flaky, `tools/dump_flash.py` reads in small MD5-checked
chunks and resumes where it stopped, and `tools/dump_flash_rom.py` uses only
the ROM loader (slow, but survives almost anything).

Restore:

```bash
esptool --chip esp32 --port COM19 --before no_reset --after no_reset write-flash 0 htram-stock-backup.bin
```

Do not run `espefuse burn-*`. Fuses are permanent.

## Prebuilt images

Every [release](https://github.com/YoYoZ/htram-fw/releases) has:

| File | Use |
| --- | --- |
| `htram-fw-X.Y.Z-full.bin` | **First flash** of a stock unit, one image at `0x0`. Erases Wi-Fi settings and Apple Home pairing, along with stock ESP-AT's leftovers. |
| `htram-fw-X.Y.Z-ota.bin` | **Updates over Wi-Fi**; keeps everything. |
| `htram-fw-X.Y.Z-parts.zip` | Updates over UART that keep settings. |
| `c3-uart-bridge-b115200.bin` | The [C3 bridge](#the-esp32-c3-supermini-bridge), one image at `0x0`. `_swap`: TX and RX swapped. |
| `SHA256SUMS` | Checksums. |

You only need esptool (`pip install esptool`, which brings pyserial).

```bash
python tools/htram_reset.py COM19 boot                  # only with the C3 bridge
sh tools/flash_htram.sh COM19 htram-fw-X.Y.Z-full.bin   # waits for download mode, then writes
python tools/htram_reset.py COM19 run
# power-cycle the monitor
```

With any other adapter, hold IO0 low, power-cycle the monitor and write the
image yourself:

```bash
esptool --chip esp32 --port COM19 --before no_reset --after no_reset write-flash 0x0 htram-fw-X.Y.Z-full.bin
```

The bridge itself: hold the C3's BOOT button while plugging it in, then

```bash
esptool --chip esp32c3 --port COM20 write-flash 0x0 c3-uart-bridge-b115200.bin
```

## First flash, from source

```bash
pio run -e htram
python tools/htram_reset.py COM19 boot
sh tools/flash_htram.sh COM19         # waits for download mode, then writes
python tools/htram_reset.py COM19 run
# power-cycle the monitor
```

This writes the parts and leaves `nvs` alone, so it also works as a UART
update that keeps settings. An unzipped release does the same:
`sh tools/flash_htram.sh COM19 htram-fw-X.Y.Z-parts/`.

**On Windows, keep the project in a short path** such as `C:\src\htram-fw`.
Windows limits paths to 260 characters, and inside a deep folder the build
fails with "No such file or directory" for HomeSpan headers that plainly
exist. If moving it is not an option, build elsewhere and point the flash
script there:

```bash
PLATFORMIO_BUILD_DIR=C:/pio/build PLATFORMIO_LIBDEPS_DIR=C:/pio/libdeps pio run -e htram
HTRAM_BUILD_DIR=C:/pio/build/htram sh tools/flash_htram.sh COM19
```

If a flash is interrupted after the bootloader region was erased, the module
will not boot — but download mode lives in mask ROM, so just flash again.

## Partition table

htram-fw uses the `min_spiffs` table: two 1.875 MB app slots, so OTA updates
have room to grow. `nvs` (Wi-Fi credentials, HomeKit pairing) and `otadata`
sit at the same addresses as in the default table, so switching keeps your
settings and pairing.

The table can only be changed over UART. A unit first flashed with the default
table keeps working and keeps accepting OTA updates while the image fits its
1.25 MB slots; to switch, write the parts over UART once ([from source](#first-flash-from-source),
or a release's `-parts`). That keeps settings and pairing.

## Later: over Wi-Fi

```bash
HTRAM_OTA_PASS=<password> pio run -e htram_ota -t upload
```

Or a release's image, with
[`espota.py`](https://github.com/espressif/arduino-esp32/blob/2.0.17/tools/espota.py)
from Arduino-ESP32:

```bash
python espota.py -i htram.local -a <password> -f htram-fw-X.Y.Z-ota.bin
```

The password is the admin password if you set one, otherwise the device's own,
shown on its `/setup` page.

**There is no automatic rollback.** The Arduino bootloader boots whatever was
written last, so an update that crashes at startup needs the UART wires to
recover. Test risky changes on the bench, or keep the wires on until you trust
a build.

## The ESP32-C3 SuperMini bridge

The C3's USB is built into the chip, so it cannot simply be held in reset the
way a dev board's USB chip can. The bridge firmware turns it into a USB-to-UART
adapter.

| C3 pin | HTRAM module | Env |
| --- | --- | --- |
| 20 | pin 35 (TXD0) | `b115200` (RX on 20, TX on 21) |
| 21 | pin 34 (RXD0) | |
| GND | GND | |
| 5 | pin 25 (IO0) | boot-mode control, optional |
| 6 | pin 3 (EN) | reset, optional |

If you swap TX and RX, flash env `b115200_swap` instead of resoldering.
`env:probe` listens on both pins and labels which one carries data.

Boot-mode control:

```bash
python tools/htram_reset.py COM19 boot     # IO0 held low; power-cycle to enter download mode
python tools/htram_reset.py COM19 run      # IO0 released
python tools/htram_reset.py COM19 status
```

Lessons that cost us an evening each:

- **IO0 needs maximum drive strength.** At default strength the C3 lost to
  whatever holds IO0 high on this board; the bridge uses `GPIO_DRIVE_CAP_3`.
- **Opening the C3's USB port can reset the C3.** Its built-in USB serial
  resets on certain DTR/RTS states, so the IO0 setting is stored in flash and
  reapplied at boot.
- **The UART transmit buffer must hold a whole esptool block** (16 KB). With a
  small buffer the bridge stalls, the USB side overflows and silently drops
  bytes. The bridge uses 40 KB.
- **Use the IDF UART driver, not Arduino's HardwareSerial**, on the C3; the
  latter hit an interrupt-watchdog panic when the target's TX line dropped.
- **Use Arduino's HWCDC for the USB side.** The IDF 4.4 `usb_serial_jtag`
  driver holds short packets indefinitely.

## Sniffing the AT link

Wire GPIO16 (module pin 27) to C3 pin 3 and GPIO17 (pin 28) to C3 pin 4, flash
`env:sniff_at`, and log with `tools/serial_log.py`. Each direction is tagged
`[GD32>ESP]` / `[ESP>GD32]` with a timestamp. `env:sniff_ble` adds a BLE client
that connects to the stock firmware like the phone app did.
