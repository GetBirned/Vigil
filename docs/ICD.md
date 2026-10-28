# Interface Control Document: Vigil UDP Messages

| Item | Value |
|---|---|
| Document | VIGIL-ICD-001 |
| Revision | 2 (protocol version 1) |
| Interfaces | IF-1 sensor → tracker (Detection Report) · IF-2 tracker → display (Detection Report + Track Report) |
| Source of truth in code | `common/include/vigil/messages.hpp`, `common/src/messages.cpp` (C++), `display/src/shared/protocol.ts` (TypeScript) |

## 1. Interfaces and transport

| Property | IF-1: sensor → tracker | IF-2: tracker → display |
|---|---|---|
| Producer | `vigil-sim`, or any sensor that follows this document | `vigil-tracker` |
| Consumer | `vigil-tracker` | `vigil-display` gateway |
| Protocol | UDP over IPv4, one message per datagram | UDP over IPv4, one message per datagram |
| Default destination | `127.0.0.1:50000` | `127.0.0.1:50001` |
| Messages | Detection Report (type 1) | Detection Report (type 1), then Track Report (type 2), for every scan processed |
| Max datagram payload | 1472 bytes (one 1500-byte Ethernet frame, no IP fragmentation) | 1472 bytes for type 1; 65,507 bytes for type 2 (may fragment at the IP layer) |
| Rate | One message per sensor scan (default 1 Hz) | Two messages per scan processed |
| Delivery | Best effort. Receivers detect loss with `seq` and must tolerate reordering, duplication and loss. | Same |

On IF-2 the tracker forwards the Detection Report it received unchanged, then sends the Track
Report for that scan with the **same `seq`**. A display pairs them by `seq`. If the Detection
Report is lost, the display still shows the tracks.

## 2. Encoding rules

- All integers are **unsigned, little-endian**.
- All floating-point fields are **IEEE 754, little-endian**: f64 = binary64, f32 = binary32.
- No padding between fields. Offsets are in bytes from the start of the datagram or record.
- Coordinates are in the sensor's local tangent plane: **x = east, y = north, metres**. The
  sensor is at the origin.
- Fields marked *reserved* are sent as zero and ignored on receipt.

## 3. Common header (24 bytes, every message)

| Offset | Size | Type | Field | Description |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `magic` | `0x56474C31` ("VGL1"). Bytes on the wire: `31 4C 47 56` |
| 4 | 2 | u16 | `version` | Protocol version. This document describes `1` |
| 6 | 2 | u16 | `msg_type` | `1` = Detection Report, `2` = Track Report |
| 8 | 4 | u32 | `seq` | Type 1: increments by 1 per report, wraps at 2³²; `0` after traffic means the sensor restarted. Type 2: the `seq` of the Detection Report that produced it |
| 12 | 2 | u16 | `source_id` | Type 1: the sensor ID. Type 2: the tracker ID |
| 14 | 2 | u16 | `count` | Number of records that follow |
| 16 | 8 | u64 | `timestamp_ns` | Scan time, nanoseconds since scenario start |

## 4. Detection Report (type 1)

### 4.1 Detection record (16 bytes, repeated `count` times, starting at offset 24)

| Offset | Size | Type | Field | Units |
|---:|---:|---|---|---|
| 0 | 8 | f64 | `x_m` | metres east |
| 8 | 8 | f64 | `y_m` | metres north |

Detections carry no target identity. Their order within a report is arbitrary.

### 4.2 Size

`datagram size = 24 + 16 × count`. With a 1472-byte limit, `count ≤ 90`. A producer with more
detections in one scan must drop the excess. Version 1 has no fragmentation.

## 5. Track Report (type 2)

### 5.1 Scan summary (8 bytes, at offset 24)

| Offset | Size | Type | Field | Description |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `processing_ns` | Time the tracker spent on this scan |
| 4 | 2 | u16 | `detections_in_scan` | Detections in the scan that produced this picture |
| 6 | 2 | u16 | *reserved* | |

### 5.2 Track record (48 bytes, repeated `count` times, starting at offset 32)

| Offset | Size | Type | Field | Description |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `track_id` | Unique for the life of the tracker process; never reused |
| 4 | 1 | u8 | `status` | `0` = tentative, `1` = confirmed, `2` = coasting |
| 5 | 1 | u8 | *reserved* | |
| 6 | 2 | u16 | `hits` | Detections associated so far (saturates at 65,535) |
| 8 | 8 | f64 | `x_m` | Estimated position, metres east |
| 16 | 8 | f64 | `y_m` | Estimated position, metres north |
| 24 | 8 | f64 | `vx_mps` | Estimated velocity, metres/second east |
| 32 | 8 | f64 | `vy_mps` | Estimated velocity, metres/second north |
| 40 | 4 | f32 | `position_sigma_m` | RMS 1-sigma position uncertainty, √((Pxx + Pyy) / 2) |
| 44 | 4 | f32 | `velocity_sigma_mps` | RMS 1-sigma velocity uncertainty, √((Pvxvx + Pvyvy) / 2) |

The report holds every track the tracker currently has, tentative ones included. A track
missing from a report has been dropped.

### 5.3 Size

`datagram size = 24 + 8 + 48 × count`. With a 65,507-byte limit, `count ≤ 1364`.

## 6. Validation (receiver behaviour)

A receiver must discard the whole datagram, and count it as malformed, if any check fails:

| Check | Error |
|---|---|
| Datagram shorter than 24 bytes | `kTooShort` |
| `magic` ≠ `0x56474C31` | `kBadMagic` |
| `version` ≠ 1 | `kUnsupportedVersion` |
| `msg_type` is not one the receiver expects | `kUnknownMessageType` |
| Type 1 length ≠ `24 + 16 × count`, or type 2 length ≠ `32 + 48 × count` | `kLengthMismatch` |
| Any floating-point field is NaN or ±∞ | `kNonFiniteValue` |
| Track `status` > 2 | `kBadTrackStatus` |

The tracker discards Detection Reports whose `timestamp_ns` is older than the last report it
processed, and counts them as out of order.

## 7. Example

A Detection Report with `seq = 0xDEADBEEF`, `sensor_id = 7`, one detection at (1.5, −2.25):

```
31 4C 47 56  01 00  01 00  EF BE AD DE  07 00  01 00  <8 bytes timestamp>
00 00 00 00 00 00 F8 3F   00 00 00 00 00 00 02 C0
```

## 8. Revision history

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-10-27 | Initial release: IF-1, Detection Report |
| 2 | 2026-10-28 | Added IF-2 and the Track Report (type 2). Protocol version unchanged: type 1 is identical |
