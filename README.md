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

## Build and Upload

```bash
chmod +x scripts/build_upload.sh
./scripts/build_upload.sh
```

Or pass a serial port:

```bash
./scripts/build_upload.sh /dev/cu.usbserial-0001
```

## API

The relay uses DHCP for its Wi-Fi address. Check the assigned address in the
serial monitor after startup.

### Browser dashboard

Start the dependency-free local dashboard with Node.js:

```bash
node dashboard/server.mjs
```

Open `http://127.0.0.1:4173`. The relay URL is prefilled with
`http://192.168.10.130` and can be changed in the dashboard. Enter the API key
for authenticated device and configuration changes; it remains in the current
browser tab's session storage. Browser access from a different origin requires
the firmware's API CORS preflight support, included in this firmware.

The ESP32-C3 PlatformIO build enables USB CDC on boot so `Serial` output is
available through the board's native USB serial/JTAG port.

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
