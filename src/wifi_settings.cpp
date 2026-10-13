#include "wifi_settings.h"
#include "../include/config.h"
#include <Preferences.h>
#include <cJSON.h>

namespace {
constexpr WifiNetwork defaults[] = WIFI_NETWORKS;
static_assert(sizeof(defaults) / sizeof(defaults[0]) >= 1 &&
              sizeof(defaults) / sizeof(defaults[0]) <= MAX_WIFI_NETWORKS,
              "Configure 1-8 default Wi-Fi networks");
portMUX_TYPE settingsMux = portMUX_INITIALIZER_UNLOCKED;
WifiSettings current{};
bool stored = false;

bool validNetwork(const WifiNetwork &network) {
  const size_t ssidLength = strnlen(network.ssid, sizeof(network.ssid));
  const size_t length = strnlen(network.password, sizeof(network.password));
  if (!ssidLength || ssidLength > 32) return false;
  if (length == 64) {
    for (size_t i = 0; i < length; ++i) if (!isxdigit(network.password[i])) return false;
    return true;
  }
  return length == 0 || (length >= 8 && length <= 63);
}

void publish(WifiSettings next, bool saved) {
  portENTER_CRITICAL(&settingsMux);
  next.version = current.version + 1;
  current = next;
  stored = saved;
  portEXIT_CRITICAL(&settingsMux);
}

WifiSettings defaultSettings() {
  WifiSettings result{};
  for (const auto &network : defaults) {
    if (validNetwork(network)) result.networks[result.count++] = network;
  }
  return result;
}
}  // namespace

WifiSettings wifiSettingsSnapshot() {
  portENTER_CRITICAL(&settingsMux);
  const WifiSettings result = current;
  portEXIT_CRITICAL(&settingsMux);
  return result;
}

bool wifiSettingsStored() {
  portENTER_CRITICAL(&settingsMux);
  const bool result = stored;
  portEXIT_CRITICAL(&settingsMux);
  return result;
}

void loadWifiSettings() {
  WifiSettings loaded{};
  Preferences prefs;
  bool valid = false;
  if (prefs.begin("relay-wifi", true)) {
    if (prefs.getBytesLength("networks") == sizeof(loaded)) {
      prefs.getBytes("networks", &loaded, sizeof(loaded));
      valid = loaded.version == 1 && loaded.count > 0 && loaded.count <= MAX_WIFI_NETWORKS;
      for (uint8_t i = 0; valid && i < loaded.count; ++i) valid = validNetwork(loaded.networks[i]);
    }
    prefs.end();
  }
  publish(valid ? loaded : defaultSettings(), valid);
}

bool saveWifiSettings(const String &json, String &error) {
  if (json.length() > 4096 || json.indexOf("\\u0000") >= 0) {
    error = "Invalid or oversized Wi-Fi configuration";
    return false;
  }
  cJSON *root = cJSON_ParseWithOpts(json.c_str(), nullptr, true);
  cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "networks");
  const int count = cJSON_GetArraySize(list);
  if (!cJSON_IsObject(root) || !cJSON_IsArray(list) || count < 1 || count > MAX_WIFI_NETWORKS) {
    cJSON_Delete(root);
    error = "Provide 1-8 networks";
    return false;
  }
  const WifiSettings previous = wifiSettingsSnapshot();
  WifiSettings next{};
  next.version = 1;  // Persistent format version, separate from the runtime revision.
  next.count = count;
  bool valid = true;
  for (int i = 0; valid && i < count; ++i) {
    cJSON *entry = cJSON_GetArrayItem(list, i);
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(entry, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(entry, "password");
    valid = cJSON_IsObject(entry) && cJSON_IsString(ssid) && strlen(ssid->valuestring) <= 32;
    if (!valid) break;
    strcpy(next.networks[i].ssid, ssid->valuestring);
    if (password) {
      valid = cJSON_IsString(password) && strlen(password->valuestring) <= 64;
      if (!valid) break;
      strcpy(next.networks[i].password, password->valuestring);
    } else {
      // Omitted passwords preserve credentials for the same SSID only.
      bool found = false;
      for (uint8_t j = 0; j < previous.count; ++j) {
        if (!strcmp(previous.networks[j].ssid, next.networks[i].ssid)) {
          strcpy(next.networks[i].password, previous.networks[j].password);
          found = true;
          break;
        }
      }
      valid = found;
    }
    valid = valid && validNetwork(next.networks[i]);
    for (int j = 0; valid && j < i; ++j) valid = strcmp(next.networks[j].ssid, next.networks[i].ssid) != 0;
  }
  cJSON_Delete(root);
  if (!valid) {
    error = "Use unique SSIDs (1-32 bytes), and passwords of 8-63 bytes, 64 hex digits, or empty for open networks. New networks require a password field.";
    return false;
  }
  Preferences prefs;
  if (!prefs.begin("relay-wifi", false)) { error = "Wi-Fi storage unavailable"; return false; }
  const bool saved = prefs.putBytes("networks", &next, sizeof(next)) == sizeof(next);
  prefs.end();
  if (!saved) { error = "Wi-Fi settings could not be saved"; return false; }
  publish(next, true);
  return true;
}

bool resetWifiSettings(String &error) {
  WifiSettings next = defaultSettings();
  if (!next.count) { error = "No valid defaults in config.h"; return false; }
  Preferences prefs;
  if (!prefs.begin("relay-wifi", false)) { error = "Wi-Fi storage unavailable"; return false; }
  const bool removed = !prefs.isKey("networks") || prefs.remove("networks");
  prefs.end();
  if (!removed) { error = "Could not reset Wi-Fi settings"; return false; }
  publish(next, false);
  return true;
}
