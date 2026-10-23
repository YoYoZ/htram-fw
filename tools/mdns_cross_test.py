"""Checks whether mDNS crosses between sites.

Sends Bonjour queries on the local LAN for services Apple devices and
HomeKit accessories announce, and lists every responder by IP. With an mDNS
repeater working across the tunnel, devices from the other site's subnet show
up here; without one, only local devices do.

    python mdns_cross_test.py [rounds]
"""
import socket, struct, sys, time

SERVICES = ["_hap._tcp.local", "_airplay._tcp.local", "_companion-link._tcp.local",
            "_homekit._tcp.local", "_raop._tcp.local"]
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3


def query(names):
    qd = b""
    for n in names:
        qd += b"".join(bytes([len(p)]) + p.encode() for p in n.split(".")) + b"\0"
        qd += struct.pack(">HH", 12, 1)  # PTR, IN
    return struct.pack(">HHHHHH", 0, 0, len(names), 0, 0, 0) + qd


def labels(data):
    """Pull readable service/instance names out of a response, loosely."""
    out, cur = [], b""
    for b in data:
        if 32 <= b < 127:
            cur += bytes([b])
        else:
            if len(cur) >= 4:
                out.append(cur.decode())
            cur = b""
    return [x for x in out if not x.startswith("_") or x in ("_hap", "_airplay", "_companion-link", "_homekit", "_raop")]


me = socket.gethostbyname(socket.gethostname())
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 5353))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
             socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0"))
s.settimeout(0.3)

seen = {}
for r in range(ROUNDS):
    s.sendto(query(SERVICES), ("224.0.0.251", 5353))
    end = time.time() + 2.5
    while time.time() < end:
        try:
            data, (ip, _) = s.recvfrom(9000)
        except socket.timeout:
            continue
        if ip == me or not (struct.unpack(">H", data[2:4])[0] & 0x8000):
            continue  # our own query, or someone else's query
        seen.setdefault(ip, set()).update(labels(data)[:6])

print(f"responders seen from {me}:")
for ip in sorted(seen, key=lambda a: tuple(int(x) for x in a.split("."))):
    print(f"  {ip:16s} {', '.join(sorted(seen[ip]))[:110]}")
remote = [ip for ip in seen if not ip.startswith(".".join(me.split(".")[:3]) + ".")]
print("devices from another subnet:", remote if remote else "none - mDNS is NOT crossing the tunnel")
