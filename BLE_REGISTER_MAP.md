# GreenPower BLE Register Map

This document records the GreenPower Modbus-over-BLE register mappings used by
the ESP32 relay. The transport details are documented in the repository-level
[`BLE_DEVICE_SPEC.md`](../BLE_DEVICE_SPEC.md).

## Runtime registers

All values are big-endian 16-bit Modbus holding registers unless noted
otherwise. The runtime blocks are read by `GET /api/v1/live`.

| Address | Name | Decoding |
|---|---|---|
| `0x0003` | `errorCode` | Error bitfield; see error values below |
| `0x0004` | `batteryStatus` | `0=normal`, `1=undervoltage`, `2=float charge`, `3=full`, `4=overvoltage` |
| `0x0005` | `batteryPercent` | Integer percent |
| `0x0007` | `batteryVoltage` | Raw `/ 10`, V |
| `0x0008` | `batteryCurrent` | Raw `/ 10`, A |
| `0x0009` | `pvDay` | `0=night`, nonzero=day |
| `0x000A` | `pvVoltage` | Raw `/ 10`, V |
| `0x000B` | `pvCurrent` | Raw `/ 10`, A |
| `0x000C` | `fanVoltage` | Raw `/ 10`, V |
| `0x000D` | `fanCurrent` | Raw `/ 10`, A |
| `0x000E` | `fanUnloadCurrent` | Raw `/ 10`, A |
| `0x000F` | `workTemperature` | `(raw - 500) / 10`, °C; `9999=abnormal` |
| `0x0010` | `batteryTemperature` | `(raw - 500) / 10`, °C; `9999=disabled` |
| `0x001D` | `fanRpm` | Integer RPM |
| `0x001E` | `outputVoltage` | Raw `/ 10`, V |
| `0x001F` | `outputCurrent` | Raw `/ 10`, A; verify against hardware captures |
| `0x0027` | `pvPower` | Integer W |
| `0x0028` | `fanPower` | Integer W |
| `0x0029` | `batteryPower` | Integer W |
| `0x002A` | `outputPower` | Integer W |
| `0x002B` | `fanUnloadPower` | Integer W |
| `0x002C` | `fanWindSpeed` | Raw `/ 100` |
| `0x0030`-`0x0033` | daily energy | Raw `/ 100` |
| `0x0034`-`0x0037` | 30-day energy | Raw `/ 10` |
| `0x0038`-`0x003F` | cumulative energy | Unsigned 32-bit big-endian pairs |

Error bit values observed in the Android application:

| Value | Name |
|---:|---|
| `0` | normal |
| `2` | faulty charging circuit |
| `4` | battery not connected |
| `8` | load short |
| `16` | PV overvoltage |
| `32` | load overvoltage |
| `64` | temperature failure |
| `128` | storage failure |

## Configuration blocks

The named configuration endpoints are:

```text
GET /api/v1/config/battery
PUT /api/v1/config/battery
GET /api/v1/config/system
PUT /api/v1/config/system
GET /api/v1/config/pv
PUT /api/v1/config/pv
GET /api/v1/config/fan
PUT /api/v1/config/fan
GET /api/v1/config/output
PUT /api/v1/config/output
```

Each endpoint reads or writes the corresponding documented register block.
Writes require the existing `X-Api-Key` header. Unknown JSON properties are
rejected rather than silently ignored.

### Battery (`0x1001`, 10 registers)

| Register | JSON property | Decoding |
|---|---|---|
| `0x1001` | `capacityAh` | Integer Ah |
| `0x1002` | `ratedVoltage` | Integer V |
| `0x1003`-`0x1009` | battery voltage thresholds | Raw `/ 10`, V |
| `0x100A` | `batteryType` | `0=custom`, `1=VRLA`, `2=NCM2`, `3=NCM3`, `4=LiFePO4` |

The Android source exposes the remaining fields as charge/discharge and
recovery/over-voltage thresholds. Their exact product labels vary by firmware;
the raw register values remain available until confirmed on hardware.

### System (`0x100B`, 5 registers)

| Register | JSON property | Decoding |
|---|---|---|
| `0x100B` | `temperatureCompensation` | Raw value |
| `0x100C` | `rs485Address` | Integer |
| `0x100D` | `systemType` | Android system-voltage selection |
| `0x100E` | `language` | Android language index |
| `0x100F` | `reserved` | Raw |

The Android decompilation has inconsistent field naming for this block, so
system writes should be treated as advanced settings.

### PV (`0x2001`, 5 registers)

| Register | JSON property | Decoding |
|---|---|---|
| `0x2001` | `openVoltage` | Raw `/ 10`, V |
| `0x2002` | `closeVoltage` | Raw `/ 10`, V |
| `0x2003` | `chargingEnabled` | `0/1` |
| `0x2004` | `mpptEnabled` | `0/1` |
| `0x2005` | `maxChargingCurrent` | Raw `/ 10`, A |

### Fan (`0x3001`, 14 registers)

| Register | JSON property | Decoding |
|---|---|---|
| `0x3001` | `chargingEnabled` | `0/1` |
| `0x3002` | `mpptEnabled` | `0/1` |
| `0x3003` | `unloadEnabled` | `0/1` |
| `0x3004` | `maxVoltage` | Integer V |
| `0x3005` | `maxCurrent` | Raw `/ 10`, A |
| `0x3006` | `maxRpm` | Integer RPM |
| `0x3007` | `polePairs` | Integer |
| `0x3008` | `mpptCutInVoltage` | Integer V |
| `0x3009` | `mpptCutInRpm` | Integer RPM |
| `0x300A` | `unloadDelayMinutes` | Integer minutes |
| `0x300B` | `ratedRpm` | Integer RPM |
| `0x300C` | `ratedVoltage` | Integer V |
| `0x300D` | `mpptAdjustmentFactor` | Raw value |
| `0x300E` | `mpptMaxVoltage` | Integer V |

Fan curve blocks are `0x300F` and `0x3023`, each containing 20 curve
registers. Their voltage/power item pairing should be verified against the
device firmware before exposing named writes.

### Output (`0x4001`, 31 registers)

| Register | JSON property | Decoding |
|---|---|---|
| `0x4001` | `mode` | `1=light control`, `2=normally on`, `3=time control` |
| `0x4002` | `enabled` | `0/1` |
| `0x4009` | `protectionCurrent` | Raw `/ 10`, A |
| `0x400A` | `ratedCurrent` | Raw `/ 10`, A |
| `0x400B` | `ratedVoltage` | Integer V |
| `0x400C` | `ratedPower` | Integer W |
| `0x4010` | `energyEnabled` | `0/1` |
| `0x4012` | `energyVoltage` | Raw `/ 10`, V |
| `0x4014` | `lowVoltage` | Raw `/ 10`, V |
| `0x4015` | `lowRecoveryVoltage` | Raw `/ 10`, V |

Scheduled power/time fields remain available in the raw `registers` array
until their exact labels and units are confirmed against hardware.

## Raw compatibility API

The existing diagnostic endpoints remain supported:

```text
GET /api/v1/registers?start=0x0003&count=28
PUT /api/v1/registers/0x3001
```

The named endpoints are an additive API. Raw register access is retained for
undocumented fields, diagnostics, and future device variants.
