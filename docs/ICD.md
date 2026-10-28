# Interface Control Document: Sensor → Tracker Detection Report

| Item | Value |
|---|---|
| Document | VIGIL-ICD-001 |
| Revision | 1 (protocol version 1) |
| Producer | `vigil-sim` (any sensor that follows this document) |
| Consumer | `vigil-tracker` |
| Source of truth in code | `common/include/vigil/messages.hpp`, `common/src/messages.cpp` |

## 1. Transport

| Property | Value |
|---|---|
| Protocol | UDP over IPv4, one report per datagram |
| Default destination | `127.0.0.1:50000` (configurable on both sides) |
| Max datagram payload | 1472 bytes (fits a 1500-byte Ethernet MTU with no IP fragmentation) |
| Delivery | Best effort. Receivers detect loss with the sequence number and must tolerate reordering, duplication and loss. |
| Rate | One report per sensor scan (default 1 Hz) |

## 2. Encoding rules

- All integers are **unsigned, little-endian**.
- All floating-point fields are **IEEE 754 binary64, little-endian**.
- No padding between fields. Offsets are in bytes from the start of the datagram.
- Coordinates are in the sensor's local tangent plane: **x = east, y = north, metres**.

## 3. Message layout: Detection Report (type 1)

### 3.1 Header (24 bytes)

| Offset | Size | Type | Field | Description |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `magic` | `0x56474C31` ("VGL1"). Bytes on the wire: `31 4C 47 56` |
| 4 | 2 | u16 | `version` | Protocol version. This document describes `1` |
| 6 | 2 | u16 | `msg_type` | `1` = Detection Report |
| 8 | 4 | u32 | `seq` | Increments by 1 per report, wraps at 2³². `0` after traffic means the sensor restarted |
| 12 | 2 | u16 | `sensor_id` | Identifies the producing sensor |
| 14 | 2 | u16 | `count` | Number of detection records that follow (0–90) |
| 16 | 8 | u64 | `timestamp_ns` | Scan time, nanoseconds since scenario start |

### 3.2 Detection record (16 bytes, repeated `count` times)

| Offset (within record) | Size | Type | Field | Units |
|---:|---:|---|---|---|
| 0 | 8 | f64 | `x_m` | metres east |
| 8 | 8 | f64 | `y_m` | metres north |

Detections carry no target identity. Their order within a report is arbitrary.

### 3.3 Size

`datagram size = 24 + 16 × count`. With a 1472-byte limit, `count ≤ 90`. A producer with more
detections in one scan must drop the excess. Version 1 has no fragmentation.

## 4. Validation (receiver behaviour)

A receiver must discard the whole datagram, and count it as malformed, if any check fails:

| Check | Error |
|---|---|
| Datagram shorter than 24 bytes | `kTooShort` |
| `magic` ≠ `0x56474C31` | `kBadMagic` |
| `version` ≠ 1 | `kUnsupportedVersion` |
| `msg_type` ≠ 1 | `kUnknownMessageType` |
| Datagram length ≠ `24 + 16 × count` | `kLengthMismatch` |
| Any coordinate is NaN or ±∞ | `kNonFiniteValue` |

Reports whose `timestamp_ns` is older than the last report the tracker processed are discarded
as out of order.

## 5. Example

A report with `seq = 0xDEADBEEF`, `sensor_id = 7`, one detection at (1.5, −2.25):

```
31 4C 47 56  01 00  01 00  EF BE AD DE  07 00  01 00  <8 bytes timestamp>
00 00 00 00 00 00 F8 3F   00 00 00 00 00 00 02 C0
```

## 6. Revision history

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-10-27 | Initial release |
