#include "../src/wifi_settings.h"
#include <Preferences.h>
#include <cassert>
#include <cstdio>

int main() {
  String error;
  loadWifiSettings();
  assert(!wifiSettingsStored());
  assert(wifiSettingsSnapshot().count >= 1);
  assert(saveWifiSettings(R"({"networks":[{"ssid":"First","password":"password1"},{"ssid":"Open","password":""},{"ssid":"Quoted\"SSID","password":"password2"}]})", error));
  assert(wifiSettingsStored());
  loadWifiSettings(); // Re-read persisted bytes, as at boot.
  auto saved = wifiSettingsSnapshot();
  assert(saved.count == 3);
  assert(!strcmp(saved.networks[0].password, "password1"));
  assert(!strcmp(saved.networks[2].ssid, "Quoted\"SSID"));
  assert(saveWifiSettings(R"({"networks":[{"ssid":"Open"},{"ssid":"First"}]})", error));
  loadWifiSettings();
  saved = wifiSettingsSnapshot();
  assert(saved.count == 2 && !strcmp(saved.networks[0].ssid, "Open"));
  assert(!strcmp(saved.networks[1].password, "password1"));
  for (const char *invalid : {
      R"({"networks":[]})", R"({"networks":[{"ssid":"New"}]})",
      R"({"networks":[{"ssid":"First","password":null}]})",
      R"({"networks":[{"ssid":"First","password":"short"}]})",
      R"({"networks":[{"ssid":"First"},{"ssid":"First"}]})",
      R"({"networks":[{"ssid":"","password":""}]})",
      R"({"networks":[{"ssid":"First"}]} trailing)", "not json"}) {
    assert(!saveWifiSettings(invalid, error));
    assert(wifiSettingsSnapshot().version == saved.version);
  }
  Preferences::failWrite = true;
  assert(!saveWifiSettings(R"({"networks":[{"ssid":"Changed","password":"password3"}]})", error));
  assert(wifiSettingsSnapshot().version == saved.version);
  assert(!resetWifiSettings(error));
  Preferences::failWrite = false;
  loadWifiSettings();
  assert(wifiSettingsSnapshot().count == 2);
  assert(resetWifiSettings(error));
  loadWifiSettings();
  assert(!wifiSettingsStored());
  assert(wifiSettingsSnapshot().count >= 1);
  // A corrupt blob falls back to compile-time defaults.
  Preferences::data["networks"] = {1, 2, 3};
  loadWifiSettings();
  assert(!wifiSettingsStored());
  puts("Wi-Fi validation, persistence, password retention and storage failure tests passed");
}
