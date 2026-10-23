#include "homekit.h"

#include <HomeSpan.h>
#include <Preferences.h>

#include "gd32_link.h"

static const char *HOSTNAME = "htram";
// Change it for your build: -DHTRAM_OTA_PASS='"something-long"' in platformio.ini,
// and the same value in upload_flags for env:htram_ota.
#ifndef HTRAM_OTA_PASS
#define HTRAM_OTA_PASS "htram-ota"
#endif
static const char *OTA_PASS = HTRAM_OTA_PASS;
static const uint16_t HAP_PORT = 1201;  // port 80 stays with the JSON API

// "CO2 detected" turns on at this level and off again 100 ppm below it, so a
// reading hovering at the threshold does not flap. 1000 ppm is where the
// device's own display turns yellow.
#ifndef HTRAM_CO2_ALERT_PPM
#define HTRAM_CO2_ALERT_PPM 1000
#endif
static const int CO2_ALERT_HYSTERESIS = 100;

// A reading older than this marks the sensors as faulty in the Home app.
static const uint32_t STALE_MS = 60000;

static volatile bool wifi_up = false;

// ---------------------------------------------------------------------------
// Setup code
//
// HomeSpan ships with one fixed code for every device, which is a poor default
// for a firmware meant to be flashed onto many units. Derive one from the MAC
// address instead, skipping the codes HomeKit refuses as too simple.
// ---------------------------------------------------------------------------
static bool code_allowed(const char *c) {
  static const char *banned[] = {"00000000", "11111111", "22222222", "33333333",
                                 "44444444", "55555555", "66666666", "77777777",
                                 "88888888", "99999999", "12345678", "87654321"};
  for (const char *b : banned)
    if (!strcmp(c, b)) return false;
  return true;
}

static String raw_code() {
  uint64_t h = ESP.getEfuseMac() ^ 0x9E3779B97F4A7C15ULL;
  char c[9];
  do {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    snprintf(c, sizeof(c), "%08lu", (unsigned long)(h % 100000000ULL));
  } while (!code_allowed(c));
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
    if (!alert && r.co2 >= HTRAM_CO2_ALERT_PPM) alert = 1;
    else if (alert && r.co2 < HTRAM_CO2_ALERT_PPM - CO2_ALERT_HYSTERESIS) alert = 0;
    set_if_changed<uint8_t>(detected, alert);
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

void begin(const String &ssid, const String &pass, const String &serial) {
#ifndef HTRAM_HOMESPAN_LOG
#define HTRAM_HOMESPAN_LOG 0
#endif
  homeSpan.setLogLevel(HTRAM_HOMESPAN_LOG);  // 1-2 log every HomeKit connection to UART0
  // UART0 is wired to whatever was used to flash this module; never let stray
  // bytes there reach HomeSpan's console, which has commands that erase data.
  homeSpan.setSerialInputDisable(true);
  homeSpan.setHostNameSuffix("");
  homeSpan.setPortNum(HAP_PORT);
  homeSpan.setSketchVersion("htram-fw 1.1");
  homeSpan.setWifiCredentials(ssid.c_str(), pass.c_str());
  homeSpan.setWifiCallback([]() { wifi_up = true; });
  homeSpan.enableOTA(OTA_PASS);

  homeSpan.begin(Category::Sensors, "HTRAM CO2 Monitor", HOSTNAME, "HTRAM-V1-W");

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
  new Characteristic::FirmwareRevision("1.1");

  new Co2Sensor();
  new TemperatureSensor();
  new HumiditySensor();
  new Battery();

  homeSpan.autoPoll();
}

}  // namespace homekit
