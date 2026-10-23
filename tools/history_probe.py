"""Explores the HTRAM's on-device history log through htram-fw's /api/raw.

    python history_probe.py [host] [address-hex ...]

With no addresses: reads the start pointer, then binary-searches for the end of
the log (the last packet that still holds records) and prints the first and
last records. With addresses: dumps the packet at each one.
"""
import datetime, json, sys, urllib.request

host = sys.argv[1] if len(sys.argv) > 1 else "htram.local"


def packet(addr):
    url = f"http://{host}/api/raw?cmd=2093&payload=01{addr:08x}"
    with urllib.request.urlopen(url, timeout=6) as r:
        reply = json.load(r)["reply"]
    f = bytes.fromhex(reply)
    nxt = int.from_bytes(f[7:11], "big")
    data = f[11:-3]
    recs = []
    for i in range(0, len(data) - 7, 8):
        r = data[i:i + 8]
        if r[0:4] == b"\xff\xff\xff\xff":  # erased flash past the end of the log
            continue
        t = r[6] - 256 if r[6] > 128 else r[6]
        recs.append((int.from_bytes(r[0:4], "big"), int.from_bytes(r[4:6], "big"), t, r[7]))
    return nxt, recs


def show(rec):
    ts, co2, t, rh = rec
    when = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    return f"{when}  co2 {co2:5d}  {t:3d} C  {rh:3d} %"


if len(sys.argv) > 2:
    for a in sys.argv[2:]:
        nxt, recs = packet(int(a, 16))
        print(f"0x{int(a, 16):08x} -> next 0x{nxt:08x}, {len(recs)} records")
        for r in recs:
            print("   ", show(r))
    sys.exit()

# The log is a ring buffer: the start pointer is the oldest packet, records run
# up to the end of the ring, wrap to its beginning, and stop shortly before the
# start pointer (the device keeps the sector ahead of its write position erased).
RING_START, RING_END = 0x00200000, 0x00300000


def last_with_records(lo, hi):
    """Last 128-byte packet in [lo, hi) holding records, assuming they are contiguous from lo."""
    while hi - lo > 0x80:
        mid = (lo + (hi - lo) // 2) & ~0x7F
        if packet(mid)[1]:
            lo = mid
        else:
            hi = mid
    return lo


start, _ = packet(0xFFFFFFFF)
print(f"start pointer 0x{start:08x}, ring 0x{RING_START:08x}-0x{RING_END:08x}")
if packet(RING_START)[1] and RING_START < start:
    end = last_with_records(RING_START, start)
    used = (RING_END - start + end - RING_START) // 0x80 + 1
else:
    end = last_with_records(start, RING_END)
    used = (end - start) // 0x80 + 1
first = packet(start)[1][0]
last = packet(end)[1][-1]
print(f"newest packet 0x{end:08x}; {used} packets ~ {used * 16} records")
print("oldest:", show(first))
print("newest:", show(last))
