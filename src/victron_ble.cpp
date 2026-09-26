#include "../include/victron_ble.h"

#include <mbedtls/aes.h>

static constexpr uint16_t VICTRON_COMPANY_ID = 0x02e1;
static constexpr uint8_t PRODUCT_ADVERTISEMENT = 0x10;

static bool validRecordType(uint8_t value) {
  return value <= 0x0d || value == 0x0f;
}

static bool parseVictronAdvertisementData(const uint8_t *manufacturerData, size_t length,
                                          VictronAdvertisement &out) {
  size_t offset = 0;
  if (length < 2) return false;
  const uint16_t company = manufacturerData[0] |
                           (static_cast<uint16_t>(manufacturerData[1]) << 8);
  if (company == VICTRON_COMPANY_ID) offset = 2;
  constexpr size_t RECORD_HEADER_SIZE = 8;
  if (length - offset <= RECORD_HEADER_SIZE) return false;

  const uint8_t *data = manufacturerData + offset;
  if (data[0] != PRODUCT_ADVERTISEMENT) return false;

  const uint16_t counter = static_cast<uint16_t>(data[5]) |
                           (static_cast<uint16_t>(data[6]) << 8);
  const uint16_t productId = static_cast<uint16_t>(data[2]) |
                             (static_cast<uint16_t>(data[3]) << 8);
  const uint8_t recordType = data[4];
  if (!validRecordType(recordType)) return false;

  const size_t keyOffset = 7;
  const size_t encryptedOffset = RECORD_HEADER_SIZE;
  const size_t encryptedLength = length - offset - encryptedOffset;
  if (encryptedLength == 0 || encryptedLength > 16) return false;

  out.productId = productId;
  out.recordType = recordType;
  out.counter = counter;
  out.keyCheck = data[keyOffset];
  out.encrypted.assign(data + encryptedOffset, data + encryptedOffset + encryptedLength);
  return true;
}

bool parseVictronAdvertisement(const std::string &manufacturerData, VictronAdvertisement &out) {
  return parseVictronAdvertisementData(
      reinterpret_cast<const uint8_t *>(manufacturerData.data()), manufacturerData.size(), out);
}

bool parseVictronAdvertisement(const String &manufacturerData, VictronAdvertisement &out) {
  return parseVictronAdvertisementData(
      reinterpret_cast<const uint8_t *>(manufacturerData.c_str()), manufacturerData.length(), out);
}

bool decryptVictronAdvertisement(const VictronAdvertisement &advertisement,
                                 const uint8_t key[16], std::vector<uint8_t> &plaintext) {
  if (advertisement.encrypted.empty() || advertisement.encrypted.size() > 16 ||
      key[0] != advertisement.keyCheck) {
    return false;
  }

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int result = mbedtls_aes_setkey_enc(&aes, key, 128);
  uint8_t nonceCounter[16] = {
    static_cast<uint8_t>(advertisement.counter & 0xff),
    static_cast<uint8_t>(advertisement.counter >> 8),
    0
  };
  uint8_t streamBlock[16] = {};
  size_t offset = 0;
  plaintext.resize(advertisement.encrypted.size());
  if (result == 0) {
    result = mbedtls_aes_crypt_ctr(&aes, advertisement.encrypted.size(), &offset,
                                   nonceCounter, streamBlock, advertisement.encrypted.data(),
                                   plaintext.data());
  }
  mbedtls_aes_free(&aes);
  return result == 0;
}

const char *victronRecordName(uint8_t recordType) {
  switch (recordType) {
    case 0x00: return "test";
    case 0x01: return "solar_charger";
    case 0x02: return "battery_monitor";
    case 0x03: return "inverter";
    case 0x04: return "dc_dc_converter";
    case 0x05: return "smart_lithium";
    case 0x06: return "inverter_rs";
    case 0x07: return "gx_device";
    case 0x08: return "ac_charger";
    case 0x09: return "smart_battery_protect";
    case 0x0a: return "lynx_smart_bms";
    case 0x0b: return "multi_rs";
    case 0x0c: return "ve_bus";
    case 0x0d: return "dc_energy_meter";
    case 0x0f: return "orion_xs";
    default: return "unknown";
  }
}

const char *victronProductName(uint16_t productId) {
  switch (productId) {
    case 0xa3a0: return "VE.Bus Smart Dongle";
    default: return "Victron device";
  }
}

static uint32_t readBits(const std::vector<uint8_t> &data, size_t start, uint8_t count) {
  uint32_t result = 0;
  for (uint8_t i = 0; i < count; ++i) {
    const size_t bit = start + i;
    if (bit / 8 >= data.size()) return UINT32_MAX;
    result |= static_cast<uint32_t>((data[bit / 8] >> (bit % 8)) & 1) << i;
  }
  return result;
}

static String bytesHex(const std::vector<uint8_t> &data) {
  static const char hex[] = "0123456789ABCDEF";
  String result;
  for (uint8_t byte : data) {
    result += hex[byte >> 4];
    result += hex[byte & 0x0f];
  }
  return result;
}

String victronTelemetryJson(uint8_t recordType, const std::vector<uint8_t> &plaintext) {
  String json = "{\"productType\":\"";
  json += victronRecordName(recordType);
  json += "\",\"recordType\":";
  json += String(recordType);
  json += ",\"raw\":\"";
  json += bytesHex(plaintext);
  json += "\",\"values\":{";

  bool hasValue = false;
  auto add = [&json, &hasValue](const char *name, const String &value) {
    if (hasValue) json += ",";
    json += "\"";
    json += name;
    json += "\":";
    json += value;
    hasValue = true;
  };
  auto addNumber = [&add](const char *name, float value, uint8_t decimals) {
    add(name, String(value, static_cast<unsigned int>(decimals)));
  };
  auto signedValue = [](uint32_t value, uint8_t bits) -> int32_t {
    const uint32_t sign = static_cast<uint32_t>(1) << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
  };

  if (recordType == 0x01 && plaintext.size() >= 12) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t error = readBits(plaintext, 8, 8);
    const uint32_t voltage = readBits(plaintext, 16, 16);
    const uint32_t current = readBits(plaintext, 32, 16);
    const uint32_t yield = readBits(plaintext, 48, 16);
    const uint32_t pvPower = readBits(plaintext, 64, 16);
    const uint32_t loadCurrent = readBits(plaintext, 80, 9);
    if (state != 0xff) add("deviceState", String(state));
    if (error != 0xff) add("chargerError", String(error));
    if (voltage != 0x7fff) addNumber("batteryVoltageV", static_cast<int16_t>(voltage) / 100.0f, 2);
    if (current != 0x7fff) addNumber("batteryCurrentA", static_cast<int16_t>(current) / 10.0f, 1);
    if (yield != 0xffff) addNumber("yieldTodayKWh", yield / 100.0f, 2);
    if (pvPower != 0xffff) add("pvPowerW", String(pvPower));
    if (loadCurrent != 0x1ff) addNumber("loadCurrentA", loadCurrent / 10.0f, 1);
  } else if (recordType == 0x02 && plaintext.size() >= 15) {
    const uint32_t timeToGo = readBits(plaintext, 0, 16);
    const uint32_t voltage = readBits(plaintext, 16, 16);
    const uint32_t alarm = readBits(plaintext, 32, 16);
    const uint32_t aux = readBits(plaintext, 48, 16);
    const uint32_t auxInput = readBits(plaintext, 64, 2);
    const uint32_t current = readBits(plaintext, 66, 22);
    const uint32_t consumed = readBits(plaintext, 88, 20);
    const uint32_t soc = readBits(plaintext, 108, 10);
    if (timeToGo != 0xffff) add("timeToGoMinutes", String(timeToGo));
    if (voltage != 0x7fff) addNumber("batteryVoltageV", static_cast<int16_t>(voltage) / 100.0f, 2);
    add("alarmReason", String(alarm));
    if (auxInput != 3 && aux != 0xffff) {
      const char *auxName = auxInput == 0 ? "auxVoltageV" :
                            auxInput == 1 ? "midVoltageV" : "temperatureK";
      add(auxName, String(aux));
    }
    if (current != 0x3fffff) {
      int32_t signedCurrent = (current & 0x200000) ? static_cast<int32_t>(current | 0xffc00000) :
                                                     static_cast<int32_t>(current);
      addNumber("batteryCurrentA", signedCurrent / 1000.0f, 3);
    }
    if (consumed != 0xfffff) addNumber("consumedAh", -static_cast<int32_t>(consumed) / 10.0f, 1);
    if (soc != 0x3ff) addNumber("stateOfChargePercent", soc / 10.0f, 1);
  } else if (recordType == 0x03 && plaintext.size() >= 11) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t alarm = readBits(plaintext, 8, 16);
    const uint32_t batteryVoltage = readBits(plaintext, 24, 16);
    const uint32_t acPower = readBits(plaintext, 40, 16);
    const uint32_t acVoltage = readBits(plaintext, 56, 15);
    const uint32_t acCurrent = readBits(plaintext, 71, 11);
    if (state != 0xff) add("deviceState", String(state));
    add("alarmReason", String(alarm));
    if (batteryVoltage != 0x7fff)
      addNumber("batteryVoltageV", signedValue(batteryVoltage, 16) / 100.0f, 2);
    if (acPower != 0xffff) add("acApparentPowerVA", String(acPower));
    if (acVoltage != 0x7fff) addNumber("acVoltageV", acVoltage / 100.0f, 2);
    if (acCurrent != 0x7ff) addNumber("acCurrentA", acCurrent / 10.0f, 1);
  } else if (recordType == 0x04 && plaintext.size() >= 10) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t error = readBits(plaintext, 8, 8);
    const uint32_t inputVoltage = readBits(plaintext, 16, 16);
    const uint32_t outputVoltage = readBits(plaintext, 32, 16);
    const uint32_t offReason = readBits(plaintext, 48, 32);
    if (state != 0xff) add("deviceState", String(state));
    if (error != 0xff) add("chargerError", String(error));
    if (inputVoltage != 0xffff) addNumber("inputVoltageV", inputVoltage / 100.0f, 2);
    if (outputVoltage != 0x7fff)
      addNumber("outputVoltageV", signedValue(outputVoltage, 16) / 100.0f, 2);
    add("offReason", String(offReason));
  } else if (recordType == 0x05 && plaintext.size() >= 16) {
    const uint32_t bmsFlags = readBits(plaintext, 0, 32);
    const uint32_t error = readBits(plaintext, 32, 16);
    add("bmsFlags", String(bmsFlags));
    add("errorFlags", String(error));
    for (uint8_t cell = 0; cell < 8; ++cell) {
      const uint32_t voltage = readBits(plaintext, 48 + cell * 7, 7);
      if (voltage != 0x7f) {
        const String name = "cell" + String(cell + 1) + "VoltageV";
        add(name.c_str(), String((voltage + 260) / 100.0f, 2));
      }
    }
    const uint32_t batteryVoltage = readBits(plaintext, 104, 12);
    const uint32_t balancer = readBits(plaintext, 116, 4);
    const uint32_t temperature = readBits(plaintext, 120, 7);
    if (batteryVoltage != 0xfff) addNumber("batteryVoltageV", batteryVoltage / 100.0f, 2);
    if (balancer != 0xf) add("balancerStatus", String(balancer));
    if (temperature != 0x7f) add("temperatureC", String(static_cast<int32_t>(temperature) - 40));
  } else if (recordType == 0x06 && plaintext.size() >= 12) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t error = readBits(plaintext, 8, 8);
    const uint32_t batteryVoltage = readBits(plaintext, 16, 16);
    const uint32_t batteryCurrent = readBits(plaintext, 32, 16);
    const uint32_t pvPower = readBits(plaintext, 48, 16);
    const uint32_t yield = readBits(plaintext, 64, 16);
    const uint32_t acPower = readBits(plaintext, 80, 16);
    if (state != 0xff) add("deviceState", String(state));
    if (error != 0xff) add("chargerError", String(error));
    if (batteryVoltage != 0x7fff)
      addNumber("batteryVoltageV", signedValue(batteryVoltage, 16) / 100.0f, 2);
    if (batteryCurrent != 0x7fff)
      addNumber("batteryCurrentA", signedValue(batteryCurrent, 16) / 10.0f, 1);
    if (pvPower != 0xffff) add("pvPowerW", String(pvPower));
    if (yield != 0xffff) addNumber("yieldTodayKWh", yield / 100.0f, 2);
    if (acPower != 0x7fff) add("acOutputPowerW", String(signedValue(acPower, 16)));
  } else if (recordType == 0x08 && plaintext.size() >= 13) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t error = readBits(plaintext, 8, 8);
    if (state != 0xff) add("deviceState", String(state));
    if (error != 0xff) add("chargerError", String(error));
    for (uint8_t channel = 0; channel < 3; ++channel) {
      const size_t start = 16 + channel * 24;
      const uint32_t voltage = readBits(plaintext, start, 13);
      const uint32_t current = readBits(plaintext, start + 13, 11);
      if (voltage != 0x1fff)
        addNumber(("battery" + String(channel + 1) + "VoltageV").c_str(), voltage / 100.0f, 2);
      if (current != 0x7ff)
        addNumber(("battery" + String(channel + 1) + "CurrentA").c_str(), current / 10.0f, 1);
    }
    const uint32_t temperature = readBits(plaintext, 88, 7);
    const uint32_t acCurrent = readBits(plaintext, 96, 9);
    if (temperature != 0x7f) add("temperatureC", String(static_cast<int32_t>(temperature) - 40));
    if (acCurrent != 0x1ff) addNumber("acCurrentA", acCurrent / 10.0f, 1);
  } else if (recordType == 0x0a && plaintext.size() >= 16) {
    const uint32_t error = readBits(plaintext, 0, 8);
    const uint32_t timeToGo = readBits(plaintext, 8, 16);
    const uint32_t voltage = readBits(plaintext, 24, 16);
    const uint32_t current = readBits(plaintext, 40, 16);
    const uint32_t ioStatus = readBits(plaintext, 56, 16);
    const uint32_t warnings = readBits(plaintext, 72, 18);
    const uint32_t soc = readBits(plaintext, 90, 10);
    const uint32_t consumed = readBits(plaintext, 100, 20);
    const uint32_t temperature = readBits(plaintext, 120, 7);
    add("bmsError", String(error));
    if (timeToGo != 0xffff) add("timeToGoMinutes", String(timeToGo));
    if (voltage != 0x7fff)
      addNumber("batteryVoltageV", signedValue(voltage, 16) / 100.0f, 2);
    if (current != 0x7fff)
      addNumber("batteryCurrentA", signedValue(current, 16) / 10.0f, 1);
    add("ioStatus", String(ioStatus));
    add("warningsAlarms", String(warnings));
    if (soc != 0x3ff) addNumber("stateOfChargePercent", soc / 10.0f, 1);
    if (consumed != 0xfffff) addNumber("consumedAh", -static_cast<int32_t>(consumed) / 10.0f, 1);
    if (temperature != 0x7f) add("temperatureC", String(static_cast<int32_t>(temperature) - 40));
  } else if (recordType == 0x0b && plaintext.size() >= 16) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t error = readBits(plaintext, 8, 8);
    const uint32_t current = readBits(plaintext, 16, 16);
    const uint32_t voltage = readBits(plaintext, 32, 14);
    const uint32_t activeAcIn = readBits(plaintext, 46, 2);
    const uint32_t acInPower = readBits(plaintext, 48, 16);
    const uint32_t acOutPower = readBits(plaintext, 64, 16);
    const uint32_t pvPower = readBits(plaintext, 80, 16);
    const uint32_t yield = readBits(plaintext, 96, 16);
    if (state != 0xff) add("deviceState", String(state));
    if (error != 0xff) add("chargerError", String(error));
    if (current != 0x7fff)
      addNumber("batteryCurrentA", signedValue(current, 16) / 10.0f, 1);
    if (voltage != 0x3fff) addNumber("batteryVoltageV", voltage / 100.0f, 2);
    add("activeAcInput", String(activeAcIn));
    if (acInPower != 0x7fff) add("activeAcInputPowerW", String(signedValue(acInPower, 16)));
    if (acOutPower != 0x7fff) add("acOutputPowerW", String(signedValue(acOutPower, 16)));
    if (pvPower != 0xffff) add("pvPowerW", String(pvPower));
    if (yield != 0xffff) addNumber("yieldTodayKWh", yield / 100.0f, 2);
  } else if (recordType == 0x0c && plaintext.size() >= 13) {
    const uint32_t state = readBits(plaintext, 0, 8);
    const uint32_t error = readBits(plaintext, 8, 8);
    const uint32_t current = readBits(plaintext, 16, 16);
    const uint32_t voltage = readBits(plaintext, 32, 14);
    const uint32_t activeAcIn = readBits(plaintext, 46, 2);
    const uint32_t acInPower = readBits(plaintext, 48, 19);
    const uint32_t acOutPower = readBits(plaintext, 67, 19);
    const uint32_t alarm = readBits(plaintext, 86, 2);
    const uint32_t temperature = readBits(plaintext, 88, 7);
    const uint32_t soc = readBits(plaintext, 95, 7);
    if (state != 0xff) add("deviceState", String(state));
    if (error != 0xff) add("veBusError", String(error));
    if (current != 0x7fff)
      addNumber("batteryCurrentA", signedValue(current, 16) / 10.0f, 1);
    if (voltage != 0x3fff) addNumber("batteryVoltageV", voltage / 100.0f, 2);
    add("activeAcInput", String(activeAcIn));
    if (acInPower != 0x3ffff)
      add("activeAcInputPowerW", String(signedValue(acInPower, 19)));
    if (acOutPower != 0x3ffff)
      add("acOutputPowerW", String(signedValue(acOutPower, 19)));
    if (alarm != 3) add("alarm", String(alarm));
    if (temperature != 0x7f) add("batteryTemperatureC", String(static_cast<int32_t>(temperature) - 40));
    if (soc != 0x7f) add("stateOfChargePercent", String(soc));
  } else if (recordType == 0x0d && plaintext.size() >= 11) {
    const uint32_t monitorMode = readBits(plaintext, 0, 16);
    const uint32_t voltage = readBits(plaintext, 16, 16);
    const uint32_t alarm = readBits(plaintext, 32, 16);
    const uint32_t aux = readBits(plaintext, 48, 16);
    const uint32_t auxInput = readBits(plaintext, 64, 2);
    const uint32_t current = readBits(plaintext, 66, 22);
    add("monitorMode", String(signedValue(monitorMode, 16)));
    if (voltage != 0x7fff)
      addNumber("batteryVoltageV", signedValue(voltage, 16) / 100.0f, 2);
    add("alarmReason", String(alarm));
    if (auxInput != 3 && aux != 0xffff) {
      add(auxInput == 0 ? "auxVoltageV" : "temperatureK", String(aux / 100.0f, 2));
    }
    if (current != 0x3fffff)
      addNumber("batteryCurrentA", signedValue(current, 22) / 1000.0f, 3);
  }

  json += "}}";
  return json;
}
