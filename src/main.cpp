// htram-fw - replacement firmware for the ESP32 in a Honeywell HTRAM-V1-W.
//
// Honeywell retired the phone app this CO2 monitor needed, which left a working
// sensor with no way to get its readings off the device. The monitor's own
// display keeps working regardless; this firmware replaces only the Wi-Fi/BLE
// module beside it, asks the main board for a reading every few seconds, and
// serves the result on the local network as JSON.
//
//   GET /api/data   { "co2": 1200, "temperature": 21, ... }
//   GET /metrics    Prometheus text format
//   GET /           status page, including the Apple Home setup code
//   GET /setup      Wi-Fi credentials
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
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "gd32_link.h"
#include "homekit.h"

static const char *SETUP_SSID = "HTRAM-Setup";
static const char *SETUP_PASS = "htram-setup";

static WebServer server(80);
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
  j += ",\"uptime_seconds\":" + String(millis() / 1000);
  j += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

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
  server.send(200, "text/plain; version=0.0.4", m);
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
    h += "<tr><td>Temperature</td><td>" + String(r.temperature) + " &deg;C</td></tr>";
    h += "<tr><td>Humidity</td><td>" + String(r.humidity) + " %</td></tr>";
    h += "<tr><td>Battery</td><td>" + String(r.battery * 25) + " %" +
         (r.charging ? " (charging)" : "") + "</td></tr>";
    h += "<tr><td>Reading age</td><td>" + String((millis() - r.millis_at) / 1000) +
         " s</td></tr>";
  }
  h += "</table><p class=m>" + String(s.replies) + " of " + String(s.requests) +
       " requests answered";
  if (s.bad_frames) h += ", " + String(s.bad_frames) + " bad frames";
  h += ".<br>Uptime " + String(millis() / 1000) + " s. IP " +
       (portal_mode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + ".";
  if (!portal_mode)
    h += "<br>Apple Home: Add Accessory &rarr; More options, setup code <b>" +
         homekit::setup_code() + "</b>";
  h += "<br><a href=/api/data>/api/data</a> &middot; <a href=/metrics>/metrics</a>"
       " &middot; <a href=/setup>Wi-Fi setup</a></p>";
  server.send(200, "text/html", h);
}

static void handle_setup_page() {
  String h = PAGE_HEAD;
  h += "<title>HTRAM Wi-Fi</title><h1>Wi-Fi</h1><form method=POST action=/setup>";
  h += "<label>Network</label><input name=ssid value=\"" + escape(wifi_ssid) + "\">";
  h += "<label>Password</label><input name=pass type=password>";
  h += "<button>Save and restart</button></form>";
  h += "<p class=m>The password is stored on the device only.</p>";
  server.send(200, "text/html", h);
}

static void handle_setup_save() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (!ssid.length()) {
    server.send(400, "text/plain", "network name required");
    return;
  }
  prefs.begin("htram", false);
  prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("pass", pass);
  prefs.end();

  String h = PAGE_HEAD;
  h += "<title>HTRAM</title><p>Saved. Restarting&hellip;</p>";
  server.send(200, "text/html", h);
  delay(600);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
static void start_portal() {
  portal_mode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_SSID, SETUP_PASS);
}

static void start_web() {
  server.on("/", handle_root);
  server.on("/api/data", handle_data);
  server.on("/metrics", handle_metrics);
  server.on("/setup", HTTP_GET, handle_setup_page);
  server.on("/setup", HTTP_POST, handle_setup_save);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  if (!portal_mode) MDNS.addService("http", "tcp", 80);  // HomeSpan owns mDNS itself
  web_started = true;
}

void setup() {
  // The main board starts talking as soon as it powers us up, so bring the
  // link up before anything slow.
  gd32::begin();

  prefs.begin("htram", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  String serial = prefs.getString("serial", "HTRAM");
  prefs.end();

  if (!wifi_ssid.length()) {
    start_portal();
    start_web();
    return;
  }

  // HomeSpan logs to UART0 but leaves starting it to the sketch.
  Serial.begin(115200);

  // From here HomeSpan runs Wi-Fi, mDNS (htram.local) and OTA updates in its
  // own task, reconnecting by itself if the network drops.
  homekit::begin(wifi_ssid, wifi_pass, serial);
}

void loop() {
  // The GD32 link and HomeSpan each run in their own task.
  delay(2);
  if (!web_started && homekit::network_ready()) start_web();
  if (web_started) server.handleClient();

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
