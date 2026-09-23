#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

#include "../include/config.h"

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

struct TxResult {
  bool ok = false;
  String error;
  std::vector<uint8_t> response;
  std::vector<uint16_t> registers;
};

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

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    bool hasService = d.haveServiceUUID() && d.isAdvertisingService(BLEUUID(SERVICE_UUID));
    bool nameOk = strlen(DEVICE_NAME_HINT) == 0 || (d.haveName() && d.getName().indexOf(DEVICE_NAME_HINT) >= 0);
    if (hasService && nameOk) {
      if (foundDevice) delete foundDevice;
      foundDevice = new BLEAdvertisedDevice(d);
      BLEDevice::getScan()->stop();
    }
  }
};

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *) override { bleConnected = true; }
  void onDisconnect(BLEClient *) override {
    bleConnected = false;
    deviceReady = false;
    writeChar = nullptr;
    notifyChar = nullptr;
    lastError = "ble_disconnected";
  }
};

static bool connectBle() {
  deviceReady = false;
  lastError = "";
  rxBuf.clear();

  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  writeChar = nullptr;
  notifyChar = nullptr;

  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  scan->setActiveScan(true);
  if (foundDevice) {
    delete foundDevice;
    foundDevice = nullptr;
  }

  scan->start(10, false);
  scan->clearResults();
  if (!foundDevice) {
    lastError = "device_not_found";
    return false;
  }

  deviceName = foundDevice->haveName() ? foundDevice->getName().c_str() : "";
  deviceAddress = foundDevice->getAddress().toString().c_str();

  bleClient = BLEDevice::createClient();
  bleClient->setClientCallbacks(new ClientCallbacks());
  if (!bleClient->connect(foundDevice)) {
    lastError = "connect_failed";
    return false;
  }
  bleConnected = true;

  BLERemoteService *service = bleClient->getService(BLEUUID(SERVICE_UUID));
  if (!service) {
    lastError = "service_missing";
    return false;
  }

  writeChar = service->getCharacteristic(BLEUUID(WRITE_UUID));
  notifyChar = service->getCharacteristic(BLEUUID(NOTIFY_UUID));
  if (!writeChar || !notifyChar) {
    lastError = "characteristic_missing";
    return false;
  }

  if (notifyChar->canNotify()) notifyChar->registerForNotify(notifyCallback);
  delay(200);

  writeChar->writeValue((uint8_t *)"+++", 3, false);
  delay(250);
  const char exitAt[] = "AT+exit\r\n";
  writeChar->writeValue((uint8_t *)exitAt, sizeof(exitAt) - 1, false);
  delay(250);

  std::vector<uint8_t> readAddress = {0xff, 0x03, 0xf0, 0x01, 0x00, 0x01, 0xf3, 0x14};
  rxBuf.clear();
  writeChar->writeValue(readAddress.data(), readAddress.size(), false);

  uint32_t start = millis();
  while (millis() - start < 5000) {
    if (rxBuf.size() >= 7 && rxBuf[1] == 0x03) {
      std::vector<uint8_t> frame(rxBuf.begin(), rxBuf.begin() + 7);
      if (!crcOk(frame)) {
        lastError = "modbus_address_crc_invalid";
        return false;
      }
      slaveAddress = frame[0];
      deviceReady = true;
      lastSeenMs = millis();
      return true;
    }
    delay(5);
  }

  lastError = "modbus_address_timeout";
  return false;
}

static TxResult transact(const std::vector<uint8_t> &request, uint8_t expectedFunc) {
  TxResult r;
  if (!deviceReady || !bleConnected || !writeChar) {
    r.error = "ble_not_ready";
    return r;
  }
  if (transactionBusy) {
    r.error = "busy";
    return r;
  }
  transactionBusy = true;
  rxBuf.clear();

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
        transactionBusy = false;
        return r;
      }
      if (r.response[1] & 0x80) {
        r.error = "modbus_exception";
        transactionBusy = false;
        return r;
      }
      r.ok = true;
      transactionBusy = false;
      return r;
    }
    delay(5);
  }

  r.error = "timeout";
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

static void handleStatus() {
  String json = "{";
  json += "\"wifi\":{\"connected\":" + String(WiFi.isConnected() ? "true" : "false");
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + "},";
  json += "\"ble\":{\"connected\":" + String(bleConnected ? "true" : "false");
  json += ",\"ready\":" + String(deviceReady ? "true" : "false");
  json += ",\"deviceName\":\"" + deviceName + "\",\"deviceAddress\":\"" + deviceAddress + "\"";
  json += ",\"modbusAddress\":" + String(slaveAddress) + ",\"lastSeenMs\":" + String(lastSeenMs) + "},";
  json += "\"lastError\":" + String(lastError.length() ? "\"" + lastError + "\"" : "null");
  json += "}";
  sendJson(200, json);
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

static void handleLive() {
  TxResult a = readRegisters(0x0003, 28);
  if (!a.ok) {
    sendError(errCode(a.error), a.error);
    return;
  }
  TxResult b = readRegisters(0x0027, 25);
  if (!b.ok) {
    sendError(errCode(b.error), b.error);
    return;
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

  String json = "{";
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

static void setupRoutes() {
  const char *prefix = "/api/v1";
  server.on(String(prefix) + "/status", HTTP_GET, handleStatus);
  server.on(String(prefix) + "/ble/reconnect", HTTP_POST, handleReconnect);
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
    if (server.method() == HTTP_PUT && server.uri().startsWith("/api/v1/registers/")) {
      handleWriteRegister();
    } else {
      sendError(404, "not_found");
    }
  });
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Wi-Fi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Wi-Fi IP: ");
  Serial.println(WiFi.localIP());

  BLEDevice::init("greenpower-rest-relay");
  connectBle();

  static const char *headers[] = {"X-Api-Key"};
  server.collectHeaders(headers, 1);
  setupRoutes();
  server.begin();
  Serial.println("REST API started");
}

void loop() {
  server.handleClient();

  static uint32_t lastReconnectAttempt = 0;
  if ((!bleConnected || !deviceReady) && millis() - lastReconnectAttempt > 15000) {
    lastReconnectAttempt = millis();
    connectBle();
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
  }
}
