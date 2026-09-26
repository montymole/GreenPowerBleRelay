#pragma once

#include <Arduino.h>
#include <string>
#include <vector>

struct VictronAdvertisement {
  uint16_t productId = 0;
  uint8_t recordType = 0;
  uint16_t counter = 0;
  uint8_t keyCheck = 0;
  std::vector<uint8_t> encrypted;
};

bool parseVictronAdvertisement(const std::string &manufacturerData, VictronAdvertisement &out);
bool parseVictronAdvertisement(const String &manufacturerData, VictronAdvertisement &out);
bool decryptVictronAdvertisement(const VictronAdvertisement &advertisement,
                                 const uint8_t key[16], std::vector<uint8_t> &plaintext);
const char *victronRecordName(uint8_t recordType);
const char *victronProductName(uint16_t productId);
String victronTelemetryJson(uint8_t recordType, const std::vector<uint8_t> &plaintext);
