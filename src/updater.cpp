#include "updater.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <sodium.h>

#include "update_key.h"
#include "version.h"

#ifndef HTRAM_UPDATE_REPO
#define HTRAM_UPDATE_REPO "YoYoZ/htram-fw"
#endif
// GitHub redirects this to the newest release's file, whatever its version.
// Test builds point HTRAM_UPDATE_MANIFEST at a local server instead.
#ifndef HTRAM_UPDATE_MANIFEST
#define HTRAM_UPDATE_MANIFEST \
  "https://github.com/" HTRAM_UPDATE_REPO "/releases/latest/download/htram-fw-ota.json"
#endif
static const char *MANIFEST_URL = HTRAM_UPDATE_MANIFEST;

static const uint32_t FIRST_CHECK_MS = 5 * 60 * 1000UL;  // after boot, plus up to an hour
static const uint32_t CHECK_EVERY_MS = 24 * 3600 * 1000UL;
static const uint32_t CONFIRM_WITHIN_MS = 5 * 60 * 1000UL;
static const uint32_t STALL_MS = 20000;

// Arduino confirms every newly installed image as soon as it boots, which
// leaves the bootloader nothing to roll back to. Confirm here instead, once
// the image has shown it works (see loop()).
extern "C" bool verifyRollbackLater() { return true; }

// ---------------------------------------------------------------------------
// State shared with the web pages
// ---------------------------------------------------------------------------
struct Status {
  String state = "idle";  // idle, checking, downloading, installing, error
  String latest;          // newest version the last check found
  String error;
  String skipped;         // a version that rolled back; not installed automatically
  int progress = -1;
  uint32_t checked_at = 0;
  bool available = false;
};
static Status st;
static SemaphoreHandle_t lock;
static volatile bool want_check = false, want_install = false;
static bool auto_on = true, started = false;

static void set_state(const char *s, const String &error = "") {
  xSemaphoreTake(lock, portMAX_DELAY);
  st.state = s;
  st.error = error;
  if (strcmp(s, "downloading")) st.progress = -1;
  xSemaphoreGive(lock);
  if (error.length()) Serial.printf("[update] %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
struct Manifest {
  String version, sha256, sig, url;
  uint32_t size = 0;
};

// Enough JSON for a flat object of strings and numbers that CI writes.
static String json_field(const String &j, const char *key) {
  String k = String("\"") + key + "\"";
  int i = j.indexOf(k);
  if (i < 0) return "";
  i = j.indexOf(':', i + k.length());
  if (i < 0) return "";
  do i++; while (i < (int)j.length() && j[i] == ' ');
  if (i < (int)j.length() && j[i] == '"') {
    int e = j.indexOf('"', i + 1);
    return e < 0 ? "" : j.substring(i + 1, e);
  }
  int e = i;
  while (e < (int)j.length() && isDigit(j[e])) e++;
  return j.substring(i, e);
}

static bool from_hex(const String &h, uint8_t *out, size_t n) {
  if (h.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = {h[2 * i], h[2 * i + 1], 0};
    char *end;
    out[i] = (uint8_t)strtoul(b, &end, 16);
    if (*end) return false;
  }
  return true;
}

// "1.10.0" > "1.9.3"
static bool newer(const String &a, const String &b) {
  int x[3] = {0}, y[3] = {0};
  sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
  sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; i++)
    if (x[i] != y[i]) return x[i] > y[i];
  return false;
}

// CI signs exactly these bytes; see .github/workflows/build.yml.
static String signed_message(const Manifest &m) {
  return "htram-fw-ota\n" + m.version + "\n" + String(m.size) + "\n" + m.sha256;
}

static bool signature_ok(const Manifest &m) {
  uint8_t sig[crypto_sign_BYTES];
  if (!from_hex(m.sig, sig, sizeof(sig))) return false;
  String msg = signed_message(m);
  return crypto_sign_verify_detached(sig, (const uint8_t *)msg.c_str(), msg.length(),
                                     UPDATE_PUBLIC_KEY) == 0;
}

// The download needs no trusted TLS: nothing is installed unless the
// signature over version, size and hash checks out against our public key.
// For the same reason plain http:// is accepted (test servers).
struct Clients {
  WiFiClientSecure tls;
  WiFiClient plain;
};

// The HTTP status, or a negative HTTPClient error when there was no answer.
static int http_get(HTTPClient &http, Clients &c, const String &url) {
  c.tls.setInsecure();
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("htram-fw/" HTRAM_FW_VERSION);
  WiFiClient &client = url.startsWith("https://") ? c.tls : c.plain;
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  return http.GET();
}

static bool fetch_manifest(Manifest &m, String &err) {
  Clients client;
  HTTPClient http;
  int code = http_get(http, client, MANIFEST_URL);
  if (code != HTTP_CODE_OK) {
    // 404: the newest published release predates updates (or has none yet).
    err = code == HTTP_CODE_NOT_FOUND ? String("the latest release has no update manifest")
          : code < 0                  ? "cannot reach GitHub (" + HTTPClient::errorToString(code) + ")"
                                      : "GitHub answered HTTP " + String(code);
    http.end();
    return false;
  }
  String j = http.getString();
  http.end();
  m.version = json_field(j, "version");
  m.size = json_field(j, "size").toInt();
  m.sha256 = json_field(j, "sha256");
  m.sig = json_field(j, "sig");
  m.url = json_field(j, "url");
  if (!m.version.length() || !m.size || !m.url.startsWith("http")) {
    err = "the release has no usable htram-fw-ota.json";
    return false;
  }
  if (!signature_ok(m)) {
    err = "signature check failed, ignoring release " + m.version;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Download and install
// ---------------------------------------------------------------------------
static void install(const Manifest &m) {
  static uint8_t buf[2048];
  Clients client;
  HTTPClient http;
  set_state("downloading");
  if (http_get(http, client, m.url) != HTTP_CODE_OK || http.getSize() != (int)m.size) {
    http.end();
    return set_state("error", "download of " + m.version + " failed");
  }
  if (!Update.begin(m.size, U_FLASH)) {
    http.end();
    return set_state("error", String("cannot start the update: ") + Update.errorString());
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient *s = http.getStreamPtr();
  uint32_t got = 0, last = millis();
  bool ok = true;
  while (got < m.size && ok) {
    size_t n = s->available();
    if (!n) {
      if (!http.connected() || millis() - last > STALL_MS) ok = false;
      delay(5);
      continue;
    }
    int r = s->readBytes(buf, min(n, sizeof(buf)));
    if (r <= 0) continue;
    mbedtls_sha256_update_ret(&sha, buf, r);
    if (Update.write(buf, r) != (size_t)r) ok = false;
    got += r;
    last = millis();
    xSemaphoreTake(lock, portMAX_DELAY);
    st.progress = (int)(100ULL * got / m.size);
    xSemaphoreGive(lock);
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
  if (!ok || got != m.size || m.sha256 != hex) {
    Update.abort();
    return set_state("error", ok ? "the download did not match its signature; nothing installed"
                                 : "the download stalled; nothing installed");
  }
  // Remember what is being tried, so the old image can tell if it rolls back.
  Preferences prefs;
  prefs.begin("htram", false);
  prefs.putString("upd_pending", m.version);
  prefs.end();
  if (!Update.end()) return set_state("error", String("install failed: ") + Update.errorString());
  set_state("installing");
  Serial.printf("[update] %s installed, restarting\n", m.version.c_str());
  delay(1500);  // let a waiting web page see it
  ESP.restart();
}

static void check(bool install_it, bool by_hand) {
  set_state("checking");
  Manifest m;
  String err;
  if (!fetch_manifest(m, err)) return set_state("error", err);
  xSemaphoreTake(lock, portMAX_DELAY);
  st.latest = m.version;
  st.available = newer(m.version, HTRAM_FW_VERSION);
  st.checked_at = millis();
  bool skip = !by_hand && m.version == st.skipped;
  xSemaphoreGive(lock);
  if (!newer(m.version, HTRAM_FW_VERSION) || !install_it || skip) return set_state("idle");
  install(m);
}

static void task(void *) {
  // Spread the fleet's daily checks over an hour.
  uint32_t next = millis() + FIRST_CHECK_MS + (uint32_t)(ESP.getEfuseMac() % 3600) * 1000UL;
  for (;;) {
    bool by_hand = want_check;
    if (by_hand || (int32_t)(millis() - next) >= 0) {
      bool install_it = by_hand ? want_install : auto_on;
      want_check = want_install = false;
      check(install_it, by_hand);
      next = millis() + CHECK_EVERY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
namespace updater {

void begin() {
  if (started) return;
  started = true;
  lock = xSemaphoreCreateMutex();
  if (sodium_init() < 0) Serial.println("[update] libsodium did not start");
  Preferences prefs;
  prefs.begin("htram", false);
  auto_on = prefs.getBool("upd_auto", true);
  // A version we tried that is not the one running now rolled back.
  String pending = prefs.getString("upd_pending", "");
  if (pending.length() && pending != HTRAM_FW_VERSION) {
    prefs.putString("upd_bad", pending);
    prefs.remove("upd_pending");
    Serial.printf("[update] %s rolled back to %s\n", pending.c_str(), HTRAM_FW_VERSION);
  }
  st.skipped = prefs.getString("upd_bad", "");
  prefs.end();
  // TLS needs the stack.
  xTaskCreate(task, "update", 12288, nullptr, 1, nullptr);
}

void loop(bool healthy) {
#ifdef HTRAM_TEST_NEVER_HEALTHY  // test build: must roll back by itself
  healthy = false;
#endif
  static int provisional = -1;  // -1 unknown, 0 no, 1 waiting for confirmation
  if (provisional == 0) return;
  if (provisional < 0) {
    esp_ota_img_states_t s;
    provisional = esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK &&
                  s == ESP_OTA_IMG_PENDING_VERIFY;
    if (provisional) Serial.println("[update] new image, waiting for it to prove itself");
    return;
  }
  if (healthy) {
    esp_ota_mark_app_valid_cancel_rollback();
    Preferences prefs;
    prefs.begin("htram", false);
    prefs.remove("upd_pending");
    prefs.end();
    provisional = 0;
    Serial.println("[update] new image confirmed");
  } else if (millis() > CONFIRM_WITHIN_MS) {
    Serial.println("[update] new image never came up properly, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void request(bool install_it) {
  want_install = install_it;
  want_check = true;
}

bool auto_install() { return auto_on; }

void set_auto_install(bool on) {
  auto_on = on;
  Preferences prefs;
  prefs.begin("htram", false);
  prefs.putBool("upd_auto", on);
  prefs.end();
}

String status_json() {
  if (!started) return "{\"state\":\"off\",\"current\":\"" HTRAM_FW_VERSION "\"}";
  xSemaphoreTake(lock, portMAX_DELAY);
  Status s = st;
  xSemaphoreGive(lock);
  String j = "{\"state\":\"" + s.state + "\",\"current\":\"" HTRAM_FW_VERSION "\"";
  // Whether the running image still has to prove itself (see loop()).
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t img;
  const char *image = esp_ota_get_state_partition(run, &img) != ESP_OK ? "unknown"
                      : img == ESP_OTA_IMG_PENDING_VERIFY              ? "provisional"
                      : img == ESP_OTA_IMG_VALID                       ? "confirmed"
                                                                       : "flashed over serial";
  j += ",\"image\":\"" + String(image) + "\",\"slot\":\"" + String(run->label) + "\"";
  j += ",\"latest\":" + (s.latest.length() ? "\"" + s.latest + "\"" : String("null"));
  j += ",\"available\":" + String(s.available ? "true" : "false");
  j += ",\"auto\":" + String(auto_on ? "true" : "false");
  if (s.progress >= 0) j += ",\"progress\":" + String(s.progress);
  if (s.checked_at) j += ",\"checked_seconds_ago\":" + String((millis() - s.checked_at) / 1000);
  if (s.skipped.length()) j += ",\"skipped\":\"" + s.skipped + "\"";
  if (s.error.length()) j += ",\"error\":\"" + s.error + "\"";
  return j + "}";
}

String status_html() {
  if (!started) return "";
  xSemaphoreTake(lock, portMAX_DELAY);
  Status s = st;
  xSemaphoreGive(lock);
  String h = "<div class=m>Firmware " HTRAM_FW_VERSION " &middot; ";
  if (s.state == "downloading") h += "downloading " + s.latest + ", " + String(max(s.progress, 0)) + " %";
  else if (s.state == "installing") h += "installing " + s.latest + ", restarting";
  else if (s.state == "checking") h += "checking for updates&hellip;";
  else if (s.available) h += "<b>" + s.latest + " available</b>";
  else if (s.checked_at) h += "up to date";
  else h += "not checked yet";
  if (s.state == "error") h += " &middot; " + s.error;
  bool busy = s.state == "checking" || s.state == "downloading" || s.state == "installing";
  if (!busy) {
    h += "<form method=POST action=/update style='display:inline;margin-left:.5rem'>";
    h += s.available ? "<button name=action value=install>Install now</button>"
                     : "<button name=action value=check>Check now</button>";
    h += "</form>";
  }
  return h + "</div>";
}

}  // namespace updater
