# ESP32 Bluetooth-to-REST API Controller Plan

## Goal

Build a minimal ESP32 firmware that connects to the GreenPower device over BLE, joins a Wi-Fi network, and exposes a REST API on the LAN. The ESP32 acts as a relay and protocol adapter:

```text
HTTP client -> Wi-Fi -> ESP32 REST API -> BLE GATT -> GreenPower device
```

The REST API should expose the same practical capabilities as the BLE protocol:

- Read live properties and config registers.
- Write supported settings/registers.
- Report connection health.
- Optionally expose raw Modbus-over-BLE access for debugging and future unknown features.

## Platform Assumptions

- Target: ESP32 with BLE client + Wi-Fi support.
- Firmware style: Arduino ESP32 or ESP-IDF. For minimal implementation, Arduino ESP32 is likely faster.
- Wi-Fi credentials are compiled into firmware first, as requested.
- ESP32 is a Wi-Fi station on an existing network, not initially an access point.
- Only one REST client operation should talk to the BLE device at a time.

## Known BLE Device Interface

Use the primary device-control BLE profile:

| Purpose | UUID |
|---|---|
| BLE service | `0000FFB0-0000-1000-8000-00805F9B34FB` |
| Write characteristic | `0000FFB1-0000-1000-8000-00805F9B34FB` |
| Notify/read characteristic | `0000FFB2-0000-1000-8000-00805F9B34FB` |

BLE behavior:

- Scan for service `FFB0`.
- Connect to matching peripheral.
- Subscribe to notifications on `FFB2`.
- Write command bytes to `FFB1`.
- BLE writes may need 20-byte chunking.
- Responses may arrive split across multiple notifications.

## Device Protocol Summary

The payload sent through BLE is Modbus RTU-like.

Read request:

```text
[slave][03][addr_hi][addr_lo][count_hi][count_lo][crc_lo][crc_hi]
```

Write single register:

```text
[slave][06][addr_hi][addr_lo][value_hi][value_lo][crc_lo][crc_hi]
```

Read response:

```text
[slave][03][byte_count][data...][crc_lo][crc_hi]
```

Write response:

```text
[slave][06][addr_hi][addr_lo][value_hi][value_lo][crc_lo][crc_hi]
```

CRC:

- CRC16/MODBUS
- Initial `0xFFFF`
- Polynomial `0xA001`
- Output order: low byte, high byte

Startup sequence:

1. Connect BLE.
2. Subscribe to notify characteristic.
3. First connection only: write ASCII `+++`.
4. Write ASCII `AT+exit\r\n`.
5. Read Modbus address using fixed bytes:

   ```text
   FF 03 F0 01 00 01 F3 14
   ```

6. Use response byte `0` as the slave address for future requests.

## Minimal Firmware Modules

### 1. Config Module

Compile-time settings:

```cpp
const char* WIFI_SSID = "...";
const char* WIFI_PASSWORD = "...";
const char* DEVICE_NAME_HINT = ""; // optional
const uint32_t BLE_SCAN_TIMEOUT_MS = 10000;
const uint32_t BLE_OPERATION_TIMEOUT_MS = 5000;
```

Future option: store these in NVS, captive portal, or provisioning endpoint.

### 2. Wi-Fi Manager

Responsibilities:

- Connect ESP32 to baked-in Wi-Fi.
- Reconnect automatically if Wi-Fi drops.
- Expose current IP and RSSI to status endpoint.

Minimal behavior:

- Boot.
- Connect to Wi-Fi.
- Start REST server only after Wi-Fi is connected.

### 3. BLE Device Client

Responsibilities:

- Scan for `FFB0`.
- Connect to the first matching device, or a configured MAC if supplied.
- Discover service and characteristics.
- Subscribe to notifications.
- Maintain connection state.
- Reconnect if disconnected.

State machine:

```text
BOOT
  -> WIFI_CONNECTING
  -> BLE_SCANNING
  -> BLE_CONNECTING
  -> BLE_SUBSCRIBING
  -> DEVICE_INIT
  -> READY
  -> ERROR / RECONNECTING when needed
```

Important state fields:

```text
bleConnected: bool
deviceReady: bool
modbusAddress: uint8_t, default 0x02 until discovered
lastError: string
lastSeenMs: uint32_t
```

### 4. Modbus-over-BLE Transport

Responsibilities:

- Build read/write frames.
- Append CRC16.
- Send frame to BLE write characteristic.
- Buffer notification bytes.
- Detect complete response frames.
- Validate CRC.
- Match response to the active request.
- Enforce one active BLE operation at a time.

Minimal API inside firmware:

```text
readRegisters(address, count) -> vector<uint16_t>
writeRegister(address, value) -> success/failure
rawTransaction(bytes) -> response bytes
```

Concurrency rule:

- Use a mutex/lock around BLE transactions.
- If another REST request arrives during an active BLE transaction, either queue it or return HTTP `409 Conflict`.
- For minimal version, return `409 Conflict`.

Timeout rule:

- If no complete response is received within 5 seconds, fail the REST request with HTTP `504 Gateway Timeout`.

### 5. Register Decode Layer

Responsibilities:

- Convert raw register blocks into JSON.
- Keep register scaling and names separate from BLE transport.
- Keep unknown values available as raw register values.

Minimal decoded groups:

- Live status from `0x0003`, count `28`.
- Power/energy from `0x0027`, count `25`.
- System/status from `0x1001`, count `14`.
- Output mode from `0x4001`, count `1`.

Config groups:

- Battery config: `0x1001`, count `10`.
- System config: `0x100B`, count `5`.
- PV config: `0x2001`, count `5`.
- Fan config: `0x3001`, count `14`.
- Output config: `0x4001`, count `31`.

## REST API Design

Base path:

```text
/api/v1
```

### Health and Discovery

```http
GET /api/v1/status
```

Response:

```json
{
  "wifi": {
    "connected": true,
    "ip": "192.168.1.50",
    "rssi": -54
  },
  "ble": {
    "connected": true,
    "ready": true,
    "deviceName": "optional",
    "deviceAddress": "AA:BB:CC:DD:EE:FF",
    "modbusAddress": 2,
    "lastSeenMs": 123456
  },
  "lastError": null
}
```

```http
POST /api/v1/ble/reconnect
```

Forces BLE disconnect, scan, reconnect, subscribe, and init.

### Live Data

```http
GET /api/v1/live
```

Performs the minimal live reads and returns decoded data. This endpoint can read:

- `0x0003`, count `28`
- `0x0027`, count `25`
- `0x1001`, count `14`
- `0x4001`, count `1`

Response shape:

```json
{
  "battery": {
    "status": 0,
    "percent": 87,
    "voltage": 12.8,
    "current": 1.2,
    "temperature": 24.5,
    "power": 15
  },
  "pv": {
    "day": true,
    "voltage": 18.4,
    "current": 2.1,
    "power": 39,
    "dailyEnergy": 1.23
  },
  "fan": {
    "voltage": 12.1,
    "current": 0.4,
    "rpm": 1200,
    "power": 5,
    "windSpeed": 3.2
  },
  "output": {
    "mode": 1,
    "voltage": 12.0,
    "current": 0.8,
    "power": 10
  },
  "system": {
    "errorCode": 0,
    "workTemperature": 23.5
  }
}
```

### Config Reads

```http
GET /api/v1/config/battery
GET /api/v1/config/system
GET /api/v1/config/pv
GET /api/v1/config/fan
GET /api/v1/config/output
```

Each endpoint reads the relevant register block and returns:

```json
{
  "start": "0x1001",
  "count": 10,
  "registers": {
    "0x1001": 123,
    "0x1002": 456
  },
  "decoded": {}
}
```

For the minimal version, `decoded` can be sparse. Preserve raw register values so clients can still work.

### Generic Register Reads

```http
GET /api/v1/registers?start=0x0003&count=28
```

Response:

```json
{
  "slave": 2,
  "start": "0x0003",
  "count": 28,
  "registers": [0, 1, 87],
  "rawResponseHex": "020338..."
}
```

Limits:

- `count` must be `1..31` initially.
- Reject invalid hex or out-of-range values with HTTP `400`.

### Register Writes

```http
PUT /api/v1/registers/0x3001
Content-Type: application/json

{
  "value": 1
}
```

Response:

```json
{
  "ok": true,
  "slave": 2,
  "address": "0x3001",
  "value": 1,
  "rawResponseHex": "020630010001..."
}
```

Validation:

- Value must be `0..65535`.
- Address must be in an allowlist for normal API writes.
- Add an unsafe/debug mode if unrestricted writes are needed.

### Named Writes

Add these only after the generic write path is tested:

```http
PUT /api/v1/config/pv
PUT /api/v1/config/fan
PUT /api/v1/config/battery
PUT /api/v1/config/output
PUT /api/v1/config/system
```

Example:

```json
{
  "chargingEnabled": true,
  "mpptEnabled": true
}
```

The firmware maps fields to register writes.

### Raw Debug Endpoint

Useful during development:

```http
POST /api/v1/raw/modbus
Content-Type: application/json

{
  "hex": "02030003001C..."
}
```

Response:

```json
{
  "responseHex": "020338...",
  "crcValid": true
}
```

Protect this endpoint:

- Compile-time `ENABLE_RAW_API`.
- Or require a static token header.

## Writable Register Allowlist

Initial allowlist based on decoded Android app behavior:

| Area | Writable registers |
|---|---|
| Battery/system | `0x1001`, `0x1003`-`0x100B`, `0x100C`, `0x100D`-`0x1010` |
| Password | `0xE00A` |
| PV | `0x2001`-`0x2005` |
| Fan | `0x3001`-`0x300E` |
| Output | `0x4001`, `0x4002`, `0x4009`-`0x4010`, `0x4012`, `0x4014`, `0x4015`, `0x4018`-`0x401F` |

Recommendation:

- Keep the allowlist strict for normal REST writes.
- Use raw/debug endpoint only during lab testing.
- Log all writes with timestamp, address, value, and result.

## Data Cache Strategy

Minimal version can do direct reads per REST request.

Better version:

- Background poll every 5-10 seconds.
- Cache `/live` response.
- REST `/live` returns cached data by default.
- Add `?fresh=true` to force a BLE read.

Suggested cache fields:

```text
liveCacheJson
liveCacheTimestampMs
lastReadSuccess
lastReadError
```

This prevents many HTTP clients from overwhelming the BLE link.

## Error Handling

HTTP status mapping:

| Condition | HTTP |
|---|---:|
| ESP32 Wi-Fi OK, BLE not connected | `503 Service Unavailable` |
| Device connected but not initialized | `503 Service Unavailable` |
| BLE operation already active | `409 Conflict` |
| Invalid address/count/value | `400 Bad Request` |
| BLE response timeout | `504 Gateway Timeout` |
| CRC invalid | `502 Bad Gateway` |
| Modbus exception response | `502 Bad Gateway` |
| Write rejected by allowlist | `403 Forbidden` |

Error response:

```json
{
  "error": "ble_timeout",
  "message": "Timed out waiting for device response",
  "details": {}
}
```

## Security

Minimal LAN-only version:

- No TLS on ESP32.
- Require a static API token for write endpoints.
- Read endpoints may be open on trusted LAN, or also token-protected.

Header:

```http
X-Api-Key: <compiled-in-key>
```

Write operations should require the token from the first version.

## Implementation Milestones

### Milestone 1: BLE Proof of Life

- Boot ESP32.
- Connect to Wi-Fi.
- Scan for service `FFB0`.
- Connect BLE.
- Subscribe to `FFB2`.
- Read Modbus address with fixed frame.
- Log discovered slave address.

Done when serial log shows:

```text
Wi-Fi connected
BLE connected
Notifications enabled
Modbus address = 2
```

### Milestone 2: Modbus Transaction Layer

- Implement CRC16/MODBUS.
- Implement `readRegisters(start, count)`.
- Implement notification buffering.
- Validate response CRC.
- Read `0x0003`, count `28`.
- Print decoded raw registers.

Done when register values can be read repeatedly without reconnecting.

### Milestone 3: REST Server

- Add `/api/v1/status`.
- Add `/api/v1/registers?start=...&count=...`.
- Add JSON response formatting.
- Return useful HTTP errors.

Done when a LAN client can run:

```text
GET http://esp32-ip/api/v1/status
GET http://esp32-ip/api/v1/registers?start=0x0003&count=28
```

### Milestone 4: Live Decoded Data

- Add decoder for live blocks.
- Add `/api/v1/live`.
- Include raw values for fields not fully understood.

Done when `/live` returns battery, PV, fan, output, and system objects.

### Milestone 5: Safe Writes

- Add generic `PUT /api/v1/registers/{address}`.
- Enforce writable register allowlist.
- Require API key.
- Verify write response echo.

Done when known safe settings can be changed and verified by reading config back.

### Milestone 6: Config Endpoints

- Add config read endpoints.
- Add selected named write endpoints.
- Add validation and scaling for common fields.

Done when client app no longer needs to know register addresses for common settings.

### Milestone 7: Reliability

- Add BLE reconnect loop.
- Add Wi-Fi reconnect loop.
- Add live data cache.
- Add watchdog-safe nonblocking loops.
- Add persistent metrics counters.

Done when the relay runs unattended for days.

## Suggested Firmware Structure

```text
src/
  main.cpp
  config.h
  wifi_manager.h/.cpp
  ble_client.h/.cpp
  modbus_ble.h/.cpp
  register_map.h/.cpp
  api_server.h/.cpp
  json_helpers.h/.cpp
```

Core interfaces:

```cpp
class ModbusBleClient {
public:
  bool begin();
  bool isReady() const;
  uint8_t slaveAddress() const;
  Result<std::vector<uint16_t>> readRegisters(uint16_t start, uint16_t count);
  Result<void> writeRegister(uint16_t address, uint16_t value);
};
```

```cpp
class ApiServer {
public:
  void begin(ModbusBleClient* client);
  void loop();
};
```

## Open Questions to Verify on Hardware

- Does every device advertise service `FFB0`, or do some require name/MAC filtering?
- Is the AT enter/exit step truly required on first connection, or can Modbus reads work immediately?
- Is the Modbus slave address always returned as response byte `0`, or should the actual register value also be inspected?
- Are notifications always enabled successfully on `FFB2` across ESP32 BLE libraries?
- Which writable registers are safe to expose in the first production build?
- Does output current mapping need a corrected read block/count?

## Minimal First Build Scope

For the first useful firmware, implement only:

- Baked-in Wi-Fi credentials.
- BLE scan/connect/subscribe/init.
- CRC16/MODBUS.
- `GET /api/v1/status`.
- `GET /api/v1/registers?start=...&count=...`.
- `GET /api/v1/live`.
- `PUT /api/v1/registers/{address}` with API key and allowlist.

Defer:

- Wi-Fi provisioning.
- OTA updates.
- TLS.
- Named config write endpoints.
- Historical logging.
- Multi-device support.
