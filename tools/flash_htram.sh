#!/bin/sh
# Waits for the HTRAM's ESP32 to come up in download mode (IO0 held low by the
# C3 bridge, then the monitor power-cycled by hand), then flashes htram-fw.
#
#   python tools/htram_reset.py COM19 boot
#   sh tools/flash_htram.sh COM19 [what]
#   python tools/htram_reset.py COM19 run
#
# `what` is one of:
#   (nothing)                   your own build in .pio/build/htram
#   htram-fw-X.Y.Z-full.bin     a release's merged image, written at 0x0 (first
#                               flash: also erases Wi-Fi settings and pairing)
#   htram-fw-X.Y.Z-parts/       an unzipped release's parts (keeps settings)
# HTRAM_BUILD_DIR also points at a build or parts directory.
set -e
PORT=${1:-COM19}
WHAT=${2:-${HTRAM_BUILD_DIR:-.pio/build/htram}}

if [ -f "$WHAT" ]; then
  set -- 0x0 "$WHAT"
else
  B=${WHAT%/}
  APP0="$B/boot_app0.bin"  # releases carry their own
  [ -f "$APP0" ] || APP0="${USERPROFILE:-$HOME}/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
  for f in "$B/bootloader.bin" "$B/partitions.bin" "$APP0" "$B/firmware.bin"; do
    [ -f "$f" ] || { echo "missing $f"; exit 1; }
  done
  set -- 0x1000 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" \
         0xe000 "$APP0" 0x10000 "$B/firmware.bin"
fi

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
  --before no_reset --after no_reset write_flash -z "$@"
echo "== flash done"
