# Victron BLE Advertisement Support

The relay reads Victron Instant Readout manufacturer data passively. It does
not establish a GATT connection to Victron devices and does not support writes
to them.

## Advertisement decoding

The scanner identifies Victron manufacturer data by company ID `0x02E1`
(little-endian bytes `E1 02`) and Product Advertisement record type `0x10`.
The Extra Manufacturer Data format uses a little-endian 16-bit counter, a
product record type, a key-check byte, and AES-128-CTR encrypted product
data. The counter initializes the first two nonce/counter bytes in
least-significant-byte order; remaining bytes are zero. The product key is 16
bytes and its first byte must match the key-check byte. The counter is used
once per advertisement; no replay filtering is applied.

Recognized product record types are:

| Type | Product |
|---:|---|
| `0x00` | Test |
| `0x01` | Solar charger |
| `0x02` | Battery monitor |
| `0x03` | Inverter |
| `0x04` | DC/DC converter |
| `0x05` | SmartLithium |
| `0x06` | Inverter RS |
| `0x07` | GX device (layout not defined in the linked document) |
| `0x08` | AC charger |
| `0x09` | Smart Battery Protect |
| `0x0A` | Lynx Smart BMS |
| `0x0B` | Multi RS |
| `0x0C` | VE.Bus |
| `0x0D` | DC Energy Meter |
| `0x0F` | Not in the linked 2022 layout |

Decoded fields are returned in `data.values`; the decrypted record is retained
as hex in `data.raw`. Decoders are implemented for types `0x01`-`0x06`,
`0x08`, and `0x0A`-`0x0D`. Other types remain available as raw decrypted data
until their layouts are implemented or verified. Fields marked not-available
in the protocol are omitted from `values`.

## Device API

All paths are under `/api/v1`:

| Method and path | Contract |
|---|---|
| `POST /ble/discovery` | Start a 30-second scan by default (`?durationSeconds=1..60`); returns `202` or `409` if scanning is busy. |
| `GET /ble/discovery` | Return all recently seen BLE candidates, including unrecognized devices, RSSI, manufacturer-data length, service match, callback count, and scan state. |
| `GET /logs?limit=40` | Return recent in-memory firmware state and rate-limited advertisement-sighting logs. |
| `GET /devices` | List configured devices (at most eight). |
| `POST /devices` | Add a device using `address`, `protocol`, and optional `name`; API key required. |
| `GET /devices/{id}` | Get device state; keys are not returned. |
| `PUT /devices/{id}` | Update `name` and/or provision a 32-character hex Victron `key`; API key required. |
| `DELETE /devices/{id}` | Remove a device; API key required. |
| `GET /devices/{id}/telemetry` | Read current GreenPower GATT telemetry or latest Victron advertisement. |
| `POST /devices/{id}/reconnect` | Connect a GreenPower GATT device; advertisement-only devices return `405`. |
| `GET /devices/{id}/registers?...` | GreenPower raw-register read for the active GATT device. |
| `PUT /devices/{id}/registers/{address}` | GreenPower raw-register write; API key required. |
| `GET/PUT /devices/{id}/config/{kind}` | GreenPower named config operations; writes require the API key. |

The device ID is its normalized BLE address without colons (for example,
`aabbccddeeff`). `protocol` is `greenpower` or `victron`. A Victron `key` is
the 16-byte Instant Readout key shown in VictronConnect, represented as 32 hex
characters. The key can be supplied during create or later with `PUT`.

The advertisement's little-endian product ID is retained in discovery results.
Product ID `0xA3A0` is labeled `VE.Bus Smart Dongle`; its encrypted payload uses
record type `0x0C` (`ve_bus`).

Configuration is persisted as a versioned fixed-size registry in ESP32 NVS.
Key values are stored as ordinary NVS bytes by this firmware, not encrypted.
The REST server is plain HTTP; API-key authentication does not encrypt the key
in transit. Use only on a trusted network.

## Transport limits

Multiple Victron advertisements can be received in each scan and matched
against configured device addresses. GreenPower remains a GATT/Modbus device;
this firmware keeps a single active GreenPower GATT client. A configured
GreenPower device must be selected with its reconnect endpoint before its
register/config/telemetry routes are used. These legacy global endpoints
remain available for compatibility:

- `GET /api/v1/health`
- `GET /api/v1/status`
- `POST /api/v1/ble/reconnect`
- `GET /api/v1/live`
- `GET /api/v1/registers`
- `PUT /api/v1/registers/{address}`
- `GET/PUT /api/v1/config/{kind}`
