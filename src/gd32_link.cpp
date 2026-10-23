#include "gd32_link.h"

// ---------------------------------------------------------------------------
// Serial link
//
// Same pins and settings the stock ESP-AT build used, read back out of its
// flash and confirmed on a logic capture of a running device:
//   GPIO16 (module pin 27)  GD32 -> ESP32
//   GPIO17 (module pin 28)  ESP32 -> GD32
//   115200 8N1
//
// The stock build also had RTS flow control enabled. Nothing observed so far
// needs it, so it stays off; set HTRAM_RTS_PIN if a board turns out to stall
// without it.
// ---------------------------------------------------------------------------
#ifndef HTRAM_RX_PIN
#define HTRAM_RX_PIN 16
#endif
#ifndef HTRAM_TX_PIN
#define HTRAM_TX_PIN 17
#endif
#ifndef HTRAM_RTS_PIN
#define HTRAM_RTS_PIN -1
#endif

static HardwareSerial &AT = Serial2;

// ---------------------------------------------------------------------------
// Device protocol
//
// Frames the GD32 exchanges with a Bluetooth client look like:
//
//   7b 41 00 LL CH CL ... payload ... RH RL 7d
//   |  |     |  |         |            |    `- end marker
//   |  |     |  |         |            `- CRC-16/BUYPASS over everything before it
//   |  |     |  `- command, e.g. 4044 "give me a reading", 4144 "here it is"
//   |  |     `- number of bytes that follow this one
//   `--`- start marker and protocol version
//
// A reading comes back as command 4144 with six bytes of payload. Offsets are
// taken from the manufacturer's own app, which parsed them the same way.
// ---------------------------------------------------------------------------
static const uint8_t READ_REQUEST[] = {0x7b, 0x41, 0x00, 0x07, 0x40, 0x44,
                                       0x02, 0x00, 0xfc, 0x3e, 0x7d};
static const uint16_t CMD_READING = 0x4144;
static const int CO2_WARMING_UP = 65534;

static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t c = 0;
  for (size_t i = 0; i < len; i++) {
    c ^= (uint16_t)data[i] << 8;
    for (int bit = 0; bit < 8; bit++) c = (c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1);
  }
  return c;
}

// ---------------------------------------------------------------------------
// Standing in for the phone
//
// The GD32 will not volunteer a reading. It waits for a Bluetooth client to
// write the request frame to its "Write-Data" characteristic, which reaches it
// as an AT event. So after answering its boot sequence we report a client that
// connected, paired and subscribed, and then send it that event on a timer.
// The addresses and handles below are the ones a real phone produced.
// ---------------------------------------------------------------------------
static const char *CLIENT_ADDR = "\"02:00:5e:10:00:01\"";  // locally administered
static const uint32_t POLL_INTERVAL_MS = 5000;
static const uint32_t REPLY_TIMEOUT_MS = 45000;   // quiet this long -> resubscribe
static const uint32_t RELINK_TIMEOUT_MS = 90000;  // still quiet -> reconnect the client
static const uint32_t BINARY_TIMEOUT_MS = 500;
// The GD32 runs its setup only when it powers the ESP32 on, and ignores a
// "ready" from a module that restarted on its own (Wi-Fi setup, OTA update).
// If no setup commands arrive this long after our "ready", it is already
// configured, so go straight to connecting a client.
static const uint32_t SETUP_WAIT_MS = 4000;

enum Phase { PHASE_BOOT, PHASE_ADVERTISING, PHASE_PAIRING, PHASE_READY };

static Phase phase = PHASE_BOOT;
static bool echo = true;         // ESP-AT starts with echo on; the GD32 sends ATE0
static uint32_t connect_at = 0;  // when to report the client connecting
static uint32_t pair_at = 0;     // when to report pairing finished
static uint32_t next_poll = 0;
static uint32_t last_reply = 0;
static uint32_t last_resubscribe = 0;
static uint32_t ready_sent_at = 0;
static uint32_t last_command_at = 0;

// Milliseconds since `then`, safe across millis() wrap and against `then`
// being a moment later than the caller's own idea of "now".
static int32_t since(uint32_t then) { return (int32_t)(millis() - then); }

static String line;              // AT command being collected
static uint8_t frame[64];        // notification payload being collected
static size_t frame_want = 0;    // bytes the GD32 said it would send
static size_t frame_got = 0;
static uint32_t frame_since = 0;

static Reading current;        // guarded by reading_lock; HomeKit reads it from its own task
static portMUX_TYPE reading_lock = portMUX_INITIALIZER_UNLOCKED;
static LinkStats counters;
static char advertised_name[40] = "";  // guarded by reading_lock

static void send(const char *s) { AT.print(s); }

static void send_ok() { AT.print("\r\nOK\r\n"); }

// Announces the modem, the way a freshly booted ESP-AT does. The GD32 takes
// this as its cue to run the whole setup sequence from the top.
static void announce_ready() {
  AT.print("\r\nready\r\n");
  phase = PHASE_BOOT;
  echo = true;
  connect_at = pair_at = 0;
  last_reply = ready_sent_at = millis();
  last_command_at = 0;
}

// Reports the client dropping off and, a moment later, connecting again.
// Used to recover a link that has gone quiet.
static void reconnect_client() {
  AT.printf("\r\n+BLEDISCONN:0,%s\r\n", CLIENT_ADDR);
  phase = PHASE_ADVERTISING;
  connect_at = millis() + 1500;
  last_reply = millis();
}

// Reports a client that connected, finished pairing and turned on notifications
// for the "Read-Data" characteristic.
static void report_client_subscribed() {
  AT.printf("\r\n+BLESECKEYTYPE:0,16\r\n");
  AT.printf("+BLESECKEYTYPE:0,32\r\n");
  AT.printf("+BLESECKEYTYPE:0,1\r\n");
  AT.printf("+BLESECKEYTYPE:0,2\r\n");
  AT.printf("+BLEAUTHCMPL:0,0\r\n");
  AT.printf("+BLECFGMTU:0,247\r\n");
  // Writing 01 00 to the descriptor is how a client says "notify me".
  AT.print("+WRITE:0,2,1,1,2,");
  AT.write(0x01);
  AT.write(0x00);
  AT.print("\r\n");
  phase = PHASE_READY;
  next_poll = millis();
  last_reply = millis();
  last_resubscribe = millis();
}

// Hands the GD32 a request frame as if the client had written it.
static void request_reading() {
  AT.printf("\r\n+WRITE:0,2,2,,%u,", (unsigned)sizeof(READ_REQUEST));
  AT.write(READ_REQUEST, sizeof(READ_REQUEST));
  AT.print("\r\n");
  counters.requests++;
}

// ---------------------------------------------------------------------------
// Reply parsing
// ---------------------------------------------------------------------------
static void parse_frame(const uint8_t *f, size_t n) {
  if (n < 10 || f[0] != 0x7b || f[n - 1] != 0x7d) {
    counters.bad_frames++;
    return;
  }
  uint16_t want = ((uint16_t)f[n - 3] << 8) | f[n - 2];
  if (crc16(f, n - 3) != want) {
    counters.bad_frames++;
    return;
  }
  uint16_t cmd = ((uint16_t)f[4] << 8) | f[5];
  if (cmd != CMD_READING || n < 16) return;  // some other reply; nothing to do

  counters.replies++;

  Reading r;
  r.valid = true;
  r.co2 = ((int)f[7] << 8) | f[8];
  r.warming_up = (r.co2 == CO2_WARMING_UP);
  r.temperature = f[9] > 0x80 ? (int)f[9] - 0x100 : f[9];
  r.humidity = f[10];
  r.battery = f[11];
  r.charging = f[12] != 0;
  r.millis_at = millis();
  if (r.warming_up) r.co2 = 0;
  portENTER_CRITICAL(&reading_lock);
  current = r;
  portEXIT_CRITICAL(&reading_lock);
  last_reply = millis();
}

// ---------------------------------------------------------------------------
// AT commands
// ---------------------------------------------------------------------------
static bool starts_with(const String &s, const char *p) { return s.startsWith(p); }

static void handle_at(const String &cmd) {
  counters.at_commands++;
  last_command_at = millis();
  if (echo) {
    AT.print(cmd);
    AT.print("\r\n");
  }

  if (cmd == "ATE0") {
    echo = false;
    send_ok();
  } else if (cmd == "ATE1") {
    echo = true;
    send_ok();
  } else if (starts_with(cmd, "AT+BLECFGMTU?")) {
    send("\r\n+BLECFGMTU:0,23\r\n");
    send_ok();
  } else if (starts_with(cmd, "AT+BLEADVSTART")) {
    send_ok();
    // Give the GD32 a moment, then let a client turn up.
    if (phase == PHASE_BOOT) {
      phase = PHASE_ADVERTISING;
      connect_at = millis() + 800;
    }
  } else if (starts_with(cmd, "AT+BLEENC")) {
    send_ok();
    pair_at = millis() + 250;
  } else if (starts_with(cmd, "AT+BLEGATTSNTFY")) {
    // "AT+BLEGATTSNTFY=<conn>,<srv>,<char>,<len>" then <len> raw bytes.
    int comma = cmd.lastIndexOf(',');
    long len = comma >= 0 ? cmd.substring(comma + 1).toInt() : 0;
    if (len > 0 && len <= (long)sizeof(frame)) {
      frame_want = len;
      frame_got = 0;
      frame_since = millis();
      send("\r\n>");  // ESP-AT's "go ahead, send the bytes" prompt
    } else {
      send("\r\nERROR\r\n");
    }
  } else if (starts_with(cmd, "AT+BLENAME=")) {
    // AT+BLENAME="HTRAM-RM12xxxxxxxx" - the unit's serial number, as advertised.
    String name = cmd.substring(11);
    name.replace("\"", "");
    portENTER_CRITICAL(&reading_lock);
    strlcpy(advertised_name, name.c_str(), sizeof(advertised_name));
    portEXIT_CRITICAL(&reading_lock);
    counters.unknown++;  // still only needs an OK
    send_ok();
  } else if (starts_with(cmd, "AT+GMR")) {
    send("\r\nAT version:2.1.0.0\r\nSDK version:v4.0.1\r\ncompile time:htram-fw\r\n");
    send_ok();
  } else if (starts_with(cmd, "AT+RST") || starts_with(cmd, "AT+RESTORE")) {
    send_ok();
    delay(50);
    announce_ready();
  } else if (cmd == "AT") {
    send_ok();
  } else {
    // Everything else in the boot sequence - radio mode, service setup,
    // advertising payload, device name, pairing parameters - only needs an
    // acknowledgement for the GD32 to carry on.
    counters.unknown++;
    send_ok();
  }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
static void service();

static void link_task(void *) {
  for (;;) {
    service();
    vTaskDelay(1);
  }
}

namespace gd32 {

void begin() {
  AT.setRxBufferSize(1024);  // must precede begin() to take effect
  AT.begin(115200, SERIAL_8N1, HTRAM_RX_PIN, HTRAM_TX_PIN);
#if HTRAM_RTS_PIN >= 0
  AT.setPins(-1, -1, -1, HTRAM_RTS_PIN);
  AT.setHwFlowCtrlMode(UART_HW_FLOWCTRL_RTS, 64);
#endif
  line.reserve(128);
  delay(100);
  announce_ready();
  xTaskCreatePinnedToCore(link_task, "gd32", 4096, nullptr, 2, nullptr, 1);
}

}  // namespace gd32

// One pass over the link. Runs only in link_task.
static void service() {
  // Collecting the bytes of a notification the GD32 is in the middle of sending.
  while (frame_want && AT.available()) {
    frame[frame_got++] = AT.read();
    if (frame_got >= frame_want) {
      parse_frame(frame, frame_got);
      frame_want = frame_got = 0;
      send_ok();
      break;
    }
  }
  if (frame_want && since(frame_since) > (int32_t)BINARY_TIMEOUT_MS) {
    frame_want = frame_got = 0;
    counters.bad_frames++;
    send("\r\nERROR\r\n");
  }

  // Otherwise we are reading AT command lines.
  while (!frame_want && AT.available()) {
    char c = AT.read();
    if (c == '\n') {
      line.trim();
      if (line.length()) handle_at(line);
      line = "";
      break;  // let the rest of loop() run between commands
    } else if (c != '\r') {
      if (line.length() < 120) line += c;
    }
  }

  // We restarted but the GD32 stayed up and will not redo its setup. Its setup
  // always starts with ATE0, so echo was off before we restarted.
  if (phase == PHASE_BOOT && !last_command_at && since(ready_sent_at) > (int32_t)SETUP_WAIT_MS) {
    echo = false;
    phase = PHASE_ADVERTISING;
    connect_at = millis();
  }

  if (phase == PHASE_ADVERTISING && connect_at && since(connect_at) >= 0) {
    connect_at = 0;
    AT.printf("\r\n+BLECONN:0,%s\r\n", CLIENT_ADDR);
    phase = PHASE_PAIRING;
    // The GD32 normally answers a connection by asking for encryption, which
    // schedules pair_at. Do not wait forever if it does not.
    pair_at = millis() + 3000;
  }

  if (phase == PHASE_PAIRING && pair_at && since(pair_at) >= 0) {
    pair_at = 0;
    report_client_subscribed();
  }

  if (phase == PHASE_READY && since(next_poll) >= 0) {
    next_poll = millis() + POLL_INTERVAL_MS;
    request_reading();
  }

  // Nothing coming back? Say the client subscribed again, and if that does not
  // help, have the client drop off and reconnect.
  if (phase == PHASE_READY && since(last_reply) > (int32_t)RELINK_TIMEOUT_MS) {
    reconnect_client();
  } else if (phase == PHASE_READY && since(last_reply) > (int32_t)REPLY_TIMEOUT_MS &&
             since(last_resubscribe) > (int32_t)REPLY_TIMEOUT_MS) {
    last_resubscribe = millis();
    report_client_subscribed();
  }
}

namespace gd32 {

Reading reading() {
  portENTER_CRITICAL(&reading_lock);
  Reading r = current;
  portEXIT_CRITICAL(&reading_lock);
  return r;
}

const LinkStats &stats() { return counters; }

String device_name() {
  char copy[sizeof(advertised_name)];
  portENTER_CRITICAL(&reading_lock);
  memcpy(copy, advertised_name, sizeof(copy));
  portEXIT_CRITICAL(&reading_lock);
  return String(copy);
}
bool online() { return phase == PHASE_READY; }

}  // namespace gd32
