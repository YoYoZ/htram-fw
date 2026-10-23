"""Dump ESP32 flash over a flaky UART adapter.

The stub's READ_FLASH normally streams 4 KB SLIP packets with 64 in flight,
which a bad PL2303 clone can't keep up with. Here each request asks for small
packets with one in flight, and every request is MD5-checked by the stub, so a
saved chunk is known good. Progress lives in <out>.part; a rerun resumes.
"""
import hashlib, os, struct, sys, time
from esptool.cmds import detect_chip

PORT, OUT = sys.argv[1], sys.argv[2]
SIZE, CHUNK, PKT, BAUD = 0x400000, 0x4000, 0x100, 115200
part = OUT + ".part"


def connect():
    for attempt in range(30):
        try:
            esp = detect_chip(PORT, BAUD, "no_reset", connect_attempts=2)
            try:
                esp = esp.run_stub()
            except Exception:
                pass  # stub already running
            return esp
        except Exception as e:
            print(f"  connect failed ({e}), retry {attempt + 1}", flush=True)
            time.sleep(1)
    sys.exit("could not connect")


def read_chunk(esp, off, length):
    esp.check_command("read flash", esp.ESP_READ_FLASH,
                      struct.pack("<IIII", off, length, PKT, 1))
    data = b""
    while len(data) < length:
        p = esp.read()
        data += p
        if len(data) < length and len(p) != PKT:
            raise IOError(f"short packet 0x{len(p):x}")
        esp.write(struct.pack("<I", len(data)))
    digest = esp.read()
    if len(data) != length or digest != hashlib.md5(data).digest():
        raise IOError("md5 mismatch")
    return data


done = os.path.getsize(part) // CHUNK * CHUNK if os.path.exists(part) else 0
with open(part, "r+b" if done else "wb") as f:
    f.truncate(done)
    f.seek(done)
    esp = connect()
    off = done
    while off < SIZE:
        for tries in range(1, 11):
            try:
                data = read_chunk(esp, off, CHUNK)
                break
            except Exception as e:
                print(f"  0x{off:06x} try {tries}: {e}", flush=True)
                try:
                    esp._port.close()
                except Exception:
                    pass
                time.sleep(0.5)
                esp = connect()
        else:
            sys.exit(f"gave up at 0x{off:06x}")
        f.write(data)
        f.flush()
        off += CHUNK
        if off % 0x20000 == 0:
            print(f"0x{off:06x} / 0x{SIZE:06x}  {time.strftime('%H:%M:%S')}", flush=True)

os.replace(part, OUT)
print("DONE", OUT)
