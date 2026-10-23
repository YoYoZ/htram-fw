// Apple Home support, via HomeSpan.
//
// The monitor appears in the Home app as one accessory with a CO2 sensor,
// a temperature sensor, a humidity sensor and a battery. HomeSpan runs in its
// own task and owns Wi-Fi, mDNS and over-the-air updates once started.
#pragma once

#include <Arduino.h>

namespace homekit {

// Starts HomeSpan on the given network. `serial` goes into the accessory's
// information record. Returns once HomeSpan's task is running.
void begin(const String &ssid, const String &pass, const String &serial);

// True once HomeSpan has brought Wi-Fi up for the first time.
bool network_ready();

// The 8-digit setup code to type into the Home app, formatted "123-45-600".
// Derived from the chip's MAC address, so it is stable and unique per device:
// the 6 digits of setup_pin() followed by "00".
String setup_code();

// The 6 digits the device's own screen can show (see gd32::show_pin).
uint32_t setup_pin();

// True once the accessory is paired with a Home. False before begin().
bool paired();

}  // namespace homekit
