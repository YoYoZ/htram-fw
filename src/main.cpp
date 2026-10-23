// htram-fw - replacement firmware for the ESP32 in a Honeywell HTRAM-V1-W.
//
// Honeywell retired the phone app this CO2 monitor needed, which left a working
// sensor with no way to get its readings off the device. The monitor's own
// display keeps working regardless; this firmware replaces only the Wi-Fi/BLE
// module beside it, asks the main board for a reading every few seconds, and
// serves the result on the local network as JSON.
//
//   GET  /api/data      { "co2": 1200, "level": "yellow", "temperature": 21, ... }
//   GET  /api/settings  the device's own settings; POST to change them
//   GET  /api/history   the device's built-in one-minute log (JSON or CSV)
//   GET  /metrics       Prometheus text format
//   GET  /              status page, including the Apple Home setup code
//   GET  /settings      device settings form
//   GET  /setup         Wi-Fi credentials
//
// It also shows up in Apple Home as a CO2, temperature, humidity and battery
// sensor (see homekit.cpp).
//
// First run with no credentials stored, the device puts up its own access point
// called HTRAM-Setup (password htram-setup); connect to it and open
// http://192.168.4.1 to point it at your network.
//
// Nothing here talks to a manufacturer server, and the original firmware can be
// restored at any time from a backup of the module's flash.

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "device.h"
#include "gd32_link.h"
#include "homekit.h"
#include "version.h"

static const char *SETUP_SSID = "HTRAM-Setup";
static const char *SETUP_PASS = "htram-setup";
// No network this long after boot (wrong password, router gone): restart into
// the setup portal, and leave the portal again after a while to retry Wi-Fi.
static const uint32_t WIFI_GIVE_UP_MS = 3UL * 60 * 1000;
static const uint32_t PORTAL_RETRY_MS = 10UL * 60 * 1000;
static uint32_t portal_until = 0;  // 0 = stay in the portal (no credentials at all)

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;
static bool portal_mode = false;
static bool web_started = false;
static String wifi_ssid, wifi_pass;

// ---------------------------------------------------------------------------
// Small HTML helpers. The pages are deliberately plain: this runs on a device
// with no internet access and should render anywhere.
// ---------------------------------------------------------------------------
static const char *PAGE_HEAD =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font:16px system-ui,sans-serif;margin:2rem auto;max-width:34rem;"
    "padding:0 1rem;color:#222;background:#fafafa}"
    "h1{font-size:1.3rem}table{border-collapse:collapse;width:100%}"
    "td{padding:.4rem 0;border-bottom:1px solid #e3e3e3}"
    "td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
    "input{width:100%;padding:.5rem;margin:.3rem 0 .9rem;box-sizing:border-box}"
    "button{padding:.5rem 1rem}.m{color:#666;font-size:.85rem}"
    "@media(prefers-color-scheme:dark){body{background:#151515;color:#eee}"
    "td{border-color:#333}}</style>";

static String escape(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static String code_box();

// ---------------------------------------------------------------------------
// Access control
//
// Optional. With no admin password set everything is open, as on any plain
// LAN gadget. With one set (on /setup), every request that changes something
// needs it (HTTP basic auth, user "admin"), it becomes the OTA password, and
// it replaces the public password of the HTRAM-Setup access point - so the
// fallback portal is only usable by someone who knows it. Reading stays open.
// ---------------------------------------------------------------------------
static String admin_pass;

static bool authorized() {
  if (!admin_pass.length() || server.authenticate("admin", admin_pass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "HTRAM", "Admin password required");
  return false;
}

// Over-the-air update password: a build-time override, else the admin
// password, else one unique to this chip, shown on the setup page.
#ifndef HTRAM_OTA_PASS
#define HTRAM_OTA_PASS ""
#endif
static String ota_password() {
  if (strlen(HTRAM_OTA_PASS)) return HTRAM_OTA_PASS;
  if (admin_pass.length()) return admin_pass;
  uint64_t h = ESP.getEfuseMac() * 0x9E3779B97F4A7C15ULL;
  h ^= h >> 29;
  char b[20];
  snprintf(b, sizeof(b), "htram-%08lx", (unsigned long)(h & 0xffffffffULL));
  return b;
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
static void handle_data() {
  Reading r = gd32::reading();
  String j = "{";
  j += "\"online\":" + String(gd32::online() ? "true" : "false");
  j += ",\"valid\":" + String(r.valid ? "true" : "false");
  j += ",\"warming_up\":" + String(r.warming_up ? "true" : "false");
  if (r.valid && !r.warming_up) j += ",\"co2\":" + String(r.co2);
  else j += ",\"co2\":null";
  if (r.valid) {
    j += ",\"temperature\":" + String(r.temperature);
    j += ",\"humidity\":" + String(r.humidity);
    j += ",\"battery\":" + String(r.battery);
    j += ",\"battery_percent\":" + String(r.battery * 25);
    j += ",\"charging\":" + String(r.charging ? "true" : "false");
    j += ",\"age_seconds\":" + String((millis() - r.millis_at) / 1000);
  }
  // The monitor's own "transmission risk": its traffic light under the
  // thresholds for the selected activity level.
  device::Settings s = device::settings();
  const char *level = (r.valid && !r.warming_up) ? device::risk_level(r.co2) : "";
  j += ",\"level\":" + (*level ? "\"" + String(level) + "\"" : String("null"));
  if (s.valid) {
    j += ",\"activity\":\"" + String(device::activity_name(s.activity)) + "\"";
    j += ",\"yellow_ppm\":" + String(s.yellow_ppm) + ",\"red_ppm\":" + String(s.red_ppm);
  }
  j += ",\"uptime_seconds\":" + String(millis() / 1000);
  j += ",\"firmware_version\":\"" HTRAM_FW_VERSION "\"";
  j += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

// ---------------------------------------------------------------------------
// Device settings and history
// ---------------------------------------------------------------------------
static String settings_json() {
  device::Settings s = device::settings();
  String j = "{\"valid\":" + String(s.valid ? "true" : "false");
  if (s.valid) {
    j += ",\"activity\":\"" + String(device::activity_name(s.activity)) + "\"";
    j += ",\"yellow_ppm\":" + String(s.yellow_ppm) + ",\"red_ppm\":" + String(s.red_ppm);
    j += ",\"screen_off_seconds\":" + String(s.screen_off_s);
    j += ",\"alarm_sound\":" + String(s.alarm_sound ? "true" : "false");
    j += ",\"temperature_unit\":\"" + String(s.fahrenheit ? "F" : "C") + "\"";
    j += ",\"serial\":\"" + s.serial + "\",\"firmware\":\"" + s.firmware + "\",\"sku\":\"" + s.sku + "\"";
  }
  j += ",\"clock_synced\":" + String(s.clock_synced_at ? "true" : "false");
  if (s.clock_synced_at) j += ",\"clock_synced_seconds_ago\":" + String((millis() - s.clock_synced_at) / 1000);
  return j + "}";
}

// Applies whichever settings the request carries. Returns an error message,
// or "" if everything that was asked for was accepted.
static String apply_settings() {
  String err;
  if (server.hasArg("activity")) {
    String a = server.arg("activity");
    bool ok = a == "low"      ? device::set_activity(device::ACTIVITY_LOW)
              : a == "medium" ? device::set_activity(device::ACTIVITY_MEDIUM)
              : a == "high"   ? device::set_activity(device::ACTIVITY_HIGH)
              : a == "custom" ? device::set_thresholds(server.arg("yellow").toInt(), server.arg("red").toInt())
                              : false;
    if (!ok) err += "activity/thresholds not accepted; ";
  } else if (server.hasArg("yellow") || server.hasArg("red")) {
    if (!device::set_thresholds(server.arg("yellow").toInt(), server.arg("red").toInt()))
      err += "thresholds not accepted (red must be at least 100 above yellow); ";
  }
  if (server.hasArg("screen_off") && !device::set_screen_off(server.arg("screen_off").toInt()))
    err += "screen_off not accepted; ";
  if (server.hasArg("alarm_sound")) {
    String v = server.arg("alarm_sound");
    if (!device::set_alarm_sound(v == "1" || v == "on" || v == "true")) err += "alarm_sound not accepted; ";
  }
  if (server.hasArg("unit")) {
    String v = server.arg("unit");
    if (!device::set_fahrenheit(v == "F" || v == "f")) err += "unit not accepted; ";
  }
  if (server.hasArg("sync_clock") && !device::sync_clock()) err += "clock sync failed (no network time yet?); ";
  return err;
}

static void handle_api_settings() {
  if (server.method() == HTTP_POST) {
    if (!authorized()) return;
    String err = apply_settings();
    if (err.length()) {
      server.send(400, "application/json", "{\"error\":\"" + err + "\"}");
      return;
    }
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", settings_json());
}

// GET /api/history?minutes=1440[&format=csv]. One record per minute; reading
// takes about 25 ms per 16 records, so a day is ~2 s and a week ~15 s.
static void handle_history() {
  uint32_t minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 60;
  minutes = constrain(minutes, 1u, 10080u);
  bool csv = server.arg("format") == "csv";
  std::vector<device::Record> recs = device::history(minutes);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, csv ? "text/csv" : "application/json", "");
  // time_trusted: the timestamp is real time. time_estimated: it was shifted
  // from the device's unsynced clock (accurate to about a minute).
  server.sendContent(csv ? "time,co2,temperature,humidity,time_trusted,time_estimated\n"
                         : "{\"interval_seconds\":60,\"fields\":[\"time\",\"co2\",\"temperature\","
                           "\"humidity\",\"time_trusted\",\"time_estimated\"],\"records\":[");
  String chunk;
  for (size_t i = 0; i < recs.size(); i++) {
    const device::Record &r = recs[i];
    String t = String(r.time_trusted() ? "true" : "false") + "," + (r.estimated ? "true" : "false");
    if (csv)
      chunk += String(r.time) + "," + r.co2 + "," + r.temperature + "," + r.humidity + "," + t + "\n";
    else
      chunk += String(i ? "," : "") + "[" + r.time + "," + r.co2 + "," + r.temperature + "," + r.humidity + "," + t + "]";
    if (chunk.length() > 1200) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  if (!csv) chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");
}

static void handle_settings_page() {
  if (server.method() == HTTP_POST) {
    if (!authorized()) return;
    String err = apply_settings();
    server.sendHeader("Location", err.length() ? "/settings?error=" + err : String("/settings?saved=1"));
    server.send(303);
    return;
  }
  device::Settings s = device::settings();
  String h = PAGE_HEAD;
  h += "<title>HTRAM settings</title><h1>Device settings</h1>";
  if (server.hasArg("saved")) h += "<p class=m>Saved.</p>";
  if (server.hasArg("error")) h += "<p class=m>" + escape(server.arg("error")) + "</p>";
  if (!s.valid) {
    h += "<p>Waiting for the main board&hellip;</p>";
    server.send(200, "text/html", h);
    return;
  }
  auto opt = [&](const char *v, const char *label) {
    String a = device::activity_name(s.activity);
    return String("<option value=") + v + (a == v ? " selected>" : ">") + label + "</option>";
  };
  h += "<form method=POST action=/settings><label>Activity level</label><select name=activity "
       "style='width:100%;padding:.5rem;margin:.3rem 0 .9rem'>";
  h += opt("low", "Low &mdash; classrooms, libraries (yellow 800, red 1100)");
  h += opt("medium", "Medium &mdash; restaurants, offices (yellow 700, red 1000)");
  h += opt("high", "High &mdash; gyms, choirs (yellow 500, red 800)");
  h += opt("custom", "Custom (below)");
  h += "</select><label>Custom yellow / red, ppm</label><div style='display:flex;gap:.5rem'>"
       "<input name=yellow type=number min=400 max=2000 step=50 value=" + String(s.yellow_ppm) + ">"
       "<input name=red type=number min=500 max=5000 step=50 value=" + String(s.red_ppm) + "></div>"
       "<button>Apply level</button></form>";

  h += "<form method=POST action=/settings><label>Display</label><select name=screen_off "
       "style='width:100%;padding:.5rem;margin:.3rem 0 .9rem'>";
  h += String("<option value=0") + (s.screen_off_s ? ">" : " selected>") + "Always on</option>";
  h += String("<option value=120") + (s.screen_off_s ? " selected>" : ">") + "Turn off after 2 minutes</option>";
  h += "</select><label>Alarm sound</label><select name=alarm_sound "
       "style='width:100%;padding:.5rem;margin:.3rem 0 .9rem'>";
  h += String("<option value=1") + (s.alarm_sound ? " selected>" : ">") + "On</option>";
  h += String("<option value=0") + (s.alarm_sound ? ">" : " selected>") + "Muted</option>";
  h += "</select><label>Temperature unit on the display</label><select name=unit "
       "style='width:100%;padding:.5rem;margin:.3rem 0 .9rem'>";
  h += String("<option value=C") + (s.fahrenheit ? ">" : " selected>") + "&deg;C</option>";
  h += String("<option value=F") + (s.fahrenheit ? " selected>" : ">") + "&deg;F</option>";
  h += "</select><button>Save</button></form>";

  h += "<p class=m>Serial " + escape(s.serial) + " &middot; main board firmware " + escape(s.firmware) +
       " &middot; clock " + (s.clock_synced_at ? "synced" : "not synced yet") +
       "<br><a href=/>Back</a> &middot; <a href=/api/history?minutes=1440&format=csv>last 24 h as CSV</a></p>";
  server.send(200, "text/html", h);
}

// Developer access to the device protocol: sends one frame to the main board
// and returns its reply. Either a whole frame,  /api/raw?frame=7b4100062021...
// or a command and payload, with the CRC added: /api/raw?cmd=2021&payload=01
// Off in normal builds: it can send the main board anything, including
// switching it to Wi-Fi mode. Build with -DHTRAM_RAW_API=1 to explore.
#ifndef HTRAM_RAW_API
#define HTRAM_RAW_API 0
#endif
#if HTRAM_RAW_API
static String to_hex(const uint8_t *b, size_t n) {
  static const char *digits = "0123456789abcdef";
  String s;
  for (size_t i = 0; i < n; i++) {
    s += digits[b[i] >> 4];
    s += digits[b[i] & 15];
  }
  return s;
}

static size_t from_hex(String hex, uint8_t *out, size_t cap) {
  hex.replace(" ", "");
  size_t n = 0;
  for (size_t i = 0; i + 1 < hex.length() && n < cap; i += 2)
    out[n++] = strtoul(hex.substring(i, i + 2).c_str(), nullptr, 16);
  return n;
}

static void handle_raw() {
  if (!authorized()) return;
  uint8_t req[128], rep[256];
  size_t n = from_hex(server.arg("frame"), req, sizeof(req));
  if (!n && server.hasArg("cmd")) {
    uint8_t payload[100];
    size_t p = from_hex(server.arg("payload"), payload, sizeof(payload));
    n = gd32::build_frame(strtoul(server.arg("cmd").c_str(), nullptr, 16), payload, p, req);
  }
  if (!gd32::frame_ok(req, n)) {
    server.send(400, "application/json", "{\"error\":\"not a valid frame\"}");
    return;
  }
  uint32_t t = millis();
  size_t m = gd32::transact(req, n, rep, sizeof(rep), 2500);
  String j = "{\"request\":\"" + to_hex(req, n) + "\",\"reply\":";
  j += m ? "\"" + to_hex(rep, m) + "\"" : String("null");
  j += ",\"ms\":" + String(millis() - t) + "}";
  server.send(m ? 200 : 504, "application/json", j);
}
#endif

static void handle_metrics() {
  Reading r = gd32::reading();
  const LinkStats &s = gd32::stats();
  String m;
  m += "# HELP htram_co2_ppm Carbon dioxide concentration\n";
  m += "# TYPE htram_co2_ppm gauge\n";
  if (r.valid && !r.warming_up) m += "htram_co2_ppm " + String(r.co2) + "\n";
  if (r.valid) {
    m += "# TYPE htram_temperature_celsius gauge\n";
    m += "htram_temperature_celsius " + String(r.temperature) + "\n";
    m += "# TYPE htram_humidity_percent gauge\n";
    m += "htram_humidity_percent " + String(r.humidity) + "\n";
    m += "# TYPE htram_battery_level gauge\n";
    m += "htram_battery_level " + String(r.battery) + "\n";
    m += "# TYPE htram_charging gauge\n";
    m += "htram_charging " + String(r.charging ? 1 : 0) + "\n";
  }
  m += "# TYPE htram_link_requests_total counter\n";
  m += "htram_link_requests_total " + String(s.requests) + "\n";
  m += "# TYPE htram_link_replies_total counter\n";
  m += "htram_link_replies_total " + String(s.replies) + "\n";
  m += "# TYPE htram_link_bad_frames_total counter\n";
  m += "htram_link_bad_frames_total " + String(s.bad_frames) + "\n";

  device::Settings ds = device::settings();
  if (ds.valid) {
    m += "# HELP htram_risk_level The device's traffic light: 0 green, 1 yellow, 2 red\n";
    m += "# TYPE htram_risk_level gauge\n";
    const char *level = (r.valid && !r.warming_up) ? device::risk_level(r.co2) : "";
    if (*level) m += "htram_risk_level " + String(!strcmp(level, "red") ? 2 : !strcmp(level, "yellow") ? 1 : 0) + "\n";
    m += "# TYPE htram_threshold_ppm gauge\n";
    m += "htram_threshold_ppm{level=\"yellow\"} " + String(ds.yellow_ppm) + "\n";
    m += "htram_threshold_ppm{level=\"red\"} " + String(ds.red_ppm) + "\n";
    m += "# TYPE htram_activity_info gauge\n";
    m += "htram_activity_info{activity=\"" + String(device::activity_name(ds.activity)) + "\"} 1\n";
  }
  m += "# TYPE htram_uptime_seconds counter\n";
  m += "htram_uptime_seconds " + String(millis() / 1000) + "\n";
  m += "# TYPE htram_build_info gauge\n";
  m += "htram_build_info{version=\"" HTRAM_FW_VERSION "\"} 1\n";
  server.send(200, "text/plain; version=0.0.4", m);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
static const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "external reset";
    default: return "unknown";
  }
}

static void handle_diag() {
  const LinkStats &s = gd32::stats();
  device::Settings ds = device::settings();
  String j = "{\"firmware_version\":\"" HTRAM_FW_VERSION "\"";
  j += ",\"uptime_seconds\":" + String(millis() / 1000);
  j += ",\"reset_reason\":\"" + String(reset_reason()) + "\"";
  j += ",\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"min_free_heap\":" + String(ESP.getMinFreeHeap());
  j += ",\"wifi\":{\"mode\":\"" + String(portal_mode ? "setup portal" : "station") + "\"";
  if (!portal_mode) j += ",\"ssid\":\"" + WiFi.SSID() + "\",\"rssi\":" + String(WiFi.RSSI()) +
                         ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += "},\"homekit_paired\":" + String(homekit::paired() ? "true" : "false");
  j += ",\"link\":{\"online\":" + String(gd32::online() ? "true" : "false");
  j += ",\"requests\":" + String(s.requests) + ",\"replies\":" + String(s.replies);
  j += ",\"bad_frames\":" + String(s.bad_frames) + ",\"at_commands\":" + String(s.at_commands) + "}";
  j += ",\"clock_synced\":" + String(ds.clock_synced_at ? "true" : "false");
  if (ds.clock_synced_at) j += ",\"clock_synced_seconds_ago\":" + String((millis() - ds.clock_synced_at) / 1000);
  j += ",\"main_board_firmware\":\"" + ds.firmware + "\"}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

static void handle_root() {
  Reading r = gd32::reading();
  const LinkStats &s = gd32::stats();
  String h = PAGE_HEAD;
  h += "<title>HTRAM</title><h1>Transmission Risk Air Monitor</h1><table>";
  if (!gd32::online()) {
    h += "<tr><td colspan=2>Waiting for the main board&hellip;</td></tr>";
  } else if (!r.valid) {
    h += "<tr><td colspan=2>Connected, no reading yet.</td></tr>";
  } else {
    h += "<tr><td>CO&#8322;</td><td>";
    h += r.warming_up ? String("sensor warming up") : String(r.co2) + " ppm";
    h += "</td></tr>";
    const char *level = r.warming_up ? "" : device::risk_level(r.co2);
    device::Settings ds = device::settings();
    if (*level) {
      const char *dot = !strcmp(level, "red") ? "#d33" : !strcmp(level, "yellow") ? "#e9b200" : "#2a2";
      h += String("<tr><td>Transmission risk</td><td><span style='color:") + dot + "'>&#9679;</span> " +
           level + " &middot; " + device::activity_name(ds.activity) + " activity (" +
           String(ds.yellow_ppm) + " / " + String(ds.red_ppm) + " ppm)</td></tr>";
    }
    h += "<tr><td>Temperature</td><td>" + String(r.temperature) + " &deg;C</td></tr>";
    h += "<tr><td>Humidity</td><td>" + String(r.humidity) + " %</td></tr>";
    h += "<tr><td>Battery</td><td>" + String(r.battery * 25) + " %" +
         (r.charging ? " (charging)" : "") + "</td></tr>";
    h += "<tr><td>Reading age</td><td>" + String((millis() - r.millis_at) / 1000) +
         " s</td></tr>";
  }
  h += "</table><p class=m>htram-fw " HTRAM_FW_VERSION " &middot; up " + String(millis() / 1000) +
       " s since " + reset_reason() + " &middot; " + String(s.replies) + " of " + String(s.requests) +
       " readings answered";
  if (s.bad_frames) h += ", " + String(s.bad_frames) + " bad frames";
  h += " &middot; clock " + String(device::settings().clock_synced_at ? "synced" : "not synced");
  h += "<br>IP " + (portal_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  if (!portal_mode) h += ", Wi-Fi " + String(WiFi.RSSI()) + " dBm";
  if (!portal_mode && homekit::paired()) h += "<br>Paired with Apple Home.";
  h += "</p>";
  if (!portal_mode && !homekit::paired()) h += code_box();
  h += "<p class=m>";
  h += "<br><a href=/settings>Device settings</a> &middot; <a href=/api/data>/api/data</a>"
       " &middot; <a href=/api/history>/api/history</a> &middot; <a href=/metrics>/metrics</a>"
       " &middot; <a href=/api/diag>/api/diag</a> &middot; <a href=/setup>Wi-Fi setup</a></p>";
  // Last 24 hours from the device's own log, drawn client-side.
  h += "<svg id=c viewBox='0 0 600 120' style='width:100%;height:120px'></svg>"
       "<p class=m id=cl>Loading the last 24 h from the device&hellip;</p><script>"
       "fetch('/api/history?minutes=1440').then(r=>r.json()).then(d=>{"
       "let v=d.records.filter(x=>x[4]).map(x=>[x[0],x[1]]);if(v.length<2){cl.textContent='No history yet.';return}"
       "let t0=v[0][0],t1=v[v.length-1][0],lo=Math.min(...v.map(x=>x[1])),hi=Math.max(...v.map(x=>x[1]),lo+100);"
       "let p=v.map(x=>((x[0]-t0)/(t1-t0||1)*600).toFixed(1)+','+(115-(x[1]-lo)/(hi-lo)*110).toFixed(1)).join(' ');"
       "c.innerHTML=\"<polyline fill=none stroke=currentColor stroke-width=1.5 points='\"+p+\"'/>\";"
       "cl.textContent='CO\\u2082 '+lo+'\\u2013'+hi+' ppm over '+((t1-t0)/3600).toFixed(1)+' h'})"
       ".catch(()=>cl.textContent='History unavailable.')</script>";
  server.send(200, "text/html", h);
}

// The Apple Home setup code, big enough to read off a phone. It comes from the
// chip's MAC address, so it is known before the device ever joins a network -
// which is the point: this is the only place a user without a serial cable
// can see it before pairing.
static String code_box() {
  return "<div style='border:2px solid currentColor;border-radius:8px;padding:.8rem;margin:1rem 0;"
         "text-align:center'><div class=m>Apple Home setup code &mdash; note it down</div>"
         "<div style='font-size:2rem;font-weight:600;letter-spacing:.08em;"
         "font-variant-numeric:tabular-nums'>" +
         homekit::setup_code() +
         "</div><div class=m>Home app &rarr; Add Accessory &rarr; More options &rarr; "
         "HTRAM CO2 Monitor. The monitor's screen shows the first six digits as its "
         "pairing PIN; add <b>00</b>.</div>"
         "<form method=POST action=/show-code style='margin:.6rem 0 0'>"
         "<button>Show it on the monitor's screen</button></form></div>";
}

// Puts the 6-digit PIN on the device's screen for a minute. Readings pause
// for that minute, so it only happens on request and once after boot.
static void handle_show_code() {
  if (!portal_mode && !authorized()) return;
  gd32::show_pin(homekit::setup_pin(), 60000);
  server.sendHeader("Location", portal_mode ? String("/setup") : String("/"));
  server.send(303);
}

// In the setup portal the access point itself is the protection (its password
// is the admin password once one is set); on the home network the page asks.
static void handle_setup_page() {
  if (!portal_mode && !authorized()) return;
  String h = PAGE_HEAD;
  h += "<title>HTRAM Wi-Fi</title><h1>Transmission Risk Air Monitor</h1>";
  h += code_box();
  h += "<form method=POST action=/setup><label>Wi-Fi network</label>"
       "<input name=ssid list=nets autocomplete=off value=\"" + escape(wifi_ssid) + "\"><datalist id=nets>";
  // Offer what is in range. Takes a couple of seconds; fine for a setup page.
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; i++) h += "<option value=\"" + escape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
  h += "</datalist><label>Password</label><input name=pass type=password>";
  h += "<label>Admin password <span class=m>(optional, 8+ characters)</span></label>"
       "<input name=admin type=password autocomplete=new-password>";
  if (admin_pass.length())
    h += "<label class=m><input type=checkbox name=clear_admin style='width:auto'> remove the admin password</label>";
  h += "<button>Save and restart</button></form>";
  h += "<p class=m>Passwords are stored on the device only. The admin password protects every change "
       "(user <b>admin</b>), becomes the password of the HTRAM-Setup network, and is the OTA password.";
  if (!admin_pass.length() && !strlen(HTRAM_OTA_PASS))
    h += " Without one, this device's OTA password is <b>" + ota_password() + "</b>.";
  h += "</p>";
  server.send(200, "text/html", h);
}

static void handle_setup_save() {
  if (!portal_mode && !authorized()) return;
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String admin = server.arg("admin");
  if (!ssid.length()) {
    server.send(400, "text/plain", "network name required");
    return;
  }
  if (admin.length() && admin.length() < 8) {  // also the WPA2 password of the setup network
    server.send(400, "text/plain", "admin password must be at least 8 characters");
    return;
  }
  prefs.begin("htram", false);
  prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("pass", pass);
  if (admin.length()) prefs.putString("admin", admin);
  else if (server.hasArg("clear_admin")) prefs.remove("admin");
  prefs.end();

  String h = PAGE_HEAD;
  h += "<title>HTRAM</title><h1>Saved</h1><p>The monitor restarts and joins <b>" + escape(ssid) +
       "</b>. Put your phone back on that network, then open <b>http://htram.local</b> "
       "(or find the monitor's address in your router).</p>";
  h += code_box();
  h += "<p class=m>If it cannot join, it opens the HTRAM-Setup network again after a while.</p>";
  server.send(200, "text/html", h);
  delay(1500);  // let the page reach the phone before the access point goes away
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
// The setup access point is a captive portal: every DNS name resolves to the
// device, and the probe URLs phones use to detect one are redirected to the
// setup page, so it opens by itself as soon as someone joins HTRAM-Setup.
static void start_portal() {
  portal_mode = true;
  WiFi.mode(WIFI_AP_STA);  // STA side only for scanning nearby networks
  WiFi.softAP(SETUP_SSID, admin_pass.length() >= 8 ? admin_pass.c_str() : SETUP_PASS);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
}

static void redirect_to_setup() {
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup");
  server.send(302, "text/plain", "");
}

static void start_web() {
  server.on("/", handle_root);
  server.on("/api/data", handle_data);
  server.on("/metrics", handle_metrics);
  server.on("/api/diag", handle_diag);
#if HTRAM_RAW_API
  server.on("/api/raw", handle_raw);
#endif
  server.on("/api/settings", handle_api_settings);
  server.on("/api/history", handle_history);
  server.on("/settings", handle_settings_page);
  server.on("/show-code", HTTP_POST, handle_show_code);
  server.on("/setup", HTTP_GET, handle_setup_page);
  server.on("/setup", HTTP_POST, handle_setup_save);
  server.onNotFound([]() {
    if (portal_mode) redirect_to_setup();  // captive-portal probes and stray URLs
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
  if (!portal_mode) MDNS.addService("http", "tcp", 80);  // HomeSpan owns mDNS itself
  web_started = true;
}

void setup() {
  // The main board starts talking as soon as it powers us up, so bring the
  // link up before anything slow.
  gd32::begin();
  device::begin();  // settings, clock sync, history; waits for the link by itself

  prefs.begin("htram", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  String serial = prefs.getString("serial", "HTRAM");
  bool fallback = prefs.getBool("portal_once", false);
  admin_pass = prefs.getString("admin", "");
  prefs.end();

  if (!wifi_ssid.length() || fallback) {
    if (fallback) {
      prefs.begin("htram", false);
      prefs.putBool("portal_once", false);
      prefs.end();
      portal_until = millis() + PORTAL_RETRY_MS;  // then retry the saved network
    }
    start_portal();
    start_web();
    return;
  }

  // HomeSpan logs to UART0 but leaves starting it to the sketch.
  Serial.begin(115200);

  // From here HomeSpan runs Wi-Fi, mDNS (htram.local) and OTA updates in its
  // own task, reconnecting by itself if the network drops.
  homekit::begin(wifi_ssid, wifi_pass, serial, ota_password());
}

void loop() {
  // The GD32 link and HomeSpan each run in their own task.
  delay(2);
  if (portal_mode) dns.processNextRequest();
  if (!web_started && homekit::network_ready()) start_web();
  if (web_started) server.handleClient();

  // Not paired yet (or setting up): show the PIN on the device's screen once,
  // so the setup code can be read without a browser or a serial cable.
  static bool pin_shown_at_boot = false;
  if (!pin_shown_at_boot && gd32::online() && (portal_mode || !homekit::paired())) {
    pin_shown_at_boot = true;
    gd32::show_pin(homekit::setup_pin(), 60000);
  }

  // Wi-Fi never came up: fall back to the setup portal for a while.
  if (!portal_mode && !homekit::network_ready() && millis() > WIFI_GIVE_UP_MS) {
    prefs.begin("htram", false);
    prefs.putBool("portal_once", true);
    prefs.end();
    ESP.restart();
  }
  // Fallback portal left unused: try the saved network again.
  if (portal_mode && portal_until && (int32_t)(millis() - portal_until) > 0 && !WiFi.softAPgetStationNum())
    ESP.restart();

  // Remember the unit's serial number for the Home app's accessory record. The
  // GD32 announces it at power-on, before the accessory exists, so it takes
  // effect from the next boot.
  static bool serial_saved = false;
  String name = gd32::device_name();
  if (!serial_saved && name.length()) {
    serial_saved = true;
    prefs.begin("htram", false);
    if (prefs.getString("serial", "") != name) prefs.putString("serial", name);
    prefs.end();
  }
}
