# ESP32 BLE REST Relay

ESP32 firmware that exposes a REST API over Wi-Fi for GreenPower BLE devices
and read-only Victron BLE advertisement telemetry.

## Configure

Edit `include/config.h`:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASSWORD "..."
#define API_KEY "..."
```

For hardware details and Waveshare firmware/documentation links, see
[`HARDWARE_ESP32-C6-LCD-1.47.md`](HARDWARE_ESP32-C6-LCD-1.47.md).

## Multiple Wi-Fi networks

`include/config.h` defines an ordered default list (maximum 8 networks):

```cpp
#define WIFI_NETWORKS { \
  {"Home", "home-password"}, \
  {"Hotspot", "hotspot-password"} \
}
```

The existing `WIFI_SSID` and `WIFI_PASSWORD` macros supply the initial single
entry until you edit this list. A password of `""` selects an open network.
Each attempt has 30 seconds, then a 5-second pause before the next network.
After the last network, attempts start again from the first. On loss of an
established connection, that network is retried first, then rotation continues.

The dashboard's **Wi-Fi networks** panel can add/remove/reorder networks and
save them using the API key. The complete saved list overrides config.h and
is kept in NVS across reboots and ordinary firmware/dashboard updates. Use
**Use config.h defaults** to remove the saved override. Saving applies after
2 seconds; an unchanged active network stays connected. If its credentials
are changed or removed, the relay reconnects from the first network. Open the
new IP address if the network changes. Initial access still requires one of
the configured networks; this does not add an access-point provisioning mode.

- `GET /api/v1/wifi` returns `source`, `connectedSsid` and ordered `networks`
  with `ssid` and `hasPassword`. Passwords are never returned.
- `PUT /api/v1/wifi` saves a complete list, authenticated with `X-Api-Key`:
  `{"networks":[{"ssid":"Home","password":"home-password"},{"ssid":"Hotspot","password":"hotspot-password"}]}`.
  Omit `password` to retain the existing password for that exact SSID. Explicit
  `"password":""` selects an open network. Empty lists and duplicate SSIDs
  are rejected. SSIDs are 1–32 bytes; passwords are 8–63 bytes or 64 hex digits.
- `DELETE /api/v1/wifi` restores config.h defaults, authenticated with `X-Api-Key`.

USB serial logging stays enabled at 115200 baud. It reports each attempted SSID,
timeouts, disconnections and the connected SSID, IP address and signal strength.
Passwords are not logged.

## Build and Upload

```bash
chmod +x scripts/build_upload.sh
./scripts/build_upload.sh
```

Or pass a serial port:

```bash
./scripts/build_upload.sh /dev/cu.usbserial-0001
```

For an ESP32-C3 without a display or LEDs, use the headless PlatformIO build:

```bash
./scripts/build_upload.sh /dev/cu.usbmodem101 esp32-c3-devkitm-1
```

This target does not initialize LCD, backlight or status LED pins. Wi-Fi, BLE,
automatic reconnection, the REST API and USB serial logs remain available.

For the Waveshare ESP32-C6-LCD-1.47, use its PlatformIO environment:

```bash
./scripts/build_upload.sh /dev/cu.usbmodem1101 esp32-c6-lcd-147
```

This C6 target shows the configured/connected Wi-Fi network name, Wi-Fi status/IP,
BLE readiness and overall relay status on the LCD. Its RGB LED pulses blue/turquoise
while connecting, scanning or exchanging Modbus data, turns green when ready, and
red when a connection or transaction has failed. The LCD's relay indicator follows
the same state. A successful BLE initialization or transaction clears its previous
error. LCD backlight and RGB brightness are limited for this board.

Wi-Fi maintenance and display updates run independently of BLE operations. A Wi-Fi
attempt has 30 seconds to finish, followed by a 5-second pause before retrying;
a lost connection triggers an immediate reconnect attempt. BLE retries continue
without Wi-Fi and wait 15 seconds after an automatic attempt finishes. Modbus
timeouts also trigger BLE reinitialization. Neither connection requires the other
to come up first.
The existing default upload command continues to target the ESP32-C3.

## API

The relay uses DHCP for its Wi-Fi address. Check the assigned address in the
serial monitor after startup.

### Browser dashboard

The ESP32 serves the dashboard at `http://<relay-ip>/`, with the API at
`/api/v1/`. The dashboard always calls the API on the same origin; no server address is
configured in the UI.

Build compressed assets with Python (no npm dependencies):

```bash
python3 scripts/build_dashboard.py
```

Generated files go to `data/dashboard/`. PlatformIO automatically rebuilds them
before firmware/filesystem builds. Only HTML, CSS and browser JS are packaged;
configuration secrets and the local Node server are not included.

First installation on the headless C3 (firmware plus dashboard):

```bash
./scripts/build_upload.sh /dev/cu.usbmodem101 esp32-c3-devkitm-1
./scripts/upload_dashboard.sh /dev/cu.usbmodem101 esp32-c3-devkitm-1
```

Use `esp32-c6-lcd-147` for the display board. `upload_dashboard.sh` builds and
uploads only the SPIFFS image; it preserves firmware and NVS device settings.
The existing partition layout is unchanged (128 KiB SPIFFS).

After installing this firmware once over USB, update either part over Wi-Fi:

```bash
# Prompts for the API key, or reads RELAY_API_KEY from the environment.
python3 scripts/update_over_wifi.py http://192.168.1.50 --target dashboard
python3 scripts/update_over_wifi.py http://192.168.1.50 --target firmware
python3 scripts/update_over_wifi.py http://192.168.1.50 --target all
# Add -e esp32-c6-lcd-147 for the C6. Default is the headless C3.
```

The script builds current sources, checks chip compatibility, uploads with an
API key and MD5 transfer checksum, and waits for the relay after a firmware
reboot. `--no-build` uses existing images. OTA firmware images must retain the
same flash partition layout; changes to the partition table require USB.
If a new firmware changes Wi-Fi credentials or the API key, subsequent access
must use the new settings.

Firmware updates use the inactive OTA application partition; unsuccessful
uploads do not activate it. This is not automatic rollback of firmware that
boots but behaves incorrectly. Dashboard updates overwrite the single SPIFFS
partition: an interrupted upload can make the dashboard unavailable until it
is uploaded again. The API and its upload endpoint remain in firmware.
HTTP and API keys are unencrypted on the local network, so use a trusted LAN.

Update API (requires a nonempty configured API key in `X-Api-Key`):

- `GET /api/v1/update`: chip/filesystem identification.
- `POST /api/v1/update/firmware`: one multipart firmware image.
- `POST /api/v1/update/dashboard`: one multipart SPIFFS image.

Uploads also require `X-Update-Size` and `X-Update-MD5`. A successful firmware
upload reboots the device; dashboard uploads remount SPIFFS without rebooting.
The REST API routes remain separate from the static dashboard routes.

For local dashboard development, start the dependency-free local dashboard with Node.js:

```bash
RELAY_URL=http://192.168.1.50 node dashboard/server.mjs
```

Open `http://127.0.0.1:4173`. The local server proxies `/api/v1/` requests to
`RELAY_URL`; the browser still uses same-origin requests. Enter the API key
for authenticated changes; it remains in the current browser tab's session storage.

The ESP32-C3 and ESP32-C6 PlatformIO builds enable USB CDC on boot so `Serial`
output is available through the board's native USB serial/JTAG port.

### Discovery and device registry

- `POST /api/v1/ble/discovery` starts a BLE scan (30 seconds by default;
  optional `?durationSeconds=1..60`).
- `GET /api/v1/ble/discovery` returns discovered GreenPower and Victron devices.
- `GET /api/v1/devices` lists configured devices (maximum eight).
- `POST /api/v1/devices` configures a discovered device; requires `X-Api-Key`.
- `GET /api/v1/devices/{id}` returns device configuration and current state.
- `PUT /api/v1/devices/{id}` updates the name and/or Victron key; requires `X-Api-Key`.
- `DELETE /api/v1/devices/{id}` removes a device; requires `X-Api-Key`.
- `GET /api/v1/devices/{id}/telemetry` returns current telemetry.
- `POST /api/v1/devices/{id}/reconnect` reconnects a GreenPower GATT device.

Device IDs are the BLE address without separators, lower-case. Add a Victron
device, then provision its 32-hex-character Instant Readout key (from
VictronConnect) using the authenticated `PUT /api/v1/devices/{id}` endpoint:

```json
{"address":"AA:BB:CC:DD:EE:FF","protocol":"victron","name":"Battery monitor"}
{"key":"0123456789abcdef0123456789abcdef"}
```

The first JSON object is the `POST /api/v1/devices` body; the second is the
subsequent `PUT` body. The key is never returned by the API. The firmware
stores it in NVS; standard NVS storage is not encrypted by this application.
Provision keys only on a trusted network: this firmware's HTTP API does not
use TLS.

Victron devices are read from manufacturer-data advertisements and are not
connected to. Their product telemetry is read-only. GreenPower devices use
GATT and retain raw-register and named-config access. The ESP32-C3 firmware
maintains one GreenPower GATT connection at a time; use the per-device
reconnect endpoint to switch the active GreenPower device. Advertisement
telemetry from multiple configured Victron devices is collected during scans.

### Existing GreenPower and relay endpoints

- `GET /api/v1/health` returns HTTP 200 when Wi-Fi and a configured device (or
  the legacy GreenPower connection) is ready, otherwise 503.
- `GET /api/v1/status`
- `GET /api/v1/logs?limit=40` returns the latest in-memory firmware state logs.
- `GET /api/v1/ble/discovery` includes all observed BLE advertisements, including
  unrecognized devices, along with the scan callback count and advertisement
  metadata.
- `POST /api/v1/ble/reconnect`
- `GET /api/v1/registers?start=0x0003&count=28`
- `GET /api/v1/live`
- `PUT /api/v1/registers/0x3001` with header `X-Api-Key: ...` and JSON body `{"value":1}`

Named configuration endpoints are also available:

- `GET` or `PUT /api/v1/config/battery`
- `GET` or `PUT /api/v1/config/system`
- `GET` or `PUT /api/v1/config/pv`
- `GET` or `PUT /api/v1/config/fan`
- `GET` or `PUT /api/v1/config/output`

PUT requests require `X-Api-Key` and accept named JSON fields. For example:

```json
{"mpptEnabled":true,"maxRpm":1200,"mpptCutInVoltage":8}
```

See [`BLE_REGISTER_MAP.md`](BLE_REGISTER_MAP.md) for field names, scaling,
register addresses, and the raw compatibility API. See
[`VICTRON_BLE_SPEC.md`](VICTRON_BLE_SPEC.md) for Victron advertisement handling.
