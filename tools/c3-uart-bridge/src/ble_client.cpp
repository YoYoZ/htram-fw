#ifdef BLE_CLIENT
// BLE central for the HTRAM's Honeywell GATT service, run alongside the UART
// sniffer in probe.cpp. Connects to the first device named "HTRAM-*",
// subscribes to Read-Data / Binding-Status and prints everything as hex.
// USB commands (one per line):
//   w <hex>   write bytes to Write-Data
//   b <hex>   write bytes to Binding-Status
//   r         read Read-Data and Binding-Status
#include <Arduino.h>
#include <BLEDevice.h>

static BLEUUID SVC("fc247940-6e08-11e4-80fc-0002a5d5c51b");
static BLEUUID READ_DATA("f833d6c0-6e0b-11e4-9136-0002a5d5c51b");
static BLEUUID WRITE_DATA("3d115840-6e0b-11e4-b24f-0002a5d5c51b");
static BLEUUID BINDING("90178a02-5d4a-11e6-8b77-86f30ca893d3");

static BLEAdvertisedDevice *target = nullptr;
static BLEClient *client = nullptr;
static BLERemoteCharacteristic *ch_read = nullptr, *ch_write = nullptr, *ch_bind = nullptr;
static volatile bool connected = false;

static void print_hex(const char *tag, const uint8_t *d, size_t n) {
  Serial.printf("\n[%lu BLE %s] (%u) ", (unsigned long)millis(), tag, (unsigned)n);
  for (size_t i = 0; i < n; i++) Serial.printf("%02x ", d[i]);
  Serial.print(" | ");
  for (size_t i = 0; i < n; i++) Serial.write(d[i] >= 32 && d[i] < 127 ? d[i] : '.');
  Serial.println();
}

static void on_notify(BLERemoteCharacteristic *c, uint8_t *d, size_t n, bool) {
  print_hex(c->getUUID().equals(BINDING) ? "notify Binding" : "notify Read-Data", d, n);
}

class ClientCb : public BLEClientCallbacks {
  void onConnect(BLEClient *) override { Serial.printf("\n[%lu BLE] connected\n", (unsigned long)millis()); }
  void onDisconnect(BLEClient *) override {
    connected = false;
    Serial.printf("\n[%lu BLE] disconnected\n", (unsigned long)millis());
  }
};

class ScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (dev.haveName() && dev.getName().rfind("HTRAM-", 0) == 0) {
      Serial.printf("\n[%lu BLE] found %s %s rssi %d\n", (unsigned long)millis(), dev.getName().c_str(),
                    dev.getAddress().toString().c_str(), dev.getRSSI());
      BLEDevice::getScan()->stop();
      target = new BLEAdvertisedDevice(dev);
    }
  }
};

static void read_all() {
  if (!connected) return;
  for (auto *c : {ch_read, ch_bind}) {
    if (c && c->canRead()) {
      std::string v = c->readValue();
      print_hex(c == ch_read ? "read Read-Data" : "read Binding", (const uint8_t *)v.data(), v.size());
    }
  }
}

static bool connect_target() {
  client = BLEDevice::createClient();
  client->setClientCallbacks(new ClientCb());
  if (!client->connect(target)) return false;
  client->setMTU(247);
  BLERemoteService *svc = client->getService(SVC);
  if (!svc) {
    Serial.println("\n[BLE] Honeywell service not found");
    client->disconnect();
    return false;
  }
  ch_read = svc->getCharacteristic(READ_DATA);
  ch_write = svc->getCharacteristic(WRITE_DATA);
  ch_bind = svc->getCharacteristic(BINDING);
  for (auto *c : {ch_read, ch_bind})
    if (c && c->canNotify()) c->registerForNotify(on_notify);
  connected = true;
  read_all();
  return true;
}

static void write_hex(BLERemoteCharacteristic *c, const String &hex, const char *tag) {
  if (!connected || !c) {
    Serial.println("\n[BLE] not connected");
    return;
  }
  uint8_t buf[256];
  size_t n = 0;
  for (size_t i = 0; i + 1 < hex.length() && n < sizeof(buf);) {
    if (hex[i] == ' ') { i++; continue; }
    buf[n++] = strtoul(hex.substring(i, i + 2).c_str(), nullptr, 16);
    i += 2;
  }
  print_hex(tag, buf, n);
  c->writeValue(buf, n, true);
}

static void ble_task(void *) {
  BLEDevice::init("");
  for (;;) {
    if (!connected) {
      delete target;
      target = nullptr;
      BLEScan *scan = BLEDevice::getScan();
      scan->setAdvertisedDeviceCallbacks(new ScanCb());
      scan->setActiveScan(true);
      scan->start(5, false);
      scan->clearResults();
      if (target && !connect_target()) Serial.println("\n[BLE] connect failed, retrying");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void ble_client_start() { xTaskCreate(ble_task, "ble", 8192, nullptr, 1, nullptr); }

void ble_client_command(const String &line) {
  if (line.startsWith("w ")) write_hex(ch_write, line.substring(2), "write Write-Data");
  else if (line.startsWith("b ")) write_hex(ch_bind, line.substring(2), "write Binding");
  else if (line == "r") read_all();
}
#endif
