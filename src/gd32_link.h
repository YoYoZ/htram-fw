// Talks to the GD32F150 main board of a Honeywell HTRAM-V1-W.
//
// The GD32 runs the device: sensor, display, LEDs, buttons. It has no idea how
// to reach a network, so it drives the ESP32 next to it as an Espressif ESP-AT
// modem over a serial link and publishes readings only when a Bluetooth client
// asks for them.
//
// This module takes the place of that stock ESP-AT firmware. It answers the AT
// commands the GD32 sends at boot, presents a client on the Bluetooth service
// the GD32 just created, and from then on asks for a reading every few seconds
// the same way the manufacturer's phone app used to.
#pragma once

#include <Arduino.h>

struct Reading {
  bool valid = false;        // a well-formed frame has been parsed
  bool warming_up = false;   // sensor reports 65534 while it heats up
  int co2 = 0;               // ppm
  int temperature = 0;       // degrees Celsius, signed
  int humidity = 0;          // percent relative humidity
  int battery = 0;           // 0-4 bars, as shown on the device
  bool charging = false;
  uint32_t millis_at = 0;    // when this reading arrived
};

struct LinkStats {
  uint32_t requests = 0;     // read requests handed to the GD32
  uint32_t replies = 0;      // well-formed replies parsed
  uint32_t bad_frames = 0;   // replies that failed the length or CRC check
  uint32_t at_commands = 0;  // AT commands answered
  uint32_t unknown = 0;      // AT commands answered with a bare OK
};

namespace gd32 {

// Wires up the serial link and starts a task that services it. Call once, as
// early in setup() as possible: the GD32 starts talking as soon as it sees
// "ready" and expects prompt answers, whatever else is still starting up.
void begin();

// A copy of the most recent reading. Safe to call from any task.
Reading reading();

// Counters for the status page. Plain integers, safe to read from any task.
const LinkStats &stats();

// The name the GD32 advertises under, e.g. "HTRAM-RM12xxxxxxxx". Empty until
// the GD32 has run its setup since this module started.
String device_name();

// True once the GD32 has finished its boot sequence and accepted our client.
bool online();

// Asks the GD32 to show a 6-digit number on its screen, as the Bluetooth
// pairing passkey, for about `hold_ms`. Readings pause meanwhile. If the link
// is up it is briefly re-paired to make that happen.
void show_pin(uint32_t pin, uint32_t hold_ms = 60000);

// Builds a device frame: 7b 41 00 <len> <cmd> <payload> <crc16> 7d. `out` needs
// room for n + 9 bytes. Returns the frame length.
size_t build_frame(uint16_t cmd, const uint8_t *payload, size_t n, uint8_t *out);

// True if `f` is a well-formed frame with a valid CRC.
bool frame_ok(const uint8_t *f, size_t n);

// Sends a frame to the GD32 as a client write, between the regular readings,
// and waits for the reply whose command is the request's plus 0x0100. Returns
// the reply's length, or 0 on timeout or while the link is down. Blocks the
// calling task; safe from any task except the link's own.
size_t transact(const uint8_t *req, size_t n, uint8_t *reply, size_t cap, uint32_t timeout_ms = 1500);

}  // namespace gd32
