#include "device.h"

#include <WiFi.h>
#include <time.h>

#include "gd32_link.h"

// Commands. A reply carries the request's command plus 0x0100, which
// gd32::transact() matches on. Payload layouts are in docs/PROTOCOL.md.
static const uint16_t CMD_SERIAL = 0x2020;
static const uint16_t CMD_SKU = 0x2021;
static const uint16_t CMD_FIRMWARE = 0x2023;
static const uint16_t CMD_GET_UNIT = 0x206e;
static const uint16_t CMD_HISTORY = 0x2093;
static const uint16_t CMD_SET_UNIT = 0x2232;
static const uint16_t CMD_SET_CLOCK = 0x2242;
static const uint16_t CMD_GET_SOUND = 0x2623;
static const uint16_t CMD_SET_SOUND = 0x2643;
static const uint16_t CMD_GET_ALERTS = 0x4043;  // thresholds and screen-off time
static const uint16_t CMD_SET_ALERTS = 0x4243;  // either, selected by the payload

// The history log is a ring buffer in the device's SPI flash, 128 bytes (16
// one-minute records) per read. The device's own pointer marks the oldest
// packet; the newest sits just before it, past the wrap.
static const uint32_t RING_START = 0x00200000;
static const uint32_t RING_END = 0x00300000;
static const uint32_t PACKET = 0x80;

static const uint32_t CLOCK_RESYNC_MS = 6UL * 3600 * 1000;
static const uint32_t SETTINGS_REFRESH_MS = 10UL * 60 * 1000;

struct Preset {
  device::Activity activity;
  int yellow, red;
};
static const Preset PRESETS[] = {{device::ACTIVITY_LOW, 800, 1100},
                                 {device::ACTIVITY_MEDIUM, 700, 1000},
                                 {device::ACTIVITY_HIGH, 500, 800}};

static SemaphoreHandle_t lock;  // guards `current`
static device::Settings current;
static uint32_t newest_packet = 0;  // cache for history(); 0 = unknown

// ---------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------
// Sends cmd+payload, returns the reply's payload (bytes after the command,
// before the CRC) in `out`, or -1 if nothing came back.
static int command(uint16_t cmd, const uint8_t *payload, size_t n, uint8_t *out, size_t cap) {
  uint8_t req[64], rep[256];
  size_t len = gd32::build_frame(cmd, payload, n, req);
  size_t got = gd32::transact(req, len, rep, sizeof(rep));
  if (got < 10 || !gd32::frame_ok(rep, got)) return -1;
  int body = (int)got - 9;
  if (out) memcpy(out, rep + 6, min((size_t)body, cap));
  return body;
}

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static void with_settings(void (*fn)(device::Settings &, void *), void *ctx) {
  xSemaphoreTake(lock, portMAX_DELAY);
  fn(current, ctx);
  xSemaphoreGive(lock);
}

static device::Activity infer_activity(int yellow, int red) {
  for (const Preset &p : PRESETS)
    if (p.yellow == yellow && p.red == red) return p.activity;
  return device::ACTIVITY_CUSTOM;
}

static String ascii(const uint8_t *p, int n) {
  String s;
  for (int i = 0; i < n; i++)
    if (p[i] >= 32 && p[i] < 127) s += (char)p[i];
  s.trim();
  return s;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
// Reads one 128-byte packet. Returns false if the device did not answer.
static bool read_packet(uint32_t addr, uint32_t *next, std::vector<device::Record> *recs) {
  uint8_t payload[5] = {0x01, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8),
                        (uint8_t)addr};
  uint8_t body[200];
  int n = command(CMD_HISTORY, payload, sizeof(payload), body, sizeof(body));
  if (n < 5) return false;
  if (next) *next = ((uint32_t)body[1] << 24) | ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 8) | body[4];
  if (recs) {
    recs->clear();
    for (int i = 5; i + 8 <= n; i += 8) {
      const uint8_t *r = body + i;
      if (r[0] == 0xff && r[1] == 0xff && r[2] == 0xff && r[3] == 0xff) continue;  // erased flash
      device::Record rec;
      rec.time = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3];
      rec.co2 = be16(r + 4);
      rec.temperature = (int8_t)r[6];
      rec.humidity = r[7];
      recs->push_back(rec);
    }
  }
  return true;
}

static bool has_records(uint32_t addr) {
  std::vector<device::Record> r;
  return read_packet(addr, nullptr, &r) && !r.empty();
}

static uint32_t prev_packet(uint32_t a) { return a <= RING_START ? RING_END - PACKET : a - PACKET; }
static uint32_t next_packet(uint32_t a) { return a + PACKET >= RING_END ? RING_START : a + PACKET; }

// Last packet holding records in [lo, hi), records being contiguous from lo.
static uint32_t bisect(uint32_t lo, uint32_t hi) {
  while (hi - lo > PACKET) {
    uint32_t mid = (lo + (hi - lo) / 2) & ~(PACKET - 1);
    if (has_records(mid)) lo = mid;
    else hi = mid;
  }
  return lo;
}

static bool find_newest(uint32_t *newest, uint32_t *oldest) {
  uint32_t start;
  if (!read_packet(0xFFFFFFFF, &start, nullptr)) return false;
  *oldest = start;

  // Usually the cached position is right, or a packet or two behind.
  if (newest_packet && has_records(newest_packet)) {
    uint32_t a = newest_packet;
    for (int i = 0; i < 64 && next_packet(a) != start && has_records(next_packet(a)); i++)
      a = next_packet(a);
    *newest = newest_packet = a;
    return true;
  }
  if (RING_START < start && has_records(RING_START)) *newest = bisect(RING_START, start);
  else *newest = bisect(start, RING_END);
  newest_packet = *newest;
  return true;
}

// ---------------------------------------------------------------------------
// Background task
// ---------------------------------------------------------------------------
static bool clock_ok() { return time(nullptr) > 1704067200; }  // after 2024-01-01

static void device_task(void *) {
  bool was_online = false, ntp_started = false, need_sync = false;
  uint32_t last_refresh = 0;
  for (;;) {
    if (!ntp_started && WiFi.isConnected()) {
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      ntp_started = true;
    }
    bool online = gd32::online();
    if (online && !was_online) {
      device::refresh();
      last_refresh = millis();
      need_sync = true;  // the GD32's clock restarts whenever it loses power
    }
    was_online = online;

    if (online && need_sync && clock_ok() && device::sync_clock()) need_sync = false;
    device::Settings s = device::settings();
    if (online && s.clock_synced_at && millis() - s.clock_synced_at > CLOCK_RESYNC_MS) need_sync = true;
    if (online && millis() - last_refresh > SETTINGS_REFRESH_MS) {
      device::refresh();
      last_refresh = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
namespace device {

void begin() {
  lock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(device_task, "device", 6144, nullptr, 1, nullptr, 1);
}

Settings settings() {
  xSemaphoreTake(lock, portMAX_DELAY);
  Settings s = current;
  xSemaphoreGive(lock);
  return s;
}

const char *risk_level(int co2) {
  Settings s = settings();
  if (!s.valid || !s.yellow_ppm) return "";
  if (co2 >= s.red_ppm) return "red";
  if (co2 >= s.yellow_ppm) return "yellow";
  return "green";
}

const char *activity_name(Activity a) {
  switch (a) {
    case ACTIVITY_LOW: return "low";
    case ACTIVITY_MEDIUM: return "medium";
    case ACTIVITY_HIGH: return "high";
    case ACTIVITY_CUSTOM: return "custom";
    default: return "unknown";
  }
}

bool refresh() {
  // Query payloads, exactly as the app sent them.
  static const uint8_t Q_ALERTS[] = {0x04, 0x00, 0x60, 0x06};  // thresholds + screen-off
  static const uint8_t Q_SOUND[] = {0x01, 0x00};
  static const uint8_t Q_UNIT[] = {0x02, 0x06};
  static const uint8_t Q_SERIAL[] = {0x03};
  static const uint8_t Q_ONE[] = {0x01};

  uint8_t b[64];
  Settings s = settings();
  bool ok = true;

  int n = command(CMD_GET_ALERTS, Q_ALERTS, sizeof(Q_ALERTS), b, sizeof(b));
  if (n >= 7) {
    s.yellow_ppm = be16(b + 1);
    s.red_ppm = be16(b + 3);
    s.screen_off_s = be16(b + 5);
    s.activity = infer_activity(s.yellow_ppm, s.red_ppm);
  } else {
    ok = false;
  }
  if ((n = command(CMD_GET_SOUND, Q_SOUND, sizeof(Q_SOUND), b, sizeof(b))) >= 4) s.alarm_sound = b[3] != 0;
  if ((n = command(CMD_GET_UNIT, Q_UNIT, sizeof(Q_UNIT), b, sizeof(b))) >= 2) s.fahrenheit = b[1] != 0;
  if ((n = command(CMD_SERIAL, Q_SERIAL, sizeof(Q_SERIAL), b, sizeof(b))) > 1) s.serial = ascii(b + 1, n - 1);
  if ((n = command(CMD_FIRMWARE, Q_ONE, sizeof(Q_ONE), b, sizeof(b))) > 1) s.firmware = ascii(b + 1, n - 1);
  if ((n = command(CMD_SKU, Q_ONE, sizeof(Q_ONE), b, sizeof(b))) > 1) {
    s.sku = "";
    for (int i = 1; i < n; i++) s.sku += String(b[i] >> 4, HEX) + String(b[i] & 15, HEX);
  }
  if (ok) s.valid = true;
  with_settings([](Settings &cur, void *ctx) { cur = *(Settings *)ctx; }, &s);
  return ok;
}

bool set_thresholds(int yellow, int red) {
  if (yellow < 400 || red > 5000 || red < yellow + 100) return false;
  Activity type = infer_activity(yellow, red);
  uint8_t p[] = {0x04, 0x00, 0x40, 0x06, (uint8_t)(yellow >> 8), (uint8_t)yellow,
                 (uint8_t)(red >> 8), (uint8_t)red, 0x00, (uint8_t)type};
  bool ok = command(CMD_SET_ALERTS, p, sizeof(p), nullptr, 0) >= 0;
  refresh();
  return ok;
}

bool set_activity(Activity a) {
  for (const Preset &p : PRESETS)
    if (p.activity == a) return set_thresholds(p.yellow, p.red);
  return false;
}

bool set_screen_off(int seconds) {
  if (seconds < 0 || seconds > 65535) return false;
  uint8_t p[] = {0x04, 0x00, 0x20, 0x00, (uint8_t)(seconds >> 8), (uint8_t)seconds};
  bool ok = command(CMD_SET_ALERTS, p, sizeof(p), nullptr, 0) >= 0;
  refresh();
  return ok;
}

bool set_alarm_sound(bool on) {
  uint8_t p[] = {0x01, 0x00, 0x00, (uint8_t)(on ? 1 : 0)};
  bool ok = command(CMD_SET_SOUND, p, sizeof(p), nullptr, 0) >= 0;
  refresh();
  return ok;
}

bool set_fahrenheit(bool on) {
  uint8_t p[] = {0x02, 0x06, (uint8_t)(on ? 1 : 0)};
  bool ok = command(CMD_SET_UNIT, p, sizeof(p), nullptr, 0) >= 0;
  refresh();
  return ok;
}

bool sync_clock() {
  if (!clock_ok()) return false;
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);  // the device keeps UTC, as the app set it
  uint8_t p[] = {0x01, (uint8_t)(t.tm_year % 100), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
                 (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec};
  if (command(CMD_SET_CLOCK, p, sizeof(p), nullptr, 0) < 0) return false;
  uint32_t at = millis();
  with_settings([](Settings &cur, void *ctx) { cur.clock_synced_at = *(uint32_t *)ctx; }, &at);
  return true;
}

std::vector<Record> history(uint32_t minutes, size_t max_records) {
  std::vector<Record> out;
  uint32_t newest, oldest;
  if (!find_newest(&newest, &oldest)) return out;

  size_t want = min((size_t)minutes, max_records);
  std::vector<std::vector<Record>> packets;  // newest first
  size_t total = 0;
  uint32_t a = newest;
  while (total < want) {
    std::vector<Record> recs;
    if (!read_packet(a, nullptr, &recs) || recs.empty()) break;
    total += recs.size();
    packets.push_back(std::move(recs));
    if (a == oldest) break;
    a = prev_packet(a);
  }
  out.reserve(total);
  for (auto it = packets.rbegin(); it != packets.rend(); ++it) out.insert(out.end(), it->begin(), it->end());
  if (out.size() > want) out.erase(out.begin(), out.end() - want);
  return out;
}

}  // namespace device
