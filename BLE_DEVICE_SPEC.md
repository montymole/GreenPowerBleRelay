# GreenPower BLE Device Protocol Spec

This is an implementation-neutral spec for building a new client app. It focuses on what to scan for, what GATT attributes to use, and what bytes to exchange. Source file names are listed only as provenance.

## BLE GATT Profile

Primary device-control BLE profile:

| Purpose | UUID | Notes |
|---|---|---|
| Service | `0000FFB0-0000-1000-8000-00805F9B34FB` | Scan filter and GATT service |
| Write characteristic | `0000FFB1-0000-1000-8000-00805F9B34FB` | App writes command bytes here |
| Read/notify characteristic | `0000FFB2-0000-1000-8000-00805F9B34FB` | App enables notifications here; direct read is used only in scan/edit code |

Source: `utils/BluetoothUuidUtil.java`.

BLE settings used by the app:

- Scan filter: service UUID `FFB0`.
- Scan timeout: 10 seconds.
- Connect timeout: 5 seconds.
- Operation timeout: 5 seconds.
- BLE write splitting: 20-byte chunks.
- Reconnect count in library: 0; the app implements its own delayed reconnect.

Implementation note: treat characteristic `FFB1` as the TX/write pipe and `FFB2` as the RX/notify pipe.

## Connection Sequence

1. Scan for BLE peripherals advertising service `0000FFB0-0000-1000-8000-00805F9B34FB`.
2. Connect to the selected peripheral.
3. Enable notifications on service `FFB0`, characteristic `FFB2`.
4. Enter module AT mode by writing ASCII `+++` to characteristic `FFB1`.
5. Wait briefly or wait for an AT response on `FFB2`.
6. Exit AT mode by writing ASCII `AT+exit\r\n`.
7. Read the Modbus address with the fixed frame:

   ```text
   FF 03 F0 01 00 01 F3 14
   ```

8. Use the returned first byte as the Modbus slave address for all subsequent Modbus RTU frames. The app defaults to address `0x02` before this read.
9. Optionally read password register `0xE00A` and visible-parameter flags `0xE001`.
10. Read runtime/config registers by writing Modbus RTU request frames to `FFB1`; collect responses from notifications on `FFB2`.

On reconnect, the Android app skips the AT enter/exit step and starts at reading the Modbus address. A new app can do the same after a known-good first connection.

## Transport Payload

The BLE payload is Modbus RTU-like:

### Read Holding Registers

Function code: `0x03`.

Request format:

```text
[slave][03][addr_hi][addr_lo][count_hi][count_lo][crc_lo][crc_hi]
```

The app builds this as:

```text
slave = current Modbus address
count_hi = (count >> 8) & 0xff
count_lo = count & 0xff
crc = CRC16/MODBUS over first 6 bytes, little-endian in frame
```

Example: read 8 registers at `0xE001`, with slave `0x02`:

```text
02 03 E0 01 00 08 CRC_LO CRC_HI
```

Exact packet builder:

```text
read(slave, address, count):
  body = [
    slave,
    0x03,
    (address >> 8) & 0xff,
    address & 0xff,
    (count >> 8) & 0xff,
    count & 0xff
  ]
  return body + crc16_modbus(body)
```

### Write Single Register

Function code: `0x06`.

Request format:

```text
[slave][06][addr_hi][addr_lo][value_hi][value_lo][crc_lo][crc_hi]
```

Success response is the same 8-byte frame echoed by the device with function code `0x06`.

Exact packet builder:

```text
write_single(slave, address, value):
  body = [
    slave,
    0x06,
    (address >> 8) & 0xff,
    address & 0xff,
    (value >> 8) & 0xff,
    value & 0xff
  ]
  return body + crc16_modbus(body)
```

### CRC16

CRC is standard Modbus CRC16:

- Initial value: `0xFFFF`
- Polynomial: `0xA001`
- Output byte order: low byte then high byte

Reference implementation:

```text
crc16_modbus(bytes):
  crc = 0xffff
  for b in bytes:
    crc = crc XOR b
    repeat 8 times:
      if (crc & 1) != 0:
        crc = (crc >> 1) XOR 0xa001
      else:
        crc = crc >> 1
  return [crc & 0xff, (crc >> 8) & 0xff]
```

## Response Parsing

Read response format:

```text
[slave][03][byte_count][data...][crc_lo][crc_hi]
```

Expected total response length is:

```text
byte_count + 5
```

Notifications can split or combine frames. Buffer bytes from `FFB2` until a full frame is available:

```text
minimum frame length = 5
if frame[1] == 0x03:
  total_len = frame[2] + 5
if frame[1] == 0x06:
  total_len = 8
```

Validate CRC before decoding. The original app does not visibly validate CRC in its direct BLE parser, but a new app should.

Error response observed/handled:

```text
[slave][86][03][crc_lo][crc_hi]
```

This is treated as "command exception".

## Important Registers

Read groups used by the direct BLE device screen:

| Name | Start | Count | Expected response length | Purpose |
|---|---:|---:|---:|---|
| Modbus address | `0xF001` | 1 | 7 | Uses broadcast-ish slave `0xFF`; fixed frame above |
| Visible flags | `0xE001` | 8 | 21 | Which UI parameters/settings are visible |
| Password | `0xE00A` | 1 | 7 | 4-digit password if value `< 10000` |
| Runtime block 1 | `0x0003` | 28 | 61 | temperatures, status, voltages/currents |
| Runtime block 2 | `0x0027` | 25 | 55 | powers and energy counters |
| System/status block | `0x1001` | 14 | 33 | system mode text |
| Output mode | `0x4001` | 1 | 7 | output mode |
| Battery config | `0x1001` | 10 | 25 | battery settings |
| System config | `0x100B` | 5 | 15 | language/system/address fields |
| PV config | `0x2001` | 5 | 15 | PV/fan charge settings |
| Fan config | `0x3001` | 14 | 33 | fan settings |
| Fan curve 0-10 | `0x300F` | 20 | 45 | first half of fan curve |
| Fan curve 10-20 | `0x3023` | 20 | 45 | second half of fan curve |
| Output config | `0x4001` | 31 | 67 | output settings |

The first response data byte is at response index `3`. For a read of `N` registers, `byte_count = N * 2`.

Writeable registers seen in the app:

| Area | Registers |
|---|---|
| Battery/system | `0x1001`, `0x1003`-`0x100B`, `0x100C` Modbus address, `0x100D`-`0x1010` operations |
| Password | `0xE00A` |
| PV | `0x2001`-`0x2005` |
| Fan | `0x3001`-`0x300E`, plus fan curve item addresses from the loaded curve data |
| Output | `0x4001`, `0x4002`, `0x4009`-`0x4010`, `0x4012`, `0x4014`, `0x4015`, `0x4018`-`0x401F` |

## Runtime Scaling

The main runtime reads decode big-endian register values from the data portion of the Modbus response.

For `0x0003`, count 28:

| Data offset | Register | Meaning | Scaling |
|---:|---:|---|---|
| 0 | `0x0003` | Error code | enum/bitfield |
| 2 | `0x0004` | Battery status | enum |
| 4 | `0x0005` | Battery percentage | integer percent |
| 8 | `0x0007` | Battery voltage | `/ 10` |
| 10 | `0x0008` | Battery current | `/ 10` |
| 12 | `0x0009` | PV day/night | `0 = night`, nonzero = day |
| 14 | `0x000A` | PV voltage | `/ 10` |
| 16 | `0x000B` | PV current | `/ 10` |
| 18 | `0x000C` | Fan voltage | `/ 10` |
| 20 | `0x000D` | Fan current | `/ 10` |
| 22 | `0x000E` | Fan unload current | `/ 10` |
| 24 | `0x000F` | Work temperature | `(value - 500) / 10`; `9999 = abnormal` |
| 26 | `0x0010` | Battery temperature | `(value - 500) / 10`; `9999 = disabled` |
| 52 | `0x001D` | Fan RPM | integer |
| 54 | `0x001E` | Output voltage | `/ 10` |

The Android code also maps output current from response bytes 57-58 in this block, which corresponds to register `0x001F`; this is just outside the nominal `0x0003` count-28 range and may be a decompiler/indexing artifact. Verify with real device captures.

For `0x0027`, count 25:

| Data offset | Register | Meaning | Scaling |
|---:|---:|---|---|
| 0 | `0x0027` | PV power | integer |
| 2 | `0x0028` | Fan power | integer |
| 4 | `0x0029` | Battery power | integer |
| 6 | `0x002A` | Output power | integer |
| 8 | `0x002B` | Fan unload power | integer |
| 10 | `0x002C` | Fan wind speed | `/ 100` |
| 18 | `0x0030` | PV daily energy | `/ 100` |
| 20 | `0x0031` | Fan daily energy | `/ 100` |
| 22 | `0x0032` | Battery daily energy | `/ 100` |
| 24 | `0x0033` | Output daily energy | `/ 100` |
| 26 | `0x0034` | PV 30-day energy | `/ 10` |
| 28 | `0x0035` | Fan 30-day energy | `/ 10` |
| 30 | `0x0036` | Battery 30-day energy | `/ 10` |
| 32 | `0x0037` | Output 30-day energy | `/ 10` |
| 34 | `0x0038`/`0x0039` | PV cumulative energy | unsigned 32-bit big-endian |
| 38 | `0x003A`/`0x003B` | Fan cumulative energy | unsigned 32-bit big-endian |
| 42 | `0x003C`/`0x003D` | Battery cumulative energy | unsigned 32-bit big-endian |
| 46 | `0x003E`/`0x003F` | Output cumulative energy | unsigned 32-bit big-endian |

Note: offsets above are expressed relative to the Modbus data section, not the whole response. The original code often indexes the whole response, whose data starts at byte 3.

## AT Commands

AT mode uses the same BLE service/write/read characteristics, but payloads are ASCII instead of Modbus RTU:

| Operation | Write payload |
|---|---|
| Enter AT mode | `+++` |
| Exit AT mode | `AT+exit\r\n` |
| Set BLE name | `AT+setName=<name>\r\n` |
| Restart module | `AT+reStart\r\n` |

AT responses are newline-terminated with `\r\n`.

## Wi-Fi Provisioning Profile

There is a separate BLE profile for Wi-Fi provisioning, apparently for an HF-LPX70 style module:

| Purpose | UUID |
|---|---|
| Service | `0000FEE7-0000-1000-8000-00805F9B34FB` |
| Write | `0000FEC7-0000-1000-8000-00805F9B34FB` |
| Notify | `0000FEC8-0000-1000-8000-00805F9B34FB` |
| AT characteristic constant | `0000FED4-0000-1000-8000-00805F9B34FB` |

Provisioning frame format:

- Plain config payload: `[ssid_len][ssid bytes][password_len][password bytes][0][crc8_maxim]`
- Encrypt using TEA-like 8-round block operation with key ASCII `hiflying12345678`; trailing non-8-byte remainder is copied unchanged.
- Split encrypted bytes into BLE frames of max 17-byte payload:

  ```text
  [frame_index_1_based][total_frames][payload_len][payload...]
  ```

- After config writes, the app writes ASCII `config_ack`.
- Device response frames use the same `[index][total][len][payload]` wrapper.
- Reassembled response begins `[0xFE][payload_len]`, then TEA-encrypted JSON.

## Minimal Client Pseudocode

```text
scan(service=FFB0)
connect(device)
subscribe(service=FFB0, characteristic=FFB2)
write(service=FFB0, characteristic=FFB1, bytes="+++")
wait ~200ms or wait for AT response
write(service=FFB0, characteristic=FFB1, bytes="AT+exit\r\n")
wait for completion

write(FFB1, hex("FF 03 F0 01 00 01 F3 14"))
resp = wait_modbus_response()
slave = resp[0]

frame = modbus_read(slave, 0x0003, 28)
write(FFB1, frame)
resp = wait_modbus_response()
validate_crc(resp)
decode_registers(resp[3 : 3 + resp[2]])
```

## Source Landmarks

- UUID constants: `com/wolandoo/lsmppt/utils/BluetoothUuidUtil.java`
- Scan/connect settings: `com/wolandoo/lsmppt/ScanBleActivity.java`
- Direct BLE connection/write/notify protocol: `com/wolandoo/lsmppt/DeviceActivity.java`
- CRC16 and integer conversion helpers: `com/wolandoo/lsmppt/utils/ByteHelper.java`
- Config register usage: `BatteryConfigActivity`, `PvConfigActivity`, `FanConfigActivity`, `OutputConfigActivity`, `SystemConfigActivity`
- Wi-Fi provisioning encryption/framing: `com/wolandoo/lsmppt/utils/SmartBLELinkHelper.java`
