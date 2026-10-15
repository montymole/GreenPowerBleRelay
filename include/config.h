#pragma once

// Wi-Fi defaults used until a network list is saved through the API/UI.
// Replace these values before flashing.
#define WIFI_SSID_1 "T121"
#define WIFI_SSID_2 "Tarsele"
#define WIFI_SSID_3 "Tarsele2"
#define WIFI_SSID_4 "Piha24"
#define WIFI_SSID_5 "Piha5"
#define WIFI_PASSWORD "singingbowl"
#define API_KEY "windcontrol"

// Ordered defaults, up to 8 networks. Add entries here, for example:
//   {"Home", "password1"}, {"Hotspot", "password2"}
// An empty password denotes an open network. Saved NVS settings override this list.
#define WIFI_NETWORKS { {WIFI_SSID_1, WIFI_PASSWORD},{WIFI_SSID_2, WIFI_PASSWORD},{WIFI_SSID_3, WIFI_PASSWORD},{WIFI_SSID_4, WIFI_PASSWORD},{WIFI_SSID_5, WIFI_PASSWORD} }

// Optional: leave empty to connect to the first peripheral advertising FFB0.
#define DEVICE_NAME_HINT ""
