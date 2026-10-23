// The monitor's own settings, clock and history log, reached through the
// Honeywell commands the retired phone app used (see docs/PROTOCOL.md).
//
// Everything here goes through gd32::transact(), so it may block the calling
// task for a few tens of milliseconds per command. Do not call it from the
// GD32 link task itself.
#pragma once

#include <Arduino.h>
#include <vector>

namespace device {

// The three presets the device ships with, plus custom thresholds. The values
// match the app's own table: more activity in a room, lower CO2 alarm levels.
enum Activity : uint8_t { ACTIVITY_UNKNOWN = 0, ACTIVITY_CUSTOM = 1, ACTIVITY_LOW = 2,
                          ACTIVITY_MEDIUM = 3, ACTIVITY_HIGH = 4 };

struct Settings {
  bool valid = false;          // read from the device at least once
  int yellow_ppm = 0;          // display turns yellow at or above this
  int red_ppm = 0;             // display turns red at or above this
  Activity activity = ACTIVITY_UNKNOWN;  // inferred from the thresholds, as the app did
  int screen_off_s = 0;        // 0 = display always on
  bool alarm_sound = true;
  bool fahrenheit = false;
  String serial, firmware, sku;
  uint32_t clock_synced_at = 0;  // millis() of the last successful time sync, 0 = never
};

struct Record {
  uint32_t time;   // UTC seconds, as stamped by the device
  uint16_t co2;    // ppm
  int8_t temperature;
  uint8_t humidity;
  // The device's clock restarts at 2021-01-01 whenever it loses power and is
  // only correct after a sync. Records from before a sync are shifted to real
  // time when the sync is visible in the log (see history()); those carry
  // `estimated`. Anything still before 2024 is from an unsynced session.
  bool estimated = false;
  bool time_trusted() const { return time >= 1704067200; }  // 2024-01-01
};

// Starts a background task that syncs the clock and reads the settings when
// the link comes up, then re-syncs the clock every few hours.
void begin();

// A copy of the last known settings.
Settings settings();

// "green", "yellow" or "red" for a CO2 level under the device's thresholds,
// or "" while they are unknown. This is the monitor's "transmission risk".
const char *risk_level(int co2);

// Each returns true if the device acknowledged the change; settings are re-read.
bool set_activity(Activity a);                  // LOW, MEDIUM or HIGH preset
bool set_thresholds(int yellow_ppm, int red_ppm);  // custom: yellow 500-1100, red up to 2000
bool set_screen_off(int seconds);               // 0 = always on
bool set_alarm_sound(bool on);
bool set_fahrenheit(bool on);
bool sync_clock();
bool refresh();

// Up to `max_records` of the most recent records, oldest first, reaching back
// no further than `minutes`. Takes roughly 25 ms per 16 records.
std::vector<Record> history(uint32_t minutes, size_t max_records = 10080);

const char *activity_name(Activity a);

}  // namespace device
