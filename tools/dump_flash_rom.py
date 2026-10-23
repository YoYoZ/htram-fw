"""Dump ESP32 flash through the ROM loader only (no stub).

ROM READ_FLASH_SLOW moves 64 bytes per request/response, so there is no long
stream to lose sync on. Each 4 KB chunk is checked against the ROM's own
SPI_FLASH_MD5 of that range. Progress lives in <out>.part; a rerun resumes.
"""
import hashlib, os, sys, time
from esptool.cmds import detect_chip

PORT, OUT = sys.argv[1], sys.argv[2]
SIZE, CHUNK, BAUD = 0x400000, 0x1000, 115200
part = OUT + ".part"


def connect(wait_forever=False):
    attempt = 0
    while wait_forever or attempt < 30:
        attempt += 1
        try:
            esp = detect_chip(PORT, BAUD, "no_reset", connect_attempts=0 if wait_forever else 3)
            if esp.IS_STUB:
                # stale stub from an earlier run; wait for the user to power-cycle
                esp._port.close()
                time.sleep(2)
                continue
            esp.flash_spi_attach(0)
            return esp
        except Exception as e:
            print(f"  connect failed ({e}), retry {attempt + 1}", flush=True)
            time.sleep(1)
    sys.exit("could not connect")


done = os.path.getsize(part) // CHUNK * CHUNK if os.path.exists(part) else 0
with open(part, "r+b" if done else "wb") as f:
    f.truncate(done)
    f.seek(done)
    print("waiting for ROM download mode...", flush=True)
    esp = connect(wait_forever=True)
    print(f"connected, starting at 0x{done:06x} {time.strftime('%H:%M:%S')}", flush=True)
    off, errors = done, 0
    while off < SIZE:
        for tries in range(1, 11):
            try:
                data = esp.read_flash_slow(off, CHUNK, None)
                if hashlib.md5(data).hexdigest() != esp.flash_md5sum(off, CHUNK):
                    raise IOError("md5 mismatch")
                break
            except Exception as e:
                errors += 1
                print(f"  0x{off:06x} try {tries}: {e}", flush=True)
                try:
                    esp._port.reset_input_buffer()
                    esp.sync()
                except Exception:
                    try:
                        esp._port.close()
                    except Exception:
                        pass
                    esp = connect()
        else:
            sys.exit(f"gave up at 0x{off:06x}")
        f.write(data)
        f.flush()
        off += CHUNK
        if off % 0x40000 == 0:
            print(f"0x{off:06x} / 0x{SIZE:06x}  errors so far {errors}  {time.strftime('%H:%M:%S')}", flush=True)

os.replace(part, OUT)
print("DONE", OUT, "errors:", errors)
