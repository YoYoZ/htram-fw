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

}  // namespace gd32
