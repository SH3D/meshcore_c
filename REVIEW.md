# meshcore_c vs meshcore_py — Feature Gap Analysis

Date: 2026-06-08

## Summary

`meshcore_c` is a clean, well-structured portable C99 core + Arduino C++ wrapper for
the MeshCore Companion Radio **serial** protocol. It correctly implements the
wire-level framing (0x3C/0x3E, LE length), handshake, channel text/data
send/receive, PSK programming, and basic radio parameters — the minimum needed
for a display board or sensor hub to talk to a companion radio. The code is
concise, has zero external dependencies in the C core, and ships with four
platform examples (Linux POSIX, ESP-IDF, STM32 HAL, Arduino).

Compared to the reference Python library `meshcore_py`, this C implementation
covers roughly **30%** of the protocol surface. The gaps fall naturally into
three categories below.

---

## MAJOR GAPS (core protocol missing — things that prevent important use cases)

### 1. Contact Management — ENTIRELY MISSING
*meshcore_py* has an extensive contacts subsystem: fetch with delta sync
(`lastmod`), add/update, remove, share, import/export (meshcore:// URI), get by
public key, change path, change flags, autoadd config. None of this exists in
`meshcore_c`.

| Feature | meshcore\_py | meshcore\_c |
|---|---|---|
| GET\_CONTACTS (with lastmod sync) | ✅ | ❌ |
| ADD\_UPDATE\_CONTACT | ✅ | ❌ |
| REMOVE\_CONTACT | ✅ | ❌ |
| SHARE\_CONTACT | ✅ | ❌ |
| EXPORT\_CONTACT / IMPORT\_CONTACT | ✅ | ❌ |
| RESET\_PATH | ✅ | ❌ |
| GET\_CONTACT\_BY\_KEY | ✅ | ❌ |
| SET\_ADVERT\_NAME | ✅ | ❌ |
| SET\_ADVERT\_LATLON | ✅ | ❌ |
| GET\_ADVERT\_PATH | ✅ | ❌ |
| SET\_AUTOADD\_CONFIG / GET\_AUTOADD\_CONFIG | ✅ | ❌ |
| GET\_ALLOWED\_REPEAT\_FREQ | ✅ | ❌ |
| SET\_PATH\_HASH\_MODE / GET\_PATH\_HASH\_MODE | ✅ | ❌ |

**Missing commands:** 9, 8, 14, 15, 16, 17, 18, 30, 42, 58, 59, 60, 61
**Missing response parsing:** CONTACT_START, CONTACT, CONTACT_END, CONTACT_URI,
CONTACT_DELETED (0x8F), CONTACTS_FULL (0x90), ADVERT_PATH, AUTOADD_CONFIG,
ALLOWED_REPEAT_FREQ, DEFAULT_FLOOD_SCOPE

**Impact:** No way to manage the mesh node list from C. Critical for any
application that needs to discover and maintain contacts (repeaters, gateways).

### 2. MSG\_SENT Response — NOT PARSED
`meshcore_py` parses `MSG_SENT` (response code 6) which carries:
- `expected_ack` (4 bytes) — tag used to match the ACK confirmation
- `suggested_timeout` (4 bytes) — how long to wait for the ACK

`meshcore_c` has `MC_RESP_SENT = 6` in the enum but **no parsing case** in
`mc_parse()` and **no `send_msg` command builder** (only `send_channel_text`).
The Arduino wrapper's `sendChannelText` sends fire-and-forget.

**Impact:** No way to confirm message delivery. No timeout-driven retry.

### 3. Binary Requests — ENTIRELY MISSING
`meshcore_py` supports `BINARY_REQ` (CMD 50) with subtypes:
- STATUS (node health: battery, uptime, RSSI, SNR, airtime, packet counters)
- TELEMETRY (Cayenne LPP sensor data from remote nodes)
- MMA (min/max/avg telemetry over a time range)
- ACL (access control list — node permissions)
- NEIGHBOURS (list of nodes heard recently, with SNR/age)

And anonymous requests via `SEND_ANON_REQ` (CMD 57):
- REGIONS, OWNER, BASIC (remote clock)

**Missing:**
- `mc_cmd_send_binary_req` builder
- `mc_cmd_send_anon_req` builder
- `MC_RESP_BINARY_RESPONSE` (0x8C) parsing
- Tracking infrastructure (`expected_ack` → `BINARY_RESPONSE` tag matching)

**Impact:** No telemetry interrogation, no remote health checks, no neighbour
discovery. A major gap for monitoring/gateway applications.

### 4. STATS Response — ENUM EXISTS, PARSING MISSING
`mc_cmd_get_stats` builds the command, and `MC_RESP_STATS = 24` is in the enum.
But `mc_parse()` has **no case** for `MC_RESP_STATS`, so requesting stats
returns a raw event code with no parsed data.

`meshcore_py` parses three stat subtypes:
- **Core stats:** battery_mv, uptime_secs, errors, queue_len
- **Radio stats:** noise_floor, last_rssi, last_snr, tx_air_secs, rx_air_secs
- **Packet stats:** recv, sent, flood_tx, direct_tx, flood_rx, direct_rx, recv_errors

**Impact:** Stats command is wired but returns unparseable data. The C struct
definitions for stats need to be added to `mc_event_t`.

### 5. SELF\_INFO Parsing — INCOMPLETE
`meshcore_c`'s `mc_parse` for `MC_RESP_SELF_INFO` reads 55 bytes minimum but
the byte offsets are from an older firmware version. Compared to `meshcore_py`:

| Field | meshcore\_py (current firmware) | meshcore\_c |
|---|---|---|
| `multi_acks` | offset 43 (1 byte) | ❌ treats as reserved |
| `adv_loc_policy` | offset 44 (1 byte) | ❌ treats as reserved |
| `telemetry_mode` | offset 45 (1 byte, decoded to 3×2-bit fields) | ❌ treats as reserved |
| `manual_add_contacts` | offset 46 (1 byte) | ✅ |
| `radio_freq` / `radio_bw` | offsets 47-54 | ✅ |
| `radio_sf` / `radio_cr` | offsets 55-56 | ✅ (conditional guard) |

The C struct `mc_self_info_t` has a `manual_add_contacts` field but **not**
`multi_acks`, `adv_loc_policy`, or the three `telemetry_mode_*` sub-fields.

**Impact:** Self-info data is incomplete. Cannot read/write advanced device
settings through the C API.

### 6. DEVICE\_INFO — MISSING FIELDS
`meshcore_py` parses additional fields for firmware v9+ and v10+:
- `repeat` (repeater mode, fw ≥ 9)
- `path_hash_mode` (fw ≥ 10)
- `ver` string (20-byte version field)
- `max_contacts` correctly applied as `×2` on the wire value

`meshcore_c` applies `×2` to `max_contacts` but does not parse:
- `repeat` flag
- `path_hash_mode`
- `ver` string

**Impact:** Cannot detect repeater mode or path hash configuration from the
device query response.

### 7. CHANNEL\_MSG\_RECV — V3 FORMAT MISSING
Newer MeshCore firmware (v3+) sends `CHANNEL_MSG_RECV_V3` (response 17) which
includes an SNR byte at the start. `meshcore_c` only parses `MC_RESP_CHANNEL_MSG_RECV`
(response 8). The same gap exists for `CONTACT_MSG_RECV_V3` (response 16).

**Impact:** On newer firmware, V3 messages pass through as unknown events with
no SNR or RSSI data available.

---

## MINOR GAPS (feature-completeness items)

### 8. Direct/Contact Message Sending
- **Missing command builders:** `mc_cmd_send_txt_msg` (CMD 2), `mc_cmd_send_cmd` (CMD 2 with txt_type=1)
- **Missing command:** `SEND_LOGIN` (26), `SEND_LOGOUT` (29), `SEND_STATUS_REQ` (27)
- `meshcore_py` has the full message retry flow with `send_msg_with_retry` including automatic path-reset-to-flood fallback.

### 9. Device Configuration Commands
- `SET_DEVICE_PIN` (37) — for BLE PIN pairing
- `SET_OTHER_PARAMS` (38) — telemetry modes, multi_acks, advert policy
- `REBOOT` (19) — simple but essential
- `FACTORY_RESET` (51) — with two-step safety pattern
- `SET_TUNING_PARAMS` (21) / `GET_TUNING_PARAMS` (43) — rx_delay, airtime_factor
- `HAS_CONNECTION` (28) — check connectivity

### 10. Crypto & Security
- `EXPORT_PRIVATE_KEY` (23) / `IMPORT_PRIVATE_KEY` (24)
- `SIGN_START` (33) / `SIGN_DATA` (34) / `SIGN_FINISH` (35)
- No signature field extraction from signed contact messages (`txt_type=2`)

### 11. Network Layer Commands
- `SEND_RAW_DATA` (25) — send arbitrary payload through mesh
- `SEND_TRACE_PATH` (36) — trace route with SNR per hop
- `SEND_CONTROL_DATA` (55) — node discovery, etc.
- `SET_FLOOD_SCOPE` (54) / `SET_DEFAULT_FLOOD_SCOPE` (63) / `GET_DEFAULT_FLOOD_SCOPE` (64)

### 12. Response Parsing Missing
- `MC_RESP_CONTACT_MSG_RECV_V3` (16) — SNR-included format
- `MC_RESP_CHANNEL_MSG_RECV_V3` (17) — SNR-included format
- `MC_RESP_SIGN_START` / `MC_RESP_SIGNATURE` (19, 20)
- `MC_RESP_CUSTOM_VARS` (21)
- `MC_RESP_ADVERT_PATH` (22)
- `MC_RESP_TUNING_PARAMS` (23)
- `MC_RESP_AUTOADD_CONFIG` (25)
- `MC_RESP_ALLOWED_REPEAT_FREQ` (26)
- `MC_RESP_DEFAULT_FLOOD_SCOPE` (28)
- `MC_PUSH_LOG_RX_DATA` (0x88) — RF packet monitor
- `MC_PUSH_TRACE_DATA` (0x89) — trace response
- `MC_PUSH_NEW_ADVERT` (0x8A) — new contact push
- `MC_PUSH_TELEMETRY` (0x8B) — telemetry push
- `MC_PUSH_BINARY_RESP` (0x8C) — binary response
- `MC_RESP_PATH_DISCOVERY` (0x8D)
- `MC_PUSH_CONTROL_DATA` (0x8E)
- `MC_RESP_CONTACT_DELETED` (0x8F)
- `MC_RESP_CONTACTS_FULL` (0x90)

### 13. Battery Response — STORAGE INFO MISSING
`meshcore_py` parses the full `BATT_AND_STORAGE` response (11 bytes): battery
level + storage `used_kb` + `total_kb`. `meshcore_c` only parses the 2-byte
battery voltage (`battery_mv`).

### 14. CUSTOM\_VARS — COMMAND & RESPONSE MISSING
`GET_CUSTOM_VARS` (40) / `SET_CUSTOM_VAR` (41) — key:value pairs stored on the
device. Both command builders and `MC_RESP_CUSTOM_VARS` parsing are absent.

### 15. GET\_SELF\_TELEMETRY — MISSING
`meshcore_py` can request the local node's own telemetry via `get_self_telemetry()`.
The command builder and `MC_PUSH_TELEMETRY` response parsing are missing.

---

## NICE TO HAVE (convenience, robustness, UX)

### 16. Auto-Reconnect / ConnectionManager
`meshcore_py` has a `ConnectionManager` class with:
- Automatic reconnect with configurable max attempts
- `send_appstart()` callback after every reconnection
- CONNECTED / DISCONNECTED event emission
- Robust disconnect handling for all three transports (BLE/Serial/TCP)

The Arduino wrapper in `meshcore_c` has `begin()`, but no reconnect logic.

### 17. Event Subscription with Attribute Filtering
`meshcore_py`'s `EventDispatcher` supports:
- Subscribe to specific event types (or all via `None`)
- Attribute-based filtering (e.g., "only ACKs with code=X")
- `wait_for_event()` with timeout
- `clone()` for safe async dispatch

The C core has no event system — the caller gets a raw `mc_event_t` struct.
The Arduino wrapper has simple lambda callbacks with no filtering. This is
fine for a C library but limits complex dispatch scenarios.

### 18. Contact State Tracking
`meshcore_py` maintains internal dictionaries for contacts, self_info, and
device time, auto-updating them on relevant events. The C core leaves all
state management to the caller. Useful for real apps but adds memory/CPU cost.

### 19. Automatic Message Fetching
`meshcore_py` has `start_auto_message_fetching()` and
`stop_auto_message_fetching()` that use asyncio to automatically drain the
radio's message queue when `MESSAGES_WAITING` events arrive.

The Arduino wrapper already auto-drains in `dispatch()` (enabled by
`setAutoSync(true)`). The C core's examples also drain manually. **This is the
one "nice to have" that is already well-addressed in `meshcore_c`.**

### 20. Synchronous Request/Response Helpers
`meshcore_py` wraps fire-and-forget commands with `_sync` variants that:
- Lock via `_mesh_request_lock`
- Wait for the specific response event
- Apply suggested timeout from `MSG_SENT`
- Return parsed payload or `None`

Examples: `req_status_sync`, `req_telemetry_sync`, `send_path_discovery_sync`,
`send_login_sync`, `req_neighbours_sync`, etc.

`sent_msg_with_retry` goes further: retry up to N times, auto-switch to flood
after K failed direct attempts.

### 21. Channel Log Decryption
`meshcore_py` can decrypt AES-encrypted channel messages found in the
RF packet log (`meshcore_parser.py`). It tracks channels, computes HMACs and
AES decryption, and matches duplicate packets. This requires cryptographic
libraries (pycryptodome) and is likely too heavy for a bare-metal C core.

### 22. Cayenne LPP Telemetry Parsing
`meshcore_py` parses Cayenne LPP binary telemetry frames into JSON with typed
values (temperature, humidity, GPS, voltage, etc.). Requires an LPP library
and is application-layer — probably belongs in the caller, not the core.

### 23. More Examples
`meshcore_py` ships 33 example scripts covering chat, battery monitor, channel
manager, contacts, pingbot, RSS bot, trace, OLLAMA integration, and many BLE
workflows. `meshcore_c` has 4 good examples but many usage patterns are not
demonstrated.

### 24. Test Coverage
`meshcore_py` has 13+ unit test files. `meshcore_c` has one good unit test
(`test_codec.c`) covering frame assembly, parsing, and resync. More tests
for SELF_INFO, CONTACT parsing, error paths, and edge cases would be valuable.

---

## What meshcore\_c Does BETTER Than meshcore\_py

1. **Zero dependencies** — The C core needs only `stdint.h`, `stddef.h`, and
   `string.h`. No malloc, no crypto, no third-party libraries. Truly portable
   to any C99 environment.

2. **No I/O in the core** — Transport is 100% external. Same code works on
   Linux, ESP-IDF, STM32 HAL, and Arduino without `#ifdef`.

3. **Platform breadth** — Already ships examples for Linux, ESP-IDF, STM32,
   and Arduino. `meshcore_py` is Python-only (Linux/macOS/Windows, not
   embedded).

4. **Build simplicity** — One CMakeLists.txt, or compile with a single `cc`
   command. No package manager needed.

5. **Auto-drain in Arduino wrapper** — The `dispatch()` method automatically
   syncs messages when `MC_PUSH_MSG_WAITING` arrives, simplifying the user
   loop.

6. **Clean C API** — Pure struct-based API with no callbacks, no heap, no
   hidden state. The caller owns all memory.

---

## Prioritized Implementation Roadmap

### Phase 1 — Core Protocol Completion (Major)
1. `MSG_SENT` response parsing (ack tag + timeout tracking)
2. `MC_RESP_STATS` parsing (core/radio/packet subtypes)
3. Fix `SELF_INFO` byte offsets and add missing struct fields
4. Add `CHANNEL_MSG_RECV_V3` and `CONTACT_MSG_RECV_V3` parsing
5. Parse `repeat` and `path_hash_mode` in `DEVICE_INFO`
6. Add `mc_cmd_send_txt_msg` and `mc_cmd_send_cmd` builders

### Phase 2 — Contact & Binary Requests (Major)
7. Contact command builders: GET_CONTACTS, ADD_UPDATE_CONTACT, REMOVE_CONTACT,
   RESET_PATH, SHARE_CONTACT, EXPORT/IMPORT_CONTACT, GET_CONTACT_BY_KEY
8. Contact response parsing: CONTACT_START, CONTACT, CONTACT_END
9. Binary request infrastructure: `mc_cmd_send_binary_req`, tag tracking
10. BINARY_RESPONSE parsing with sub-type dispatch (STATUS, TELEMETRY, etc.)

### Phase 3 — Device Management (Minor)
11. REBOOT, FACTORY_RESET, SET_DEVICE_PIN
12. SET_OTHER_PARAMS, SET_TUNING_PARAMS/GET_TUNING_PARAMS
13. EXPORT_PRIVATE_KEY/IMPORT_PRIVATE_KEY
14. CUSTOM_VARS get/set
15. SET_ADVERT_NAME, SET_ADVERT_LATLON
16. SET_FLOOD_SCOPE, SET_DEFAULT_FLOOD_SCOPE
17. HasConnection, GetBattAndStorage (storage fields)
18. SEND_TRACE_PATH, SEND_RAW_DATA, SEND_CONTROL_DATA

### Phase 4 — Polish (Nice to Have)
19. ConnectionManager-style reconnect in the Arduino wrapper
20. More unit tests
21. Additional examples (chat, contacts sync, telemetry display)
22. Message retry with path-reset-to-flood fallback

---

*Comparison performed against [meshcore_py](https://github.com/meshcore-dev/meshcore_py) at commit HEAD, 2026-06-08.*
