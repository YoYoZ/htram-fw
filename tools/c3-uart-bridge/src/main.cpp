#ifndef PROBE_PINS
#include <Arduino.h>
#include <Preferences.h>
#include "driver/uart.h"

#ifndef BRIDGE_BAUD
#define BRIDGE_BAUD 115200
#endif

// USB side uses Arduino's HWCDC (the IDF 4.4 usb_serial_jtag driver doesn't
// flush short packets). UART side uses the IDF driver directly: Arduino's
// HardwareSerial hit an interrupt-watchdog panic when the target's TX dropped.

// Boot mode control: wire C3 pin 5 to the target's IO0, and optionally pin 6 to
// its EN. Both are driven open-drain (pulled low to assert, released to
// high-Z), so they never fight the target board's own pull-ups.
//
// A control sequence arriving on USB selects the mode; the bytes are still
// passed through to the target, which is harmless.
//   "<<HTRAM-BOOT>>"  hold IO0 low, so the target comes up in download mode
//   "<<HTRAM-RUN>>"   release IO0, so it comes up running its firmware
//
// IO0 latches and stays as set, because the target is normally power-cycled by
// hand. The setting is kept in flash: opening the C3's USB port can reset the
// C3 itself (its built-in USB serial resets on DTR/RTS), and the latch has to
// survive that. With EN wired as well, each sequence also resets the target
// and the power cycle is unnecessary.
static const int PIN_IO0 = 5;
static const int PIN_EN = 6;
static const char *MAGIC_BOOT = "<<HTRAM-BOOT>>";
static const char *MAGIC_RUN = "<<HTRAM-RUN>>";
static const char *MAGIC_STATUS = "<<HTRAM-STATUS>>";  // reports the pin state on USB
static uint32_t control_hits = 0;

// Strongest drive the C3 has: on the HTRAM, IO0 is held high by something
// stiffer than a plain pull-up, and default drive strength loses to it.
static void assert_low(int pin) {
  gpio_num_t g = (gpio_num_t)pin;
  gpio_reset_pin(g);
  gpio_set_drive_capability(g, GPIO_DRIVE_CAP_3);
  gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT);  // keep the input so status can read the pad
  gpio_set_level(g, 0);
}

static void release(int pin) {
  gpio_num_t g = (gpio_num_t)pin;
  gpio_reset_pin(g);
  gpio_set_direction(g, GPIO_MODE_INPUT);
  gpio_set_pull_mode(g, GPIO_FLOATING);
}

static Preferences prefs;

static void apply_io0(bool download_mode) {
  if (download_mode) assert_low(PIN_IO0);
  else release(PIN_IO0);
}

static void select_boot_mode(bool download_mode) {
  apply_io0(download_mode);
  prefs.putBool("io0_low", download_mode);

  // Only does anything if EN is wired; otherwise power-cycle the target.
  assert_low(PIN_EN);
  delay(100);
  release(PIN_EN);
}

// SuperMini pin labels: 20 = receive from target, 21 = transmit to target,
// onboard LED = GPIO8 (active low)
#ifdef SWAP_PINS
static const int PIN_RX = 21;
static const int PIN_TX = 20;
#else
static const int PIN_RX = 20;
static const int PIN_TX = 21;
#endif
static const int PIN_LED = 8;
static const uart_port_t PORT = UART_NUM_1;

void setup() {
  // esptool sends 16 KB flash blocks in one go, far faster than the UART can
  // drain them. HWCDC drops whatever overflows its receive buffer, so the UART
  // transmit buffer below must hold a whole block to keep loop() from stalling.
  Serial.setRxBufferSize(8192);
  Serial.begin();

  uart_config_t cfg = {};
  cfg.baud_rate = BRIDGE_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_APB;
  uart_driver_install(PORT, 16384, 40960, 0, NULL, 0);
  uart_param_config(PORT, &cfg);
  uart_set_pin(PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  gpio_pullup_en((gpio_num_t)PIN_RX);

  release(PIN_EN);
  prefs.begin("bridge", false);
  apply_io0(prefs.getBool("io0_low", false));

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
}

// Watches the USB->target stream for a control sequence, one byte at a time.
// Advances one matcher by a byte; returns true when the whole sequence matched.
static bool match(const char *magic, size_t &at, uint8_t c) {
  at = (c == (uint8_t)magic[at]) ? at + 1 : (c == (uint8_t)magic[0] ? 1 : 0);
  if (magic[at]) return false;
  at = 0;
  return true;
}

static void watch_for_control(uint8_t c) {
  static size_t boot_at = 0, run_at = 0, status_at = 0;
  if (match(MAGIC_BOOT, boot_at, c)) {
    control_hits++;
    select_boot_mode(true);
  }
  if (match(MAGIC_RUN, run_at, c)) {
    control_hits++;
    select_boot_mode(false);
  }
  if (match(MAGIC_STATUS, status_at, c)) {
    Serial.printf("\r\n[bridge] io0_low=%d gpio%d_reads=%d control_hits=%lu\r\n",
                  prefs.getBool("io0_low", false), PIN_IO0, digitalRead(PIN_IO0),
                  (unsigned long)control_hits);
  }
}

void loop() {
  static uint8_t buf[1024];
  bool active = false;

  int n = uart_read_bytes(PORT, buf, sizeof(buf), 0);
  if (n > 0) {
    Serial.write(buf, n);
    active = true;
  }

  n = Serial.available();
  if (n > 0) {
    n = Serial.read(buf, min((size_t)n, sizeof(buf)));
    uart_write_bytes(PORT, buf, n);
    for (int i = 0; i < n; i++) watch_for_control(buf[i]);
    active = true;
  }

  digitalWrite(PIN_LED, active ? LOW : HIGH);
  if (!active) vTaskDelay(1);
}
#endif
