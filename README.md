# ESP32 BLE REST Relay

Minimal ESP32 firmware that connects to the GreenPower BLE device and exposes a small REST API over Wi-Fi.

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

- `GET /api/v1/status`
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
register addresses, and the raw compatibility API.
