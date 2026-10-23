# Apple Home across subnets

HomeKit finds accessories with Bonjour (mDNS, UDP 5353 to `224.0.0.251`).
That is link-local multicast: routers do not forward it. If your **home hub**
(Apple TV, HomePod, iPad) sits on a different subnet than the monitor, pairing
from a phone on the monitor's network works, values flash up once — and then
the Home app says **"No Response"**, because the hub never hears the accessory
and never connects.

Diagnose it with [`tools/mdns_cross_test.py`](../tools/mdns_cross_test.py): it
lists every Bonjour responder it can hear. If nothing from the hub's subnet
shows up, discovery is not crossing. HomeSpan's log (UART0, log level 1+) shows
whether the hub ever connects.

Options, simplest first:

1. Put the monitor on the hub's network.
2. Repeat mDNS between the subnets on the router.
3. Bridge the networks at layer 2 (EoIP or similar) with a filter that passes
   only discovery traffic.

## MikroTik: two sites over WireGuard

Our setup: home hAP ac² (192.168.88.0/24, Apple TV) and office hAP ax lite
(192.168.99.0/24, monitor), joined by a WireGuard tunnel `wireguard1`. Both on
RouterOS 7.16+ (7.24 and 7.20 here). RouterOS's built-in mDNS repeater **does**
work across WireGuard once three things are in place, on **both** routers:

```
# 1. repeat mDNS between the LAN bridge and the tunnel
/ip dns set mdns-repeat-ifaces=bridge,wireguard1

# 2. let the multicast address into the tunnel
/interface wireguard peers set [find interface=wireguard1] \
    allowed-address=<existing list>,224.0.0.251/32

# 3. accept mDNS arriving from the tunnel - the step that is easy to miss
/ip firewall filter add chain=input in-interface=wireguard1 protocol=udp \
    dst-port=5353 action=accept comment="mDNS from other site via WG" \
    place-before=[find comment="defconf: drop all not coming from LAN"]
```

Step 3 is the one that silently breaks it: the repeater runs on the router, and
the default firewall drops everything addressed to the router that does not
come from the `LAN` interface list. The tunnel usually is not in that list.

Once discovery crosses, the hub connects to the accessory by plain unicast
(TCP 1201 for htram-fw), which the tunnel routes like any other traffic.

Undo:

```
/ip dns set mdns-repeat-ifaces=""
/ip firewall filter remove [find comment="mDNS from other site via WG"]
```

and take `224.0.0.251/32` back out of the peers.
