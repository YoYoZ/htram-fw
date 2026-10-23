#include "homekit.h"

#include <HomeSpan.h>
#include <Preferences.h>

#include "device.h"
#include "gd32_link.h"
#include "version.h"

static const char *HOSTNAME = "htram";
static const uint16_t HAP_PORT = 1201;  // port 80 stays with the JSON API

// "CO2 detected" turns on where the device's own display turns yellow (the
// threshold of its activity level) and off again 100 ppm below, so a reading
// hovering at the threshold does not flap. HTRAM_CO2_ALERT_PPM overrides it
// and is the fallback until the thresholds have been read.
#ifndef HTRAM_CO2_ALERT_PPM
#define HTRAM_CO2_ALERT_PPM 0
#endif
static int alert_ppm() {
  if (HTRAM_CO2_ALERT_PPM) return HTRAM_CO2_ALERT_PPM;
  device::Settings s = device::settings();
  return s.valid && s.yellow_ppm ? s.yellow_ppm : 1000;
}
static const int CO2_ALERT_HYSTERESIS = 100;

// A reading older than this marks the sensors as faulty in the Home app.
static const uint32_t STALE_MS = 60000;

static volatile bool wifi_up = false;

// ---------------------------------------------------------------------------
// Setup code
//
// HomeSpan ships with one fixed code for every device, which is a poor default
// for a firmware meant to be flashed onto many units. Derive one from the MAC
// address instead.
//
// The device's own screen can show a 6-digit Bluetooth pairing PIN (see
// gd32::show_pin), so the code is those 6 digits followed by "00": a user reads
// 123456 off the display and types 123-45-600. With a fixed "00" tail, the only
// code HomeKit would refuse as too simple is 00000000, which is skipped.
// ---------------------------------------------------------------------------
static uint32_t pin6() {
  uint64_t h = ESP.getEfuseMac() ^ 0x9E3779B97F4A7C15ULL;
  uint32_t p;
  do {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    p = (uint32_t)(h % 1000000ULL);
  } while (p == 0);
  return p;
}

static String raw_code() {
  char c[9];
  snprintf(c, sizeof(c), "%06lu00", (unsigned long)pin6());
  return String(c);
}

// ---------------------------------------------------------------------------
// Services
//
// Each one polls the link for a fresh reading and updates its characteristics
// only when a value actually changes, so the Home app is not flooded with
// notifications every five seconds.
// ---------------------------------------------------------------------------
struct Freshness {
  bool usable = false;  // a reading exists and is recent
  bool fault = false;   // the link has gone quiet
};

static Freshness check(const Reading &r) {
  Freshness f;
  bool recent = r.valid && (millis() - r.millis_at) < STALE_MS;
  f.usable = gd32::online() && recent;
  f.fault = !f.usable;
  return f;
}

template <typename T>
static void set_if_changed(SpanCharacteristic *c, T v) {
  if (c->getVal<T>() != v) c->setVal(v);
}

struct Co2Sensor : Service::CarbonDioxideSensor {
  SpanCharacteristic *detected, *level, *peak, *active, *fault;
  uint32_t seen = 0;
  int peak_ppm = 0;

  Co2Sensor() : Service::CarbonDioxideSensor() {
    detected = new Characteristic::CarbonDioxideDetected(0);
    level = new Characteristic::CarbonDioxideLevel(0);
    peak = new Characteristic::CarbonDioxidePeakLevel(0);
    active = new Characteristic::StatusActive(0);
    fault = new Characteristic::StatusFault(0);
  }

  void loop() override {
    Reading r = gd32::reading();
    Freshness f = check(r);
    set_if_changed<uint8_t>(fault, f.fault ? 1 : 0);
    set_if_changed<uint8_t>(active, (f.usable && !r.warming_up) ? 1 : 0);
    if (!f.usable || r.warming_up || r.millis_at == seen) return;
    seen = r.millis_at;

    set_if_changed<float>(level, (float)r.co2);
    if (r.co2 > peak_ppm) {
      peak_ppm = r.co2;
      set_if_changed<float>(peak, (float)peak_ppm);
    }
    uint8_t alert = detected->getVal<uint8_t>();
    int threshold = alert_ppm();
    if (!alert && r.co2 >= threshold) alert = 1;
    else if (alert && r.co2 < threshold - CO2_ALERT_HYSTERESIS) alert = 0;
    set_if_changed<uint8_t>(detected, alert);
  }
};

// A switch mirroring one of the device's own settings. Changes from the Home
// app are written to the device; changes made elsewhere (web page, API) flow
// back within a few seconds.
struct SettingSwitch : Service::Switch {
  SpanCharacteristic *on;
  bool (*get)(const device::Settings &);
  bool (*set)(bool);
  uint32_t last_sync = 0;

  SettingSwitch(const char *name, bool (*g)(const device::Settings &), bool (*s)(bool))
      : Service::Switch(), get(g), set(s) {
    on = new Characteristic::On(0);
    new Characteristic::Name(name);
  }

  boolean update() override { return set(on->getNewVal<bool>()); }

  void loop() override {
    if (millis() - last_sync < 3000) return;
    last_sync = millis();
    device::Settings s = device::settings();
    if (s.valid) set_if_changed<bool>(on, get(s));
  }
};

struct TemperatureSensor : Service::TemperatureSensor {
  SpanCharacteristic *temp, *fault;
  uint32_t seen = 0;

  TemperatureSensor() : Service::TemperatureSensor() {
    temp = (new Characteristic::CurrentTemperature(20))->setRange(-40, 100);
    fault = new Characteristic::StatusFault(0);
  }

  void loop() override {
    Reading r = gd32::reading();
    Freshness f = check(r);
    set_if_changed<uint8_t>(fault, f.fault ? 1 : 0);
    if (!f.usable || r.millis_at == seen) return;
    seen = r.millis_at;
    set_if_changed<float>(temp, (float)r.temperature);
  }
};

struct HumiditySensor : Service::HumiditySensor {
  SpanCharacteristic *rh, *fault;
  uint32_t seen = 0;

  HumiditySensor() : Service::HumiditySensor() {
    rh = new Characteristic::CurrentRelativeHumidity(50);
    fault = new Characteristic::StatusFault(0);
  }

  void loop() override {
    Reading r = gd32::reading();
    Freshness f = check(r);
    set_if_changed<uint8_t>(fault, f.fault ? 1 : 0);
    if (!f.usable || r.millis_at == seen) return;
    seen = r.millis_at;
    set_if_changed<float>(rh, (float)r.humidity);
  }
};

struct Battery : Service::BatteryService {
  SpanCharacteristic *level, *charging, *low;
  uint32_t seen = 0;

  Battery() : Service::BatteryService() {
    level = new Characteristic::BatteryLevel(100);
    charging = new Characteristic::ChargingState(0);
    low = new Characteristic::StatusLowBattery(0);
  }

  void loop() override {
    Reading r = gd32::reading();
    if (!check(r).usable || r.millis_at == seen) return;
    seen = r.millis_at;
    // The device reports 0-4 bars.
    set_if_changed<uint8_t>(level, (uint8_t)min(r.battery * 25, 100));
    set_if_changed<uint8_t>(charging, r.charging ? 1 : 0);
    set_if_changed<uint8_t>(low, r.battery <= 1 ? 1 : 0);
  }
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
namespace homekit {

String setup_code() {
  String c = raw_code();
  return c.substring(0, 3) + "-" + c.substring(3, 5) + "-" + c.substring(5);
}

bool network_ready() { return wifi_up; }

uint32_t setup_pin() { return pin6(); }

static bool started = false;

bool paired() { return started && homeSpan.controllerListBegin() != homeSpan.controllerListEnd(); }

void begin(const String &ssid, const String &pass, const String &serial, const String &ota_pass) {
#ifndef HTRAM_HOMESPAN_LOG
#define HTRAM_HOMESPAN_LOG 0
#endif
  homeSpan.setLogLevel(HTRAM_HOMESPAN_LOG);  // 1-2 log every HomeKit connection to UART0
  // UART0 is wired to whatever was used to flash this module; never let stray
  // bytes there reach HomeSpan's console, which has commands that erase data.
  homeSpan.setSerialInputDisable(true);
  homeSpan.setHostNameSuffix("");
  homeSpan.setPortNum(HAP_PORT);
  homeSpan.setSketchVersion("htram-fw " HTRAM_FW_VERSION);
  homeSpan.setWifiCredentials(ssid.c_str(), pass.c_str());
  homeSpan.setWifiCallback([]() { wifi_up = true; });
  static String ota;  // HomeSpan keeps the pointer
  ota = ota_pass;
  homeSpan.enableOTA(ota.c_str());

  homeSpan.begin(Category::Sensors, "HTRAM CO2 Monitor", HOSTNAME, "HTRAM-V1-W");
  started = true;

  // Set the setup code once per device; it takes a few seconds to compute.
  Preferences prefs;
  prefs.begin("htram", false);
  String code = raw_code();
  if (prefs.getString("hk_code", "") != code) {
    homeSpan.setPairingCode(code.c_str());
    prefs.putString("hk_code", code);
  }
  prefs.end();

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("HTRAM CO2 Monitor");
  new Characteristic::Manufacturer("Honeywell");
  new Characteristic::Model("HTRAM-V1-W");
  new Characteristic::SerialNumber(serial.c_str());
  new Characteristic::FirmwareRevision(HTRAM_FW_VERSION);

  new Co2Sensor();
  new TemperatureSensor();
  new HumiditySensor();
  new Battery();
  new SettingSwitch(
      "Alarm Sound", [](const device::Settings &s) { return s.alarm_sound; },
      [](bool on) { return device::set_alarm_sound(on); });
  new SettingSwitch(
      "Display Auto-Off", [](const device::Settings &s) { return s.screen_off_s > 0; },
      [](bool on) { return device::set_screen_off(on ? 120 : 0); });

  homeSpan.autoPoll();
}

}  // namespace homekit
