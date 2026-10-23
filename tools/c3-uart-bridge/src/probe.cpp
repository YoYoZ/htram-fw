#ifdef PROBE_PINS
// Listen-only two-channel UART sniffer. Each UART only receives; nothing is
// ever transmitted to the target. Output to USB is tagged per channel with a
// millisecond timestamp, so both directions of a link can be read side by side.
#include <Arduino.h>
#include "driver/uart.h"

#ifdef BLE_CLIENT
void ble_client_start();
void ble_client_command(const String &line);
#endif

#ifndef PROBE_A_PIN
#define PROBE_A_PIN 20
#endif
#ifndef PROBE_B_PIN
#define PROBE_B_PIN 21
#endif
#ifndef PROBE_A_TAG
#define PROBE_A_TAG "pin20"
#endif
#ifndef PROBE_B_TAG
#define PROBE_B_TAG "pin21"
#endif
#ifndef PROBE_BAUD
#define PROBE_BAUD 115200
#endif

static void start_rx(uart_port_t port, int pin) {
  uart_config_t cfg = {};
  cfg.baud_rate = PROBE_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_APB;
  uart_driver_install(port, 8192, 0, 0, NULL, 0);
  uart_param_config(port, &cfg);
  uart_set_pin(port, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void setup() {
  Serial.begin();
  // Keep the default UART0 pins passive in case they are still wired to something.
  for (int pin : {20, 21}) {
    if (pin != PROBE_A_PIN && pin != PROBE_B_PIN) {
      gpio_reset_pin((gpio_num_t)pin);
      pinMode(pin, INPUT);
    }
  }
  start_rx(UART_NUM_0, PROBE_A_PIN);
  start_rx(UART_NUM_1, PROBE_B_PIN);
#ifdef BLE_CLIENT
  ble_client_start();
#endif
}

void loop() {
  static uint8_t buf[512];
  static uint32_t last = 0;
  static int last_ch = -1;
  bool active = false;
  const struct { uart_port_t port; const char *tag; } ports[] = {
      {UART_NUM_0, PROBE_A_TAG}, {UART_NUM_1, PROBE_B_TAG}};
  for (int i = 0; i < 2; i++) {
    int n = uart_read_bytes(ports[i].port, buf, sizeof(buf), 0);
    if (n > 0) {
      if (i != last_ch) {
        Serial.printf("\n[%lu %s] ", (unsigned long)millis(), ports[i].tag);
        last_ch = i;
      }
      Serial.write(buf, n);
      active = true;
    }
  }
#ifdef BLE_CLIENT
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) ble_client_command(line);
      line = "";
    } else {
      line += c;
    }
  }
#endif
  if (millis() - last > 10000) {
    last = millis();
    Serial.printf("\n[%lu levels] %s=%d %s=%d\n", (unsigned long)millis(), PROBE_A_TAG,
                  digitalRead(PROBE_A_PIN), PROBE_B_TAG, digitalRead(PROBE_B_PIN));
    last_ch = -1;
  }
  if (!active) vTaskDelay(1);
}
#endif
