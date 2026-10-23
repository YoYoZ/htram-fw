#!/bin/sh
# Waits for the HTRAM's ESP32 to come up in download mode (IO0 held low by the
# C3 bridge, then the monitor power-cycled by hand), then flashes htram-fw.
# Run from the repository root after `pio run -e htram`:
#   python tools/htram_reset.py COM19 boot
#   sh tools/flash_htram.sh COM19
#   python tools/htram_reset.py COM19 run
set -e
PORT=${1:-COM19}
B=.pio/build/htram
APP0="$USERPROFILE/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

echo "== waiting for download mode on $PORT (power-cycle the monitor now)"
python -I -u - "$PORT" <<'EOF'
import serial, sys, time
s = serial.Serial(); s.port, s.baudrate, s.timeout = sys.argv[1], 115200, 0.5
s.dtr = s.rts = False; s.open()
buf, end = b"", time.time() + 300
while time.time() < end:
    buf += s.read(4096)
    if b"waiting for download" in buf:
        print("download mode confirmed"); sys.exit(0)
    if b"SPI_FAST_FLASH_BOOT" in buf:
        print("booted normally - IO0 was not low"); sys.exit(1)
print("timed out"); sys.exit(1)
EOF

echo "== flashing"
python -m esptool --chip esp32 --port "$PORT" --baud 115200 \
  --before no_reset --after no_reset write_flash -z \
  0x1000 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" \
  0xe000 "$APP0" 0x10000 "$B/firmware.bin"
echo "== flash done"
