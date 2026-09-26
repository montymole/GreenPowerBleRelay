#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <string>
#include <vector>

#include "../include/config.h"
#include "../include/victron_ble.h"

static const char *SERVICE_UUID = "0000FFB0-0000-1000-8000-00805F9B34FB";
static const char *WRITE_UUID = "0000FFB1-0000-1000-8000-00805F9B34FB";
static const char *NOTIFY_UUID = "0000FFB2-0000-1000-8000-00805F9B34FB";

WebServer server(80);

BLEAdvertisedDevice *foundDevice = nullptr;
BLEClient *bleClient = nullptr;
BLERemoteCharacteristic *writeChar = nullptr;
BLERemoteCharacteristic *notifyChar = nullptr;

std::vector<uint8_t> rxBuf;
volatile bool bleConnected = false;
bool deviceReady = false;
bool transactionBusy = false;
uint8_t slaveAddress = 0x02;
String lastError;
String deviceName;
String deviceAddress;
uint32_t lastSeenMs = 0;
uint32_t lastWifiStatusLog = 0;
uint32_t lastBleRetryLog = 0;

static constexpr size_t MAX_CONFIGURED_DEVICES = 8;
static constexpr size_t MAX_DISCOVERED_DEVICES = 16;
static constexpr size_t MAX_RECENT_LOGS = 40;
static portMUX_TYPE deviceStateMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE recentLogMux = portMUX_INITIALIZER_UNLOCKED;

struct DeviceConfig {
  char id[13];
  char address[18];
  char name[32];
  uint8_t protocol;
  uint8_t key[16];
  bool hasKey;
  bool enabled;
};

struct DeviceState {
  char address[18];
  uint16_t productId;
  uint8_t recordType;
  uint16_t counter;
  uint32_t lastSeen;
  int rssi;
  uint8_t dataLength;
  uint8_t plaintext[16];
  bool decoded;
};

struct DiscoveredDevice {
  char address[18];
  char name[32];
  uint16_t productId;
  uint8_t protocol;
  uint8_t recordType;
  int rssi;
  uint32_t lastSeen;
  uint8_t manufacturerDataLength;
  char manufacturerDataHex[65];
  bool manufacturerDataTruncated;
  bool hasGreenPowerService;
};

struct RecentLogEntry {
  uint32_t uptimeMs;
  char message[128];
};

DeviceConfig configuredDevices[MAX_CONFIGURED_DEVICES] = {};
DeviceState deviceStates[MAX_CONFIGURED_DEVICES] = {};
DiscoveredDevice discoveredDevices[MAX_DISCOVERED_DEVICES] = {};
size_t discoveredCount = 0;
RecentLogEntry recentLogs[MAX_RECENT_LOGS] = {};
size_t recentLogNext = 0;
size_t recentLogCount = 0;
volatile uint32_t scanAdvertisementCount = 0;
Preferences devicePreferences;
bool discoveryRunning = false;
uint32_t discoveryStartedAt = 0;
bool connectScanActive = false;
uint32_t lastAdvertisementScan = 0;
uint32_t lastVictronAdvertisementLog = 0;
uint32_t lastAnyAdvertisementLog = 0;
String requestedConnectAddress;

struct TxResult {
  bool ok = false;
  String error;
  std::vector<uint8_t> response;
  std::vector<uint16_t> registers;
};

static const char *wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL: return "no_ssid";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    case WL_CONNECTED: return "connected";
    default: return "other";
  }
}

static void logState(const char *state, const String &detail = "") {
  const uint32_t now = millis();
  String message = String("[STATE] ") + state;
  if (detail.length()) message += " - " + detail;
  char storedMessage[sizeof(recentLogs[0].message)] = {};
  message.toCharArray(storedMessage, sizeof(storedMessage));

  portENTER_CRITICAL(&recentLogMux);
  recentLogs[recentLogNext].uptimeMs = now;
  memcpy(recentLogs[recentLogNext].message, storedMessage, sizeof(storedMessage));
  recentLogNext = (recentLogNext + 1) % MAX_RECENT_LOGS;
  if (recentLogCount < MAX_RECENT_LOGS) ++recentLogCount;
  portEXIT_CRITICAL(&recentLogMux);

  Serial.print("[STATE] ");
  Serial.print(state);
  if (detail.length()) {
    Serial.print(" - ");
    Serial.print(detail);
  }
  Serial.println();
}

static String hexByte(uint8_t b) {
  char out[3];
  snprintf(out, sizeof(out), "%02X", b);
  return String(out);
}

static String bytesToHex(const std::vector<uint8_t> &bytes) {
  String s;
  for (uint8_t b : bytes) s += hexByte(b);
  return s;
}

static void copyText(char *target, size_t capacity, const String &value) {
  if (!capacity) return;
  snprintf(target, capacity, "%s", value.c_str());
}

static uint8_t hexNibble(char c);

static String normalizedAddress(String address) {
  address.toLowerCase();
  address.replace("-", ":");
  if (address.length() == 12) {
    String formatted;
    for (size_t i = 0; i < 12; i += 2) {
      if (i) formatted += ":";
      formatted += address.substring(i, i + 2);
    }
    address = formatted;
  }
  if (address.length() != 17) return "";
  for (size_t i = 0; i < address.length(); ++i) {
    if (i % 3 == 2) {
      if (address[i] != ':') return "";
    } else if (hexNibble(address[i]) == 0xff) {
      return "";
    }
  }
  return address;
}

static String deviceIdForAddress(const String &address) {
  String id = address;
  id.replace(":", "");
  id.toLowerCase();
  return id;
}

static int configuredDeviceIndex(const String &id) {
  for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
    if (configuredDevices[i].enabled && id.equalsIgnoreCase(configuredDevices[i].id)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

static void loadDeviceRegistry() {
  if (!devicePreferences.begin("ble-devices", false)) {
    Serial.println("[NVS] Unable to open BLE device registry");
    return;
  }
  const size_t expected = sizeof(configuredDevices);
  if (devicePreferences.getUInt("schema", 0) == 1 &&
      devicePreferences.getBytesLength("registry") == expected) {
    devicePreferences.getBytes("registry", configuredDevices, expected);
  }
  devicePreferences.end();
}

static bool saveDeviceRegistry() {
  if (!devicePreferences.begin("ble-devices", false)) return false;
  const size_t written = devicePreferences.putBytes("registry", configuredDevices,
                                                    sizeof(configuredDevices));
  const size_t schemaWritten = devicePreferences.putUInt("schema", 1);
  devicePreferences.end();
  return written == sizeof(configuredDevices) && schemaWritten == sizeof(uint32_t);
}

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0xff;
}

static bool parseU16(const String &s, uint16_t &out) {
  char *end = nullptr;
  unsigned long v = strtoul(s.c_str(), &end, 0);
  if (!end || *end != '\0' || v > 0xffff) return false;
  out = (uint16_t)v;
  return true;
}

static bool parseValueBody(const String &body, uint16_t &value) {
  int p = body.indexOf("value");
  if (p < 0) return false;
  p = body.indexOf(':', p);
  if (p < 0) return false;
  while (++p < (int)body.length() && isspace(body[p])) {}
  int start = p;
  while (p < (int)body.length() && isdigit(body[p])) p++;
  if (p == start) return false;
  unsigned long v = strtoul(body.substring(start, p).c_str(), nullptr, 10);
  if (v > 0xffff) return false;
  value = (uint16_t)v;
  return true;
}

static uint16_t crc16Modbus(const uint8_t *data, size_t len) {
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
  }
  return crc;
}

static bool crcOk(const std::vector<uint8_t> &frame) {
  if (frame.size() < 4) return false;
  uint16_t got = frame[frame.size() - 2] | (frame[frame.size() - 1] << 8);
  uint16_t want = crc16Modbus(frame.data(), frame.size() - 2);
  return got == want;
}

static std::vector<uint8_t> buildRead(uint8_t slave, uint16_t address, uint16_t count) {
  std::vector<uint8_t> f = {
    slave, 0x03,
    (uint8_t)(address >> 8), (uint8_t)address,
    (uint8_t)(count >> 8), (uint8_t)count
  };
  uint16_t crc = crc16Modbus(f.data(), f.size());
  f.push_back(crc & 0xff);
  f.push_back(crc >> 8);
  return f;
}

static std::vector<uint8_t> buildWrite(uint8_t slave, uint16_t address, uint16_t value) {
  std::vector<uint8_t> f = {
    slave, 0x06,
    (uint8_t)(address >> 8), (uint8_t)address,
    (uint8_t)(value >> 8), (uint8_t)value
  };
  uint16_t crc = crc16Modbus(f.data(), f.size());
  f.push_back(crc & 0xff);
  f.push_back(crc >> 8);
  return f;
}

static void notifyCallback(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) rxBuf.push_back(data[i]);
  lastSeenMs = millis();
}

template <typename Data>
static void encodeManufacturerData(const Data &data, size_t length, char (&hexOutput)[65],
                                   bool &truncated) {
  static const char hex[] = "0123456789ABCDEF";
  const size_t bytesToEncode = length < 32 ? length : 32;
  for (size_t i = 0; i < bytesToEncode; ++i) {
    const uint8_t value = static_cast<uint8_t>(data[i]);
    hexOutput[i * 2] = hex[value >> 4];
    hexOutput[i * 2 + 1] = hex[value & 0x0f];
  }
  hexOutput[bytesToEncode * 2] = '\0';
  truncated = length > bytesToEncode;
}

template <typename Data>
static void recordDiscoveredDevice(const String &address, const String &name,
                                   uint8_t protocol, uint8_t recordType, int rssi,
                                   size_t manufacturerDataLength, bool hasGreenPowerService,
                                   uint16_t productId, const Data &manufacturerData) {
  const String normalized = normalizedAddress(address);
  if (normalized.isEmpty()) return;
  char addressText[18] = {};
  char nameText[32] = {};
  copyText(addressText, sizeof(addressText), normalized);
  copyText(nameText, sizeof(nameText), name);

  portENTER_CRITICAL(&deviceStateMux);
  size_t slot = discoveredCount;
  for (size_t i = 0; i < discoveredCount; ++i) {
    if (strcmp(addressText, discoveredDevices[i].address) == 0) {
      slot = i;
      break;
    }
  }
  if (slot == discoveredCount) {
    if (discoveredCount < MAX_DISCOVERED_DEVICES) {
      ++discoveredCount;
    } else {
      slot = 0;
      for (size_t i = 1; i < discoveredCount; ++i) {
        if (discoveredDevices[i].lastSeen < discoveredDevices[slot].lastSeen) slot = i;
      }
    }
  }
  memcpy(discoveredDevices[slot].address, addressText, sizeof(addressText));
  memcpy(discoveredDevices[slot].name, nameText, sizeof(nameText));
  discoveredDevices[slot].productId = productId;
  discoveredDevices[slot].protocol = protocol;
  discoveredDevices[slot].recordType = recordType;
  discoveredDevices[slot].rssi = rssi;
  discoveredDevices[slot].lastSeen = millis();
  discoveredDevices[slot].manufacturerDataLength =
      static_cast<uint8_t>(manufacturerDataLength > 255 ? 255 : manufacturerDataLength);
  encodeManufacturerData(manufacturerData, manufacturerDataLength,
                         discoveredDevices[slot].manufacturerDataHex,
                         discoveredDevices[slot].manufacturerDataTruncated);
  discoveredDevices[slot].hasGreenPowerService = hasGreenPowerService;
  portEXIT_CRITICAL(&deviceStateMux);
}

static size_t manufacturerDataSize(const std::string &data) {
  return data.size();
}

static size_t manufacturerDataSize(const String &data) {
  return data.length();
}

static void captureAdvertisement(BLEAdvertisedDevice &advertised) {
  const String address = advertised.getAddress().toString().c_str();
  const String name = advertised.haveName() ? advertised.getName().c_str() : "";
  const bool hasGreenPower = advertised.haveServiceUUID() &&
      advertised.isAdvertisingService(BLEUUID(SERVICE_UUID));
  const auto manufacturerData = advertised.getManufacturerData();
  const size_t manufacturerLength = advertised.haveManufacturerData()
      ? manufacturerDataSize(manufacturerData) : 0;
  uint8_t protocol = hasGreenPower ? 1 : 0;
  uint8_t recordType = 0xff;
  uint16_t productId = 0;
  ++scanAdvertisementCount;

  if (manufacturerLength) {
    VictronAdvertisement victron;
    if (parseVictronAdvertisement(manufacturerData, victron)) {
      protocol = 2;
      productId = victron.productId;
      recordType = victron.recordType;
      if (lastVictronAdvertisementLog == 0 ||
          millis() - lastVictronAdvertisementLog >= 10000) {
        lastVictronAdvertisementLog = millis();
        logState("victron_advertisement",
                 address + " product=" + victronProductName(productId) +
                     " record=" + victronRecordName(recordType) +
                     " rssi=" + String(advertised.getRSSI()));
      }

      DeviceConfig configCopy[MAX_CONFIGURED_DEVICES];
      portENTER_CRITICAL(&deviceStateMux);
      memcpy(configCopy, configuredDevices, sizeof(configCopy));
      portEXIT_CRITICAL(&deviceStateMux);
      for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
        if (!configCopy[i].enabled || configCopy[i].protocol != 2 ||
            normalizedAddress(address) != configCopy[i].address) continue;

        std::vector<uint8_t> plaintext;
        const bool decoded = configCopy[i].hasKey &&
            decryptVictronAdvertisement(victron, configCopy[i].key, plaintext);
        char addressText[18] = {};
        copyText(addressText, sizeof(addressText), address);
        portENTER_CRITICAL(&deviceStateMux);
        memcpy(deviceStates[i].address, addressText, sizeof(addressText));
        deviceStates[i].productId = victron.productId;
        deviceStates[i].recordType = victron.recordType;
        deviceStates[i].counter = victron.counter;
        deviceStates[i].lastSeen = millis();
        deviceStates[i].rssi = advertised.getRSSI();
        deviceStates[i].decoded = decoded;
        deviceStates[i].dataLength = decoded ? static_cast<uint8_t>(plaintext.size()) : 0;
        if (decoded) memcpy(deviceStates[i].plaintext, plaintext.data(), plaintext.size());
        portEXIT_CRITICAL(&deviceStateMux);
        break;
      }
    }
  }

  recordDiscoveredDevice(address, name, protocol, recordType, advertised.getRSSI(),
                         manufacturerLength, hasGreenPower, productId, manufacturerData);

  if (lastAnyAdvertisementLog == 0 || millis() - lastAnyAdvertisementLog >= 10000) {
    lastAnyAdvertisementLog = millis();
    logState("ble_advertisement_seen",
             (name.isEmpty() ? String("<unnamed>") : name) + " " + address +
                 " rssi=" + String(advertised.getRSSI()));
  }
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    captureAdvertisement(d);
    bool hasService = d.haveServiceUUID() && d.isAdvertisingService(BLEUUID(SERVICE_UUID));
    String advertisedName = d.haveName() ? String(d.getName().c_str()) : String();
    bool nameOk = strlen(DEVICE_NAME_HINT) == 0 ||
                  (d.haveName() && advertisedName.indexOf(DEVICE_NAME_HINT) >= 0);
    const bool targetOk = requestedConnectAddress.isEmpty() ||
        normalizedAddress(d.getAddress().toString().c_str()) == requestedConnectAddress;
    if (connectScanActive && hasService && nameOk && targetOk) {
      if (foundDevice) delete foundDevice;
      foundDevice = new BLEAdvertisedDevice(d);
      Serial.print("[BLE] Found matching device: ");
      Serial.print(advertisedName.length() ? advertisedName : "<unnamed>");
      Serial.print(" @ ");
      Serial.println(d.getAddress().toString().c_str());
      BLEDevice::getScan()->stop();
    }
  }
};

static ScanCallbacks scanCallbacks;

static void advertisementScanComplete(BLEScanResults) {
  discoveryRunning = false;
  logState("ble_scan_complete", "advertisements=" + String(scanAdvertisementCount));
}

static bool startAdvertisementScan(uint32_t durationSeconds) {
  BLEScan *scan = BLEDevice::getScan();
  if (discoveryRunning || connectScanActive) return false;
  scanAdvertisementCount = 0;
  logState("ble_scan_start", "duration=" + String(durationSeconds) + "s");
  discoveryRunning = scan->start(durationSeconds, advertisementScanComplete, false);
  discoveryStartedAt = millis();
  if (!discoveryRunning) logState("ble_scan_failed", "start_failed");
  return discoveryRunning;
}

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {
    bleConnected = true;
    logState("ble_connected");
  }
  void onDisconnect(BLEClient *) override {
    bleConnected = false;
    deviceReady = false;
    writeChar = nullptr;
    notifyChar = nullptr;
    lastError = "ble_disconnected";
    logState("ble_disconnected");
  }
};

static bool connectBle() {
  logState("ble_scan_start", "timeout=10s");
  deviceReady = false;
  lastError = "";
  rxBuf.clear();

  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  writeChar = nullptr;
  notifyChar = nullptr;

  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);
  if (discoveryRunning) {
    scan->stop();
    discoveryRunning = false;
  }
  if (foundDevice) {
    delete foundDevice;
    foundDevice = nullptr;
  }

  connectScanActive = true;
  scan->start(10, false);
  connectScanActive = false;
  scan->clearResults();
  if (!foundDevice) {
    lastError = "device_not_found";
    logState("ble_scan_failed", lastError);
    return false;
  }

  deviceName = foundDevice->haveName() ? foundDevice->getName().c_str() : "";
  deviceAddress = foundDevice->getAddress().toString().c_str();

  bleClient = BLEDevice::createClient();
  bleClient->setClientCallbacks(new ClientCallbacks());
  logState("ble_connect_start", deviceAddress);
  if (!bleClient->connect(foundDevice)) {
    lastError = "connect_failed";
    logState("ble_connect_failed", lastError);
    return false;
  }
  bleConnected = true;

  BLERemoteService *service = bleClient->getService(BLEUUID(SERVICE_UUID));
  if (!service) {
    lastError = "service_missing";
    logState("ble_setup_failed", lastError);
    return false;
  }

  writeChar = service->getCharacteristic(BLEUUID(WRITE_UUID));
  notifyChar = service->getCharacteristic(BLEUUID(NOTIFY_UUID));
  if (!writeChar || !notifyChar) {
    lastError = "characteristic_missing";
    logState("ble_setup_failed", lastError);
    return false;
  }

  if (notifyChar->canNotify()) {
    notifyChar->registerForNotify(notifyCallback);
    logState("ble_notifications_enabled");
  } else {
    lastError = "notify_not_supported";
    logState("ble_setup_failed", lastError);
    return false;
  }
  delay(200);

  logState("ble_enter_at_mode");
  writeChar->writeValue((uint8_t *)"+++", 3, false);
  delay(250);
  const char exitAt[] = "AT+exit\r\n";
  logState("ble_exit_at_mode");
  writeChar->writeValue((uint8_t *)exitAt, sizeof(exitAt) - 1, false);
  delay(250);

  std::vector<uint8_t> readAddress = {0xff, 0x03, 0xf0, 0x01, 0x00, 0x01, 0xf3, 0x14};
  rxBuf.clear();
  logState("ble_read_modbus_address");
  writeChar->writeValue(readAddress.data(), readAddress.size(), false);

  uint32_t start = millis();
  while (millis() - start < 5000) {
    if (rxBuf.size() >= 7 && rxBuf[1] == 0x03) {
      std::vector<uint8_t> frame(rxBuf.begin(), rxBuf.begin() + 7);
      if (!crcOk(frame)) {
        lastError = "modbus_address_crc_invalid";
        logState("ble_init_failed", lastError);
        return false;
      }
      slaveAddress = frame[0];
      deviceReady = true;
      lastSeenMs = millis();
      logState("ble_ready", "modbus_slave=0x" + hexByte(slaveAddress));
      return true;
    }
    delay(5);
  }

  lastError = "modbus_address_timeout";
  logState("ble_init_failed", lastError);
  return false;
}

static TxResult transact(const std::vector<uint8_t> &request, uint8_t expectedFunc) {
  TxResult r;
  if (!deviceReady || !bleConnected || !writeChar) {
    r.error = "ble_not_ready";
    logState("modbus_rejected", r.error);
    return r;
  }
  if (transactionBusy) {
    r.error = "busy";
    logState("modbus_rejected", r.error);
    return r;
  }
  transactionBusy = true;
  rxBuf.clear();

  Serial.printf("[MODBUS] request function=0x%02X address=0x%04X bytes=%u\n",
                request.size() > 1 ? request[1] : 0,
                request.size() > 3 ? (request[2] << 8) | request[3] : 0,
                (unsigned)request.size());
  writeChar->writeValue((uint8_t *)request.data(), request.size(), false);
  uint32_t start = millis();

  while (millis() - start < 5000) {
    while (rxBuf.size() >= 5) {
      if (rxBuf[0] != slaveAddress && rxBuf[0] != 0xff) {
        rxBuf.erase(rxBuf.begin());
        continue;
      }

      uint8_t func = rxBuf[1];
      size_t need = 0;
      if (func == expectedFunc && func == 0x03) need = rxBuf[2] + 5;
      else if (func == expectedFunc && func == 0x06) need = 8;
      else if (func & 0x80) need = 5;
      else {
        rxBuf.erase(rxBuf.begin());
        continue;
      }

      if (rxBuf.size() < need) break;
      r.response.assign(rxBuf.begin(), rxBuf.begin() + need);
      rxBuf.erase(rxBuf.begin(), rxBuf.begin() + need);

      if (!crcOk(r.response)) {
        r.error = "crc_invalid";
        logState("modbus_failed", r.error);
        transactionBusy = false;
        return r;
      }
      if (r.response[1] & 0x80) {
        r.error = "modbus_exception";
        logState("modbus_failed", r.error);
        transactionBusy = false;
        return r;
      }
      r.ok = true;
      Serial.printf("[MODBUS] response ok bytes=%u\n", (unsigned)r.response.size());
      transactionBusy = false;
      return r;
    }
    delay(5);
  }

  r.error = "timeout";
  logState("modbus_failed", r.error);
  transactionBusy = false;
  return r;
}

static TxResult readRegisters(uint16_t start, uint16_t count) {
  TxResult r = transact(buildRead(slaveAddress, start, count), 0x03);
  if (!r.ok) return r;
  uint8_t byteCount = r.response[2];
  for (uint8_t i = 0; i + 1 < byteCount; i += 2) {
    r.registers.push_back((r.response[3 + i] << 8) | r.response[4 + i]);
  }
  return r;
}

static TxResult writeRegister(uint16_t address, uint16_t value) {
  return transact(buildWrite(slaveAddress, address, value), 0x06);
}

static void sendJson(int code, const String &json);
static void sendError(int code, const String &err, const String &msg = "");
static int errCode(const String &err);
static String regsJson(const std::vector<uint16_t> &regs);
static void handleLive();
static bool liveJson(String &json, String &error);

static bool parseJsonNumber(const String &body, const char *name, float &value) {
  String key = "\"" + String(name) + "\"";
  int p = body.indexOf(key);
  if (p < 0) return false;
  p = body.indexOf(':', p + key.length());
  if (p < 0) return false;
  String token = body.substring(p + 1);
  token.trim();
  if (token.startsWith("true")) {
    value = 1;
    return true;
  }
  if (token.startsWith("false")) {
    value = 0;
    return true;
  }
  char *end = nullptr;
  value = strtof(token.c_str(), &end);
  return end != token.c_str();
}

static bool namedValue(const String &body, const char *name, uint16_t &value, float scale = 1.0f) {
  float parsed;
  if (!parseJsonNumber(body, name, parsed) || parsed < 0 || parsed * scale > 65535.0f) return false;
  value = (uint16_t)lroundf(parsed * scale);
  return true;
}

static bool writeNamed(const String &body, const char *name, uint16_t address, float scale = 1.0f) {
  uint16_t value;
  if (!namedValue(body, name, value, scale)) return true;
  TxResult r = writeRegister(address, value);
  if (!r.ok) {
    lastError = r.error;
    return false;
  }
  return true;
}

static bool readConfigBlock(uint16_t start, uint16_t count, std::vector<uint16_t> &values) {
  TxResult r = readRegisters(start, count);
  if (!r.ok) {
    lastError = r.error;
    return false;
  }
  values = r.registers;
  return true;
}

static String configRaw(const std::vector<uint16_t> &values) {
  return regsJson(values);
}

static const char *batteryTypeName(uint16_t value) {
  switch (value) {
    case 0: return "custom";
    case 1: return "vrla";
    case 2: return "ncm2";
    case 3: return "ncm3";
    case 4: return "lifepo4";
    default: return "unknown";
  }
}

static const char *systemTypeName(uint16_t value) {
  switch (value) {
    case 0: return "auto";
    case 1: return "12v";
    case 2: return "24v";
    case 4: return "48v";
    default: return "other";
  }
}

static const char *outputModeName(uint16_t value) {
  switch (value) {
    case 1: return "light_control";
    case 2: return "normally_on";
    case 3: return "time_control";
    default: return "unknown";
  }
}

static void handleConfigRead(const char *kind, uint16_t start, uint16_t count) {
  std::vector<uint16_t> v;
  if (!readConfigBlock(start, count, v)) {
    sendError(errCode(lastError), lastError);
    return;
  }
  String json = "{\"config\":\"" + String(kind) + "\",\"start\":\"0x" +
                hexByte(start >> 8) + hexByte(start & 0xff) + "\",\"registers\":" +
                configRaw(v);
  auto reg = [&v](size_t i) -> uint16_t { return i < v.size() ? v[i] : 0; };

  if (strcmp(kind, "battery") == 0) {
    json += ",\"capacityAh\":" + String(reg(0));
    json += ",\"ratedVoltage\":" + String(reg(1));
    json += ",\"floatVoltage\":" + String(reg(2) / 10.0f, 1);
    json += ",\"fullRecoveryVoltage\":" + String(reg(3) / 10.0f, 1);
    json += ",\"fullVoltage\":" + String(reg(4) / 10.0f, 1);
    json += ",\"overRecoveryVoltage\":" + String(reg(5) / 10.0f, 1);
    json += ",\"overVoltage\":" + String(reg(6) / 10.0f, 1);
    json += ",\"batteryType\":" + String(reg(9));
    json += ",\"batteryTypeName\":\"" + String(batteryTypeName(reg(9))) + "\"";
  } else if (strcmp(kind, "system") == 0) {
    json += ",\"temperatureCompensation\":" + String(reg(0));
    json += ",\"rs485Address\":" + String(reg(1));
    json += ",\"systemType\":" + String(reg(2));
    json += ",\"systemTypeName\":\"" + String(systemTypeName(reg(2))) + "\"";
    json += ",\"language\":" + String(reg(3));
  } else if (strcmp(kind, "pv") == 0) {
    json += ",\"openVoltage\":" + String(reg(0) / 10.0f, 1);
    json += ",\"closeVoltage\":" + String(reg(1) / 10.0f, 1);
    json += ",\"chargingEnabled\":" + String(reg(2) ? "true" : "false");
    json += ",\"mpptEnabled\":" + String(reg(3) ? "true" : "false");
    json += ",\"maxChargingCurrent\":" + String(reg(4) / 10.0f, 1);
  } else if (strcmp(kind, "fan") == 0) {
    json += ",\"chargingEnabled\":" + String(reg(0) ? "true" : "false");
    json += ",\"mpptEnabled\":" + String(reg(1) ? "true" : "false");
    json += ",\"unloadEnabled\":" + String(reg(2) ? "true" : "false");
    json += ",\"maxVoltage\":" + String(reg(3));
    json += ",\"maxCurrent\":" + String(reg(4) / 10.0f, 1);
    json += ",\"maxRpm\":" + String(reg(5));
    json += ",\"polePairs\":" + String(reg(6));
    json += ",\"mpptCutInVoltage\":" + String(reg(7));
    json += ",\"mpptCutInRpm\":" + String(reg(8));
    json += ",\"unloadDelayMinutes\":" + String(reg(9));
    json += ",\"ratedRpm\":" + String(reg(10));
    json += ",\"ratedVoltage\":" + String(reg(11));
    json += ",\"mpptAdjustmentFactor\":" + String(reg(12));
    json += ",\"mpptMaxVoltage\":" + String(reg(13));
  } else if (strcmp(kind, "output") == 0) {
    json += ",\"mode\":" + String(reg(0));
    json += ",\"modeName\":\"" + String(outputModeName(reg(0))) + "\"";
    json += ",\"enabled\":" + String(reg(1) ? "true" : "false");
    json += ",\"protectionCurrent\":" + String(reg(8) / 10.0f, 1);
    json += ",\"ratedCurrent\":" + String(reg(9) / 10.0f, 1);
    json += ",\"ratedVoltage\":" + String(reg(10));
    json += ",\"ratedPower\":" + String(reg(11));
    json += ",\"energyEnabled\":" + String(reg(15) ? "true" : "false");
    json += ",\"energyVoltage\":" + String(reg(17) / 10.0f, 1);
    json += ",\"lowVoltage\":" + String(reg(19) / 10.0f, 1);
    json += ",\"lowRecoveryVoltage\":" + String(reg(20) / 10.0f, 1);
  }
  json += "}";
  sendJson(200, json);
}

static bool configWrite(const char *kind, const String &body) {
  if (strcmp(kind, "battery") == 0) {
    if (!writeNamed(body, "capacityAh", 0x1001) ||
        !writeNamed(body, "ratedVoltage", 0x1002) ||
        !writeNamed(body, "floatVoltage", 0x1003, 10) ||
        !writeNamed(body, "fullRecoveryVoltage", 0x1004, 10) ||
        !writeNamed(body, "fullVoltage", 0x1005, 10) ||
        !writeNamed(body, "overRecoveryVoltage", 0x1006, 10) ||
        !writeNamed(body, "overVoltage", 0x1007, 10) ||
        !writeNamed(body, "batteryType", 0x100a)) return false;
  } else if (strcmp(kind, "system") == 0) {
    if (!writeNamed(body, "temperatureCompensation", 0x100b) ||
        !writeNamed(body, "rs485Address", 0x100c) ||
        !writeNamed(body, "systemType", 0x100d) ||
        !writeNamed(body, "language", 0x100e)) return false;
  } else if (strcmp(kind, "pv") == 0) {
    if (!writeNamed(body, "openVoltage", 0x2001, 10) ||
        !writeNamed(body, "closeVoltage", 0x2002, 10) ||
        !writeNamed(body, "chargingEnabled", 0x2003) ||
        !writeNamed(body, "mpptEnabled", 0x2004) ||
        !writeNamed(body, "maxChargingCurrent", 0x2005, 10)) return false;
  } else if (strcmp(kind, "fan") == 0) {
    if (!writeNamed(body, "chargingEnabled", 0x3001) ||
        !writeNamed(body, "mpptEnabled", 0x3002) ||
        !writeNamed(body, "unloadEnabled", 0x3003) ||
        !writeNamed(body, "maxVoltage", 0x3004) ||
        !writeNamed(body, "maxCurrent", 0x3005, 10) ||
        !writeNamed(body, "maxRpm", 0x3006) ||
        !writeNamed(body, "polePairs", 0x3007) ||
        !writeNamed(body, "mpptCutInVoltage", 0x3008) ||
        !writeNamed(body, "mpptCutInRpm", 0x3009) ||
        !writeNamed(body, "unloadDelayMinutes", 0x300a) ||
        !writeNamed(body, "ratedRpm", 0x300b) ||
        !writeNamed(body, "ratedVoltage", 0x300c) ||
        !writeNamed(body, "mpptAdjustmentFactor", 0x300d) ||
        !writeNamed(body, "mpptMaxVoltage", 0x300e)) return false;
  } else if (strcmp(kind, "output") == 0) {
    if (!writeNamed(body, "mode", 0x4001) ||
        !writeNamed(body, "enabled", 0x4002) ||
        !writeNamed(body, "protectionCurrent", 0x4009, 10) ||
        !writeNamed(body, "ratedCurrent", 0x400a, 10) ||
        !writeNamed(body, "ratedVoltage", 0x400b) ||
        !writeNamed(body, "ratedPower", 0x400c) ||
        !writeNamed(body, "energyEnabled", 0x4010) ||
        !writeNamed(body, "energyVoltage", 0x4012, 10) ||
        !writeNamed(body, "lowVoltage", 0x4014, 10) ||
        !writeNamed(body, "lowRecoveryVoltage", 0x4015, 10)) return false;
  }
  return true;
}

static void handleConfigWrite(const char *kind, uint16_t start, uint16_t count) {
  if (server.header("X-Api-Key") != API_KEY) {
    sendError(401, "unauthorized");
    return;
  }
  if (!configWrite(kind, server.arg("plain"))) {
    sendError(errCode(lastError), lastError);
    return;
  }
  handleConfigRead(kind, start, count);
}

static bool isWritable(uint16_t a) {
  if (a == 0x1001 || (a >= 0x1003 && a <= 0x1010)) return true;
  if (a == 0xe00a) return true;
  if (a >= 0x2001 && a <= 0x2005) return true;
  if (a >= 0x3001 && a <= 0x300e) return true;
  if (a == 0x4001 || a == 0x4002) return true;
  if (a >= 0x4009 && a <= 0x4010) return true;
  if (a == 0x4012 || a == 0x4014 || a == 0x4015) return true;
  if (a >= 0x4018 && a <= 0x401f) return true;
  return false;
}

static void sendJson(int code, const String &json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Api-Key");
  server.sendHeader("Access-Control-Max-Age", "600");
  server.send(code, "application/json", json);
}

static void sendError(int code, const String &err, const String &msg) {
  sendJson(code, "{\"error\":\"" + err + "\",\"message\":\"" + msg + "\"}");
}

static int errCode(const String &err) {
  if (err == "busy") return 409;
  if (err == "timeout") return 504;
  if (err == "ble_not_ready") return 503;
  return 502;
}

static String regsJson(const std::vector<uint16_t> &regs) {
  String s = "[";
  for (size_t i = 0; i < regs.size(); i++) {
    if (i) s += ",";
    s += String(regs[i]);
  }
  s += "]";
  return s;
}

static bool apiAuthorized() {
  return server.header("X-Api-Key") == API_KEY;
}

static String jsonEscape(const String &value) {
  String escaped;
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c == '"' || c == '\\') escaped += '\\';
    if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else if (c == '\t') escaped += "\\t";
    else if (c < 0x20) {
      static const char hex[] = "0123456789ABCDEF";
      escaped += "\\u00";
      escaped += hex[c >> 4];
      escaped += hex[c & 0x0f];
    }
    else escaped += static_cast<char>(c);
  }
  return escaped;
}

static bool jsonStringField(const String &body, const char *name, String &value, bool &present) {
  const String key = "\"" + String(name) + "\"";
  int position = body.indexOf(key);
  present = position >= 0;
  if (!present) return true;
  position = body.indexOf(':', position + key.length());
  if (position < 0) return false;
  do { ++position; } while (position < static_cast<int>(body.length()) && isspace(body[position]));
  if (position >= static_cast<int>(body.length()) || body[position] != '"') return false;
  const int start = ++position;
  while (position < static_cast<int>(body.length())) {
    if (body[position] == '\\') return false;
    if (body[position] == '"') {
      value = body.substring(start, position);
      return true;
    }
    ++position;
  }
  return false;
}

static bool parseVictronKey(const String &text, uint8_t key[16]) {
  if (text.length() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    const uint8_t high = hexNibble(text[i * 2]);
    const uint8_t low = hexNibble(text[i * 2 + 1]);
    if (high == 0xff || low == 0xff) return false;
    key[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

static String protocolName(uint8_t protocol) {
  if (protocol == 2) return "victron";
  if (protocol == 1) return "greenpower";
  return "other";
}

static String configuredDeviceJson(size_t index) {
  DeviceConfig config;
  DeviceState state;
  portENTER_CRITICAL(&deviceStateMux);
  config = configuredDevices[index];
  state = deviceStates[index];
  portEXIT_CRITICAL(&deviceStateMux);

  String json = "{\"id\":\"" + String(config.id) + "\",\"address\":\"" +
      String(config.address) + "\",\"name\":\"" + jsonEscape(String(config.name)) +
      "\",\"protocol\":\"" + protocolName(config.protocol) + "\",\"enabled\":" +
      String(config.enabled ? "true" : "false") + ",\"keyConfigured\":" +
      String(config.hasKey ? "true" : "false") + ",\"lastSeenMs\":" +
      String(state.lastSeen) + ",\"rssi\":" + String(state.rssi);
  if (config.protocol == 2) {
    json += ",\"capabilities\":[\"telemetry\"]";
    json += ",\"transport\":\"ble_advertisement\",\"productId\":" +
        String(state.productId) + ",\"productName\":\"" +
        jsonEscape(victronProductName(state.productId)) + "\",\"productType\":\"";
    json += state.lastSeen ? victronRecordName(state.recordType) : "unknown";
    json += "\",\"decoded\":";
    json += state.decoded ? "true" : "false";
  } else {
    const bool connected = deviceAddress.equalsIgnoreCase(config.address) && bleConnected;
    json += ",\"capabilities\":[\"telemetry\",\"registers\",\"write\",\"reconnect\"]";
    json += ",\"transport\":\"gatt\",\"connected\":";
    json += connected ? "true" : "false";
    json += ",\"ready\":";
    json += connected && deviceReady ? "true" : "false";
  }
  json += "}";
  return json;
}

static void handleDevicesList() {
  String json = "{\"devices\":[";
  bool first = true;
  for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
    if (!configuredDevices[i].enabled) continue;
    if (!first) json += ",";
    json += configuredDeviceJson(i);
    first = false;
  }
  json += "],\"limit\":" + String(MAX_CONFIGURED_DEVICES) + "}";
  sendJson(200, json);
}

static void handleDeviceCreate() {
  if (!apiAuthorized()) {
    sendError(401, "unauthorized");
    return;
  }

  const String body = server.arg("plain");
  String address, name, protocol, keyText;
  bool hasAddress, hasName, hasProtocol, hasKey;
  if (!jsonStringField(body, "address", address, hasAddress) ||
      !jsonStringField(body, "name", name, hasName) ||
      !jsonStringField(body, "protocol", protocol, hasProtocol) ||
      !jsonStringField(body, "key", keyText, hasKey) ||
      !hasAddress || !hasProtocol) {
    sendError(400, "bad_request", "Expected address and protocol string fields");
    return;
  }

  address = normalizedAddress(address);
  protocol.toLowerCase();
  const uint8_t protocolId = protocol == "victron" ? 2 :
      (protocol == "greenpower" ? 1 : 0);
  uint8_t key[16] = {};
  if (address.isEmpty() || protocolId == 0 ||
      (hasKey && !parseVictronKey(keyText, key)) ||
      (hasKey && protocolId != 2)) {
    sendError(400, "bad_request", "Invalid address, protocol, or 32-character hexadecimal Victron key");
    return;
  }

  int freeSlot = -1;
  for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
    if (configuredDevices[i].enabled &&
        address.equalsIgnoreCase(configuredDevices[i].address)) {
      sendError(409, "device_exists");
      return;
    }
    if (!configuredDevices[i].enabled && freeSlot < 0) freeSlot = static_cast<int>(i);
  }
  if (freeSlot < 0) {
    sendError(409, "device_limit_reached");
    return;
  }

  const String id = deviceIdForAddress(address);
  DeviceConfig config = {};
  copyText(config.id, sizeof(config.id), id);
  copyText(config.address, sizeof(config.address), address);
  if (hasName) copyText(config.name, sizeof(config.name), name);
  config.protocol = protocolId;
  config.hasKey = hasKey;
  config.enabled = true;
  if (hasKey) memcpy(config.key, key, sizeof(key));

  portENTER_CRITICAL(&deviceStateMux);
  configuredDevices[freeSlot] = config;
  deviceStates[freeSlot] = {};
  portEXIT_CRITICAL(&deviceStateMux);
  if (!saveDeviceRegistry()) {
    portENTER_CRITICAL(&deviceStateMux);
    configuredDevices[freeSlot] = {};
    portEXIT_CRITICAL(&deviceStateMux);
    sendError(500, "storage_failed");
    return;
  }

  sendJson(201, configuredDeviceJson(freeSlot));
}

static void handleDeviceDetail(const String &id) {
  const int index = configuredDeviceIndex(id);
  if (index < 0) {
    sendError(404, "device_not_found");
    return;
  }
  sendJson(200, configuredDeviceJson(index));
}

static void handleDeviceUpdate(const String &id) {
  if (!apiAuthorized()) {
    sendError(401, "unauthorized");
    return;
  }
  const int index = configuredDeviceIndex(id);
  if (index < 0) {
    sendError(404, "device_not_found");
    return;
  }

  const String body = server.arg("plain");
  String name, keyText;
  bool hasName, hasKey;
  uint8_t key[16] = {};
  if (!jsonStringField(body, "name", name, hasName) ||
      !jsonStringField(body, "key", keyText, hasKey) ||
      (!hasName && !hasKey) || (hasKey && !parseVictronKey(keyText, key))) {
    sendError(400, "bad_request", "Expected name and/or a 32-character hexadecimal key");
    return;
  }
  if (hasKey && configuredDevices[index].protocol != 2) {
    sendError(400, "key_not_supported_for_protocol");
    return;
  }

  char nameText[sizeof(configuredDevices[index].name)] = {};
  if (hasName) copyText(nameText, sizeof(nameText), name);
  DeviceConfig previous = configuredDevices[index];
  DeviceState previousDeviceState = deviceStates[index];
  uint8_t previousKey[16];
  memcpy(previousKey, configuredDevices[index].key, sizeof(previousKey));
  portENTER_CRITICAL(&deviceStateMux);
  if (hasName) memcpy(configuredDevices[index].name, nameText, sizeof(nameText));
  if (hasKey) {
    memcpy(configuredDevices[index].key, key, sizeof(key));
    configuredDevices[index].hasKey = true;
    deviceStates[index].decoded = false;
  }
  portEXIT_CRITICAL(&deviceStateMux);
  if (!saveDeviceRegistry()) {
    portENTER_CRITICAL(&deviceStateMux);
    configuredDevices[index] = previous;
    memcpy(configuredDevices[index].key, previousKey, sizeof(previousKey));
    deviceStates[index] = previousDeviceState;
    portEXIT_CRITICAL(&deviceStateMux);
    sendError(500, "storage_failed");
    return;
  }
  sendJson(200, configuredDeviceJson(index));
}

static void handleDeviceDelete(const String &id) {
  if (!apiAuthorized()) {
    sendError(401, "unauthorized");
    return;
  }
  const int index = configuredDeviceIndex(id);
  if (index < 0) {
    sendError(404, "device_not_found");
    return;
  }
  const bool isActive = deviceAddress.equalsIgnoreCase(configuredDevices[index].address);
  DeviceConfig previous = configuredDevices[index];
  DeviceState previousState = deviceStates[index];
  portENTER_CRITICAL(&deviceStateMux);
  configuredDevices[index] = {};
  deviceStates[index] = {};
  portEXIT_CRITICAL(&deviceStateMux);
  if (!saveDeviceRegistry()) {
    portENTER_CRITICAL(&deviceStateMux);
    configuredDevices[index] = previous;
    deviceStates[index] = previousState;
    portEXIT_CRITICAL(&deviceStateMux);
    sendError(500, "storage_failed");
    return;
  }
  if (isActive && bleClient && bleClient->isConnected()) bleClient->disconnect();
  sendJson(200, "{\"deleted\":true}");
}

static void handleDiscoveryStart() {
  uint32_t durationSeconds = 30;
  if (server.hasArg("durationSeconds")) {
    const String durationText = server.arg("durationSeconds");
    bool validDuration = !durationText.isEmpty() && durationText.length() <= 2;
    for (size_t i = 0; i < durationText.length(); ++i) {
      if (!isDigit(durationText[i])) validDuration = false;
    }
    const long requestedDuration = durationText.toInt();
    if (!validDuration || requestedDuration < 1 || requestedDuration > 60) {
      sendError(400, "bad_request", "durationSeconds must be between 1 and 60");
      return;
    }
    durationSeconds = static_cast<uint32_t>(requestedDuration);
  }

  if (discoveryRunning || connectScanActive) {
    sendError(409, "scan_busy");
    return;
  }
  portENTER_CRITICAL(&deviceStateMux);
  memset(discoveredDevices, 0, sizeof(discoveredDevices));
  discoveredCount = 0;
  portEXIT_CRITICAL(&deviceStateMux);
  lastAnyAdvertisementLog = 0;

  if (!startAdvertisementScan(durationSeconds)) {
    sendError(409, "scan_busy");
    return;
  }
  sendJson(202, "{\"scanning\":true,\"durationSeconds\":" + String(durationSeconds) + "}");
}

static void handleDiscoveryResults() {
  String json = "{\"scanning\":" + String(discoveryRunning ? "true" : "false") +
                ",\"advertisementCount\":" + String(scanAdvertisementCount) +
                ",\"devices\":[";
  portENTER_CRITICAL(&deviceStateMux);
  const size_t count = discoveredCount;
  DiscoveredDevice copy[MAX_DISCOVERED_DEVICES];
  memcpy(copy, discoveredDevices, sizeof(copy));
  portEXIT_CRITICAL(&deviceStateMux);
  for (size_t i = 0; i < count; ++i) {
    if (i) json += ",";
    json += "{\"id\":\"" + deviceIdForAddress(copy[i].address) +
        "\",\"address\":\"" + String(copy[i].address) + "\",\"name\":\"" +
        jsonEscape(String(copy[i].name)) + "\",\"protocol\":\"" +
        protocolName(copy[i].protocol) + "\",\"rssi\":" + String(copy[i].rssi) +
        ",\"lastSeenMs\":" + String(copy[i].lastSeen) +
        ",\"manufacturerDataLength\":" + String(copy[i].manufacturerDataLength) +
        ",\"manufacturerDataHex\":\"" + String(copy[i].manufacturerDataHex) +
        "\",\"manufacturerDataTruncated\":" +
        String(copy[i].manufacturerDataTruncated ? "true" : "false") +
        ",\"greenPowerService\":" + String(copy[i].hasGreenPowerService ? "true" : "false");
    if (copy[i].protocol == 2) {
      json += ",\"productId\":" + String(copy[i].productId);
      json += ",\"productName\":\"" + jsonEscape(victronProductName(copy[i].productId));
      json += "\",\"productType\":\"";
      json += victronRecordName(copy[i].recordType);
      const int index = configuredDeviceIndex(deviceIdForAddress(copy[i].address));
      json += "\",\"keyRequired\":";
      json += index < 0 || !configuredDevices[index].hasKey ? "true" : "false";
    }
    json += "}";
  }
  json += "]}";
  sendJson(200, json);
}

static void handleRecentLogs() {
  size_t requestedLimit = MAX_RECENT_LOGS;
  if (server.hasArg("limit")) {
    const String limitText = server.arg("limit");
    bool validLimit = !limitText.isEmpty() && limitText.length() <= 2;
    for (size_t i = 0; i < limitText.length(); ++i) {
      if (!isDigit(limitText[i])) validLimit = false;
    }
    const long parsedLimit = limitText.toInt();
    if (!validLimit || parsedLimit < 1 || parsedLimit > MAX_RECENT_LOGS) {
      sendError(400, "bad_request", "limit must be between 1 and 40");
      return;
    }
    requestedLimit = static_cast<size_t>(parsedLimit);
  }

  static RecentLogEntry snapshot[MAX_RECENT_LOGS] = {};
  portENTER_CRITICAL(&recentLogMux);
  const size_t count = recentLogCount < requestedLimit ? recentLogCount : requestedLimit;
  const size_t first = (recentLogNext + MAX_RECENT_LOGS - count) % MAX_RECENT_LOGS;
  for (size_t i = 0; i < count; ++i) {
    snapshot[i] = recentLogs[(first + i) % MAX_RECENT_LOGS];
  }
  portEXIT_CRITICAL(&recentLogMux);

  String json = "{\"logs\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i) json += ",";
    json += "{\"uptimeMs\":" + String(snapshot[i].uptimeMs) +
        ",\"message\":\"" + jsonEscape(String(snapshot[i].message)) + "\"}";
  }
  json += "],\"count\":" + String(count) + ",\"limit\":" + String(requestedLimit) + "}";
  sendJson(200, json);
}

static void handleDeviceTelemetry(const String &id) {
  const int index = configuredDeviceIndex(id);
  if (index < 0) {
    sendError(404, "device_not_found");
    return;
  }
  DeviceConfig config;
  DeviceState state;
  portENTER_CRITICAL(&deviceStateMux);
  config = configuredDevices[index];
  state = deviceStates[index];
  portEXIT_CRITICAL(&deviceStateMux);

  if (config.protocol == 1) {
    if (!deviceAddress.equalsIgnoreCase(config.address) || !deviceReady) {
      sendError(503, "device_not_connected");
      return;
    }
    String data, error;
    if (!liveJson(data, error)) {
      sendError(errCode(error), error);
      return;
    }
    sendJson(200, "{\"id\":\"" + String(config.id) +
             "\",\"protocol\":\"greenpower\",\"transport\":\"gatt\",\"available\":true,\"data\":" +
             data + "}");
    return;
  }

  String json = "{\"id\":\"" + String(config.id) + "\",\"protocol\":\"victron\"";
  json += ",\"transport\":\"ble_advertisement\",\"keyConfigured\":";
  json += config.hasKey ? "true" : "false";
  json += ",\"available\":";
  json += state.decoded ? "true" : "false";
  json += ",\"lastSeenMs\":" + String(state.lastSeen) + ",\"rssi\":" + String(state.rssi);
  if (!config.hasKey) {
    json += ",\"decodeError\":\"key_required\",\"data\":null}";
  } else if (!state.decoded) {
    json += ",\"decodeError\":\"not_seen_or_key_mismatch\",\"data\":null}";
  } else {
    std::vector<uint8_t> plaintext(state.plaintext, state.plaintext + state.dataLength);
    json += ",\"decodeError\":null,\"data\":";
    json += victronTelemetryJson(state.recordType, plaintext);
    json += "}";
  }
  sendJson(200, json);
}

static void handleStatus() {
  String json = "{";
  json += "\"wifi\":{\"connected\":" + String(WiFi.isConnected() ? "true" : "false");
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + "},";
  json += "\"ble\":{\"connected\":" + String(bleConnected ? "true" : "false");
  json += ",\"ready\":" + String(deviceReady ? "true" : "false");
  json += ",\"deviceName\":\"" + deviceName + "\",\"deviceAddress\":\"" + deviceAddress + "\"";
  json += ",\"modbusAddress\":" + String(slaveAddress) + ",\"lastSeenMs\":" + String(lastSeenMs) + "},";
  json += "\"lastError\":" + String(lastError.length() ? "\"" + lastError + "\"" : "null");
  json += ",\"devices\":[";
  bool first = true;
  for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
    if (!configuredDevices[i].enabled) continue;
    if (!first) json += ",";
    json += configuredDeviceJson(i);
    first = false;
  }
  json += "]";
  json += "}";
  sendJson(200, json);
}

static void handleHealth() {
  bool deviceHealthy = bleConnected && deviceReady;
  for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
    if (!configuredDevices[i].enabled) continue;
    if (configuredDevices[i].protocol == 2 &&
        deviceStates[i].decoded &&
        millis() - deviceStates[i].lastSeen <= 30000) {
      deviceHealthy = true;
    }
  }
  bool healthy = WiFi.isConnected() && deviceHealthy;
  String json = "{";
  json += "\"healthy\":" + String(healthy ? "true" : "false");
  json += ",\"uptimeMs\":" + String(millis());
  json += ",\"wifiConnected\":" + String(WiFi.isConnected() ? "true" : "false");
  json += ",\"wifiStatus\":\"" + String(wifiStatusName(WiFi.status())) + "\"";
  json += ",\"bleConnected\":" + String(bleConnected ? "true" : "false");
  json += ",\"bleReady\":" + String(deviceReady ? "true" : "false");
  json += ",\"deviceCount\":";
  size_t deviceCount = 0;
  for (size_t i = 0; i < MAX_CONFIGURED_DEVICES; ++i) {
    if (configuredDevices[i].enabled) ++deviceCount;
  }
  json += String(deviceCount);
  json += ",\"lastError\":" + String(lastError.length() ? "\"" + lastError + "\"" : "null");
  json += "}";
  sendJson(healthy ? 200 : 503, json);
}

static void handleRegistersRead() {
  uint16_t start, count;
  if (!parseU16(server.arg("start"), start) || !parseU16(server.arg("count"), count) || count < 1 || count > 31) {
    sendError(400, "bad_request", "Expected start=0x0000 and count=1..31");
    return;
  }

  TxResult r = readRegisters(start, count);
  if (!r.ok) {
    sendError(errCode(r.error), r.error);
    return;
  }

  String json = "{\"slave\":" + String(slaveAddress);
  json += ",\"start\":\"0x" + hexByte(start >> 8) + hexByte(start & 0xff) + "\"";
  json += ",\"count\":" + String(count);
  json += ",\"registers\":" + regsJson(r.registers);
  json += ",\"rawResponseHex\":\"" + bytesToHex(r.response) + "\"}";
  sendJson(200, json);
}

static bool liveJson(String &json, String &error) {
  TxResult a = readRegisters(0x0003, 28);
  if (!a.ok) {
    error = a.error;
    return false;
  }
  TxResult b = readRegisters(0x0027, 25);
  if (!b.ok) {
    error = b.error;
    return false;
  }

  auto reg = [](const std::vector<uint16_t> &v, size_t i) -> uint16_t {
    return i < v.size() ? v[i] : 0;
  };

  float batteryVoltage = reg(a.registers, 4) / 10.0f;
  float batteryCurrent = reg(a.registers, 5) / 10.0f;
  float pvVoltage = reg(a.registers, 7) / 10.0f;
  float pvCurrent = reg(a.registers, 8) / 10.0f;
  float fanVoltage = reg(a.registers, 9) / 10.0f;
  float fanCurrent = reg(a.registers, 10) / 10.0f;
  float workTemp = (reg(a.registers, 12) - 500) / 10.0f;
  float battTemp = (reg(a.registers, 13) - 500) / 10.0f;

  json = "{";
  json += "\"battery\":{\"status\":" + String(reg(a.registers, 1));
  json += ",\"percent\":" + String(reg(a.registers, 2));
  json += ",\"voltage\":" + String(batteryVoltage, 1);
  json += ",\"current\":" + String(batteryCurrent, 1);
  json += ",\"temperature\":" + String(battTemp, 1);
  json += ",\"power\":" + String(reg(b.registers, 2)) + "},";
  json += "\"pv\":{\"day\":" + String(reg(a.registers, 6) ? "true" : "false");
  json += ",\"voltage\":" + String(pvVoltage, 1);
  json += ",\"current\":" + String(pvCurrent, 1);
  json += ",\"power\":" + String(reg(b.registers, 0));
  json += ",\"dailyEnergy\":" + String(reg(b.registers, 9) / 100.0f, 2) + "},";
  json += "\"fan\":{\"voltage\":" + String(fanVoltage, 1);
  json += ",\"current\":" + String(fanCurrent, 1);
  json += ",\"rpm\":" + String(reg(a.registers, 26));
  json += ",\"power\":" + String(reg(b.registers, 1));
  json += ",\"windSpeed\":" + String(reg(b.registers, 5) / 100.0f, 2) + "},";
  json += "\"output\":{\"voltage\":" + String(reg(a.registers, 27) / 10.0f, 1);
  json += ",\"power\":" + String(reg(b.registers, 3)) + "},";
  json += "\"system\":{\"errorCode\":" + String(reg(a.registers, 0));
  json += ",\"workTemperature\":" + String(workTemp, 1) + "}";
  json += "}";
  return true;
}

static void handleLive() {
  String json, error;
  if (!liveJson(json, error)) {
    sendError(errCode(error), error);
    return;
  }
  sendJson(200, json);
}

static void handleWriteRegister() {
  if (server.header("X-Api-Key") != API_KEY) {
    sendError(401, "unauthorized");
    return;
  }

  String path = server.uri();
  String addrStr = path.substring(path.lastIndexOf('/') + 1);
  uint16_t address, value;
  if (!parseU16(addrStr, address) || !parseValueBody(server.arg("plain"), value)) {
    sendError(400, "bad_request", "Expected /registers/0x3001 and body {\"value\":1}");
    return;
  }
  if (!isWritable(address)) {
    sendError(403, "write_not_allowed");
    return;
  }

  TxResult r = writeRegister(address, value);
  if (!r.ok) {
    sendError(errCode(r.error), r.error);
    return;
  }

  String json = "{\"ok\":true,\"slave\":" + String(slaveAddress);
  json += ",\"address\":\"0x" + hexByte(address >> 8) + hexByte(address & 0xff) + "\"";
  json += ",\"value\":" + String(value);
  json += ",\"rawResponseHex\":\"" + bytesToHex(r.response) + "\"}";
  sendJson(200, json);
}

static void handleReconnect() {
  bool ok = connectBle();
  if (!ok) {
    sendError(503, lastError);
    return;
  }
  handleStatus();
}

static bool isActiveGreenPowerDevice(int index) {
  return index >= 0 && configuredDevices[index].protocol == 1 &&
         deviceAddress.equalsIgnoreCase(configuredDevices[index].address) &&
         bleConnected && deviceReady;
}

static void dispatchDeviceRoute() {
  const String prefix = "/api/v1/devices/";
  const String uri = server.uri();
  String remainder = uri.substring(prefix.length());
  const int slash = remainder.indexOf('/');
  const String id = slash < 0 ? remainder : remainder.substring(0, slash);
  const String operation = slash < 0 ? "" : remainder.substring(slash + 1);
  const int index = configuredDeviceIndex(id);

  if (slash < 0) {
    if (server.method() == HTTP_GET) handleDeviceDetail(id);
    else if (server.method() == HTTP_PUT || server.method() == HTTP_PATCH) handleDeviceUpdate(id);
    else if (server.method() == HTTP_DELETE) handleDeviceDelete(id);
    else sendError(405, "method_not_allowed");
    return;
  }
  if (index < 0) {
    sendError(404, "device_not_found");
    return;
  }
  if (operation == "telemetry" && server.method() == HTTP_GET) {
    handleDeviceTelemetry(id);
    return;
  }
  if (operation == "reconnect" && server.method() == HTTP_POST) {
    if (configuredDevices[index].protocol != 1) {
      sendError(405, "advertisement_only");
      return;
    }
    requestedConnectAddress = configuredDevices[index].address;
    const bool connected = connectBle();
    requestedConnectAddress = "";
    if (!connected) {
      sendError(503, lastError);
      return;
    }
    handleStatus();
    return;
  }
  if (operation == "registers" && server.method() == HTTP_GET) {
    if (!isActiveGreenPowerDevice(index)) {
      sendError(503, "device_not_connected");
      return;
    }
    handleRegistersRead();
    return;
  }
  if (operation.startsWith("registers/") && server.method() == HTTP_PUT) {
    if (!apiAuthorized()) {
      sendError(401, "unauthorized");
      return;
    }
    if (!isActiveGreenPowerDevice(index)) {
      sendError(503, "device_not_connected");
      return;
    }
    handleWriteRegister();
    return;
  }
  if (operation.startsWith("config/")) {
    const String kind = operation.substring(7);
    uint16_t start = 0;
    uint16_t count = 0;
    if (kind == "battery") { start = 0x1001; count = 10; }
    else if (kind == "system") { start = 0x100b; count = 5; }
    else if (kind == "pv") { start = 0x2001; count = 5; }
    else if (kind == "fan") { start = 0x3001; count = 14; }
    else if (kind == "output") { start = 0x4001; count = 31; }
    else {
      sendError(404, "config_not_found");
      return;
    }
    if (!isActiveGreenPowerDevice(index)) {
      sendError(503, "device_not_connected");
      return;
    }
    if (server.method() == HTTP_GET) handleConfigRead(kind.c_str(), start, count);
    else if (server.method() == HTTP_PUT) handleConfigWrite(kind.c_str(), start, count);
    else sendError(405, "method_not_allowed");
    return;
  }
  sendError(404, "not_found");
}

static void setupRoutes() {
  const char *prefix = "/api/v1";
  server.on(String(prefix) + "/health", HTTP_GET, handleHealth);
  server.on(String(prefix) + "/status", HTTP_GET, handleStatus);
  server.on(String(prefix) + "/logs", HTTP_GET, handleRecentLogs);
  server.on(String(prefix) + "/ble/reconnect", HTTP_POST, handleReconnect);
  server.on(String(prefix) + "/ble/discovery", HTTP_POST, handleDiscoveryStart);
  server.on(String(prefix) + "/ble/discovery", HTTP_GET, handleDiscoveryResults);
  server.on(String(prefix) + "/devices", HTTP_GET, handleDevicesList);
  server.on(String(prefix) + "/devices", HTTP_POST, handleDeviceCreate);
  server.on(String(prefix) + "/registers", HTTP_GET, handleRegistersRead);
  server.on(String(prefix) + "/live", HTTP_GET, handleLive);
  server.on(String(prefix) + "/config/battery", HTTP_GET, []() {
    handleConfigRead("battery", 0x1001, 10);
  });
  server.on(String(prefix) + "/config/system", HTTP_GET, []() {
    handleConfigRead("system", 0x100b, 5);
  });
  server.on(String(prefix) + "/config/pv", HTTP_GET, []() {
    handleConfigRead("pv", 0x2001, 5);
  });
  server.on(String(prefix) + "/config/fan", HTTP_GET, []() {
    handleConfigRead("fan", 0x3001, 14);
  });
  server.on(String(prefix) + "/config/output", HTTP_GET, []() {
    handleConfigRead("output", 0x4001, 31);
  });
  server.on(String(prefix) + "/config/battery", HTTP_PUT, []() {
    handleConfigWrite("battery", 0x1001, 10);
  });
  server.on(String(prefix) + "/config/system", HTTP_PUT, []() {
    handleConfigWrite("system", 0x100b, 5);
  });
  server.on(String(prefix) + "/config/pv", HTTP_PUT, []() {
    handleConfigWrite("pv", 0x2001, 5);
  });
  server.on(String(prefix) + "/config/fan", HTTP_PUT, []() {
    handleConfigWrite("fan", 0x3001, 14);
  });
  server.on(String(prefix) + "/config/output", HTTP_PUT, []() {
    handleConfigWrite("output", 0x4001, 31);
  });
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS && server.uri().startsWith("/api/v1/")) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-Api-Key");
      server.sendHeader("Access-Control-Max-Age", "600");
      server.send(204);
    } else if (server.method() == HTTP_PUT && server.uri().startsWith("/api/v1/registers/")) {
      handleWriteRegister();
    } else if (server.uri().startsWith("/api/v1/devices/")) {
      dispatchDeviceRoute();
    } else {
      sendError(404, "not_found");
    }
  });
}

void setup() {
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 5000) {
    delay(10);
  }
  delay(250);
  Serial.println();
  Serial.println("========================================");
  Serial.println("GreenPower ESP32 BLE REST Relay");
  Serial.println("Firmware starting");
  Serial.println("========================================");
  Serial.flush();
  logState("boot", "serial=115200");

  loadDeviceRegistry();
  WiFi.mode(WIFI_STA);
  Serial.println("[WIFI] using DHCP");
  logState("wifi_connect_start", "ssid=" WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
    delay(500);
    if (millis() - lastWifiStatusLog >= 5000) {
      lastWifiStatusLog = millis();
      Serial.printf("[WIFI] status=%s elapsed=%lus\n",
                    wifiStatusName(WiFi.status()),
                    (unsigned long)((millis() - wifiStart) / 1000));
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] connected, IP=");
    Serial.println(WiFi.localIP());
    Serial.printf("[WIFI] RSSI=%d dBm\n", WiFi.RSSI());
    logState("wifi_ready");
  } else {
    lastError = "wifi_connect_timeout";
    logState("wifi_failed", String(wifiStatusName(WiFi.status())));
  }

  logState("ble_init");
  BLEDevice::init("greenpower-rest-relay");
  BLEDevice::getScan()->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  BLEDevice::getScan()->setActiveScan(true);
  if (!connectBle()) {
    logState("ble_not_ready", lastError);
  }

  static const char *headers[] = {"X-Api-Key"};
  server.collectHeaders(headers, 1);
  setupRoutes();
  server.begin();
  Serial.println("[HTTP] REST API started on port 80");
  Serial.println("[HTTP] Health: GET /api/v1/health");
  Serial.println("[HTTP] Status: GET /api/v1/status");
  logState("startup_complete");
}

void loop() {
  server.handleClient();

  if (!discoveryRunning && millis() - lastAdvertisementScan >= 10000) {
    lastAdvertisementScan = millis();
    startAdvertisementScan(5);
  }

  static uint32_t lastReconnectAttempt = 0;
  if ((!bleConnected || !deviceReady) && millis() - lastReconnectAttempt > 15000) {
    lastReconnectAttempt = millis();
    if (millis() - lastBleRetryLog >= 15000) {
      lastBleRetryLog = millis();
      logState("ble_reconnect_attempt", lastError);
    }
    if (connectBle()) {
      logState("ble_reconnect_success");
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiStatusLog >= 10000) {
      lastWifiStatusLog = millis();
      logState("wifi_reconnect_attempt", wifiStatusName(WiFi.status()));
    }
    WiFi.reconnect();
    delay(1000);
  }
}
