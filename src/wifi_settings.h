#pragma once
#include <Arduino.h>

constexpr uint8_t MAX_WIFI_NETWORKS = 8;
struct WifiNetwork {
  char ssid[33];
  char password[65];
};
struct WifiSettings {
  uint32_t version;
  uint8_t count;
  WifiNetwork networks[MAX_WIFI_NETWORKS];
};

void loadWifiSettings();
WifiSettings wifiSettingsSnapshot();
bool wifiSettingsStored();
bool saveWifiSettings(const String &json, String &error);
bool resetWifiSettings(String &error);
