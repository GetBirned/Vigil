# Interface Control Document: Vigil UDP Messages

| Item | Value |
|---|---|
| Document | VIGIL-ICD-001 |
| Revision | 3 (protocol version 1, envelope version 1) |
| Interfaces | IF-1 sensor → tracker (Detection Report) · IF-2 tracker → display (Detection Report + Track Report) |
| Source of truth in code | `common/include/vigil/messages.hpp`, `common/src/messages.cpp` (C++), `display/src/shared/protocol.ts` (TypeScript); authentication: `common/include/vigil/auth.hpp`, `common/src/auth.cpp`, `display/server/auth.ts` |

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

`datagram size = 24 + 16 × count`. With a 1472-byte limit, `count ≤ 90`, or `count ≤ 88` inside
an authenticated envelope (section 7), which adds 40 bytes. A producer with more detections in
one scan must drop the excess. Version 1 has no fragmentation.

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

## 7. Security: authenticated envelope

### 7.1 Threat model

Both interfaces are plain UDP, so without protection anyone who can reach the receiver's port
can feed it data. The envelope defends against:

| Attacker | Can | Without the envelope | With it |
|---|---|---|---|
| Off-path spoofer | Send datagrams to the port; sees no traffic | Injects ghost targets. One report with a far-future `timestamp_ns` makes the tracker discard every genuine report after it as out of order. | Rejected: not authenticated, or wrong key |
| On-path (man in the middle) | Read, modify, delay, reorder, replay and drop traffic | Adds or moves detections; replays old scans | Modified: tag mismatch. Replayed: replay window. |
| Replay after restart | Re-send traffic captured in an earlier run | Restarts the picture (a report with `seq = 0`) | Rejected: stale epoch |

Out of scope: **confidentiality** (payloads are readable; add DTLS or IPsec if they must not
be), **denial of service** (an on-path attacker can always drop traffic; authentication turns
tampering into loss, not deception), and **key distribution** (the key is provisioned out of
band as a file).

### 7.2 Format

Each message from sections 3 to 5 travels unchanged as the payload of an envelope:

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 4 | u32 | `magic` | `0x56474C53` ("VGLS" when read as little-endian bytes `53 4C 47 56`) |
| 4 | 2 | u16 | `env_version` | 1 |
| 6 | 2 | u16 | `key_id` | First 2 bytes (little-endian) of HMAC-SHA256(key, ASCII `vigil key id`) |
| 8 | 8 | u64 | `epoch` | Sender's start time, ns since the Unix epoch. Must increase across restarts. |
| 16 | 8 | u64 | `counter` | 0 for the first datagram of an epoch, then +1 per datagram |
| 24 | n | bytes | `payload` | One message, exactly as in sections 3 to 5 |
| 24 + n | 16 | bytes | `tag` | HMAC-SHA256(key, bytes 0 to 24 + n − 1), first 16 bytes |

The key is 32 random bytes, stored as 64 hex digits in a file only its owner can read
(`openssl rand -hex 32`, mode 0600). The tag is truncated to 128 bits, as in IPsec
(RFC 4868). On IF-2 the tracker seals with its own epoch and counter: it re-seals the Detection
Report it forwards rather than passing the sensor's envelope on.

### 7.3 Receiver behaviour

Checks run in this order. A datagram that fails any of them is discarded and counted by reason,
and nothing about the receiver's state changes:

| # | Check | Error |
|---|---|---|
| 1 | Starts with the envelope `magic` | not authenticated |
| 2 | At least 40 bytes | too short |
| 3 | `env_version` = 1 | unsupported envelope version |
| 4 | `key_id` matches the receiver's key | wrong key |
| 5 | (`epoch`, `counter`) passes the replay window | stale epoch, too old, replayed |
| 6 | `tag` matches, compared in **constant time** | bad tag |

Only after check 6 does the replay window record the datagram, so forged traffic can never move
it. The window follows RFC 4303 section 3.4.3: it remembers the highest `counter` accepted and a
64-bit bitmap of the 63 before it. A counter above the top is accepted and slides the window. A
counter inside it is accepted once. Anything older is rejected. A higher `epoch` means the
sender restarted, and resets the window. A lower one is rejected.

The payload is then validated as in section 6. Rejections are logged at most once per second,
so a flood of forgeries cannot flood the log. `vigil-tracker` authenticates on its receive
thread, before the queue, so forgeries cost one HMAC (about 1 µs) and never displace genuine
reports from the queue. Running without authentication requires an explicit `--insecure`.

### 7.4 Example

Key `00 01 02 … 1F` (`key_id` = `0x1495`), `epoch` = 1, `counter` = 0, payload ASCII `abc`:

```
53 4C 47 56  01 00  95 14  01 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
61 62 63
63 35 89 50 B8 13 E9 54 BC D5 C0 7A 59 DA 7D 7E
```

The C++ and TypeScript tests both check this vector, which was generated independently with
Python's `hmac` module.

## 8. Example

A Detection Report with `seq = 0xDEADBEEF`, `sensor_id = 7`, one detection at (1.5, −2.25):

```
31 4C 47 56  01 00  01 00  EF BE AD DE  07 00  01 00  <8 bytes timestamp>
00 00 00 00 00 00 F8 3F   00 00 00 00 00 00 02 C0
```

## 9. Revision history

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-10-27 | Initial release: IF-1, Detection Report |
| 2 | 2026-10-28 | Added IF-2 and the Track Report (type 2). Protocol version unchanged: type 1 is identical |
| 3 | 2026-10-28 | Added the authenticated envelope and threat model (section 7). Messages unchanged |
