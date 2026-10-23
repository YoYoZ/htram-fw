"""Build and check HTRAM BLE command frames.

Frame: 7b 41 00 <len> <cmd hi> <cmd lo> <payload...> <crc hi> <crc lo> 7d
  len = total frame length - 4, i.e. bytes after the length field (frames stay under 256)
  crc = CRC-16/BUYPASS (poly 0x8005, init 0, no reflection) over 7b..payload

Usage:  python htram_frame.py <cmd hex> [payload hex]
        python htram_frame.py check <frame hex>
"""
import sys


def crc16(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def build(cmd: bytes, payload: bytes = b"") -> bytes:
    body = bytearray(b"\x7b\x41\x00\x00" + cmd + payload)
    body[3] = (len(body) - 1) & 0xFF  # bytes after the length field, incl. crc and 7d
    c = crc16(bytes(body))
    return bytes(body) + bytes([c >> 8, c & 0xFF, 0x7D])


def check(frame: bytes) -> bool:
    return frame[0] == 0x7B and frame[-1] == 0x7D and crc16(frame[:-3]) == (frame[-3] << 8 | frame[-2])


if __name__ == "__main__":
    if sys.argv[1] == "check":
        f = bytes.fromhex(sys.argv[2])
        print("ok" if check(f) else "BAD", f.hex(" "))
    else:
        print(build(bytes.fromhex(sys.argv[1]), bytes.fromhex(sys.argv[2] if len(sys.argv) > 2 else "")).hex())
