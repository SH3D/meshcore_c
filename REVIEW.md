# meshcore_c vs meshcore_py — Feature Gap Analysis

Date: 2026-06-08 (Phase 1 complete)

## Summary

`meshcore_c` is a clean, portable C99 core + Arduino C++ wrapper for the MeshCore
Companion Radio **serial** protocol. It implements the wire framing (0x3C/0x3E,
LE length), handshake, channel text/data send/receive (incl. the V3 SNR
variants), direct/contact + CLI message sending, channel PSK programming, full
device/self/stats parsing, and basic radio parameters. Zero external deps in the
core; ships examples for Linux (`tty_bridge`, `info`), ESP-IDF, STM32 HAL, and
Arduino.

Against the reference `meshcore_py`, coverage is now roughly **45%**. The two
remaining **major** areas are contact management and binary (telemetry/status)
requests. Everything else is feature-completeness or convenience.

---

## Already implemented (baseline)

Framing/RX assembler + resync · `app_start` · `device_query` → **DeviceInfo**
(incl. `ver`, `repeat` fw≥9, `path_hash_mode` fw≥10) · **SelfInfo** (incl.
`multi_acks`, `adv_loc_policy`, `telemetry_mode` base/loc/env) · get/set device
time · **CurrTime** · battery voltage (2-byte only) · channel get/set + PSK →
**ChannelInfo** · send channel text · **ChannelMsgRecv / ChannelDataRecv** ·
**ContactMsgRecv** (+ signature extraction for `txt_type=2`) · **V3** channel +
contact recv (SNR) · `send_txt_msg` / `send_cmd` (direct + CLI, CMD 2) ·
**MsgSent** parse (ack tag + suggested timeout) · `set_radio_params` ·
`get_stats` → **Stats** (core/radio/packets) · self-advert · auto-drain on
`MsgWaiting` (Arduino wrapper).

Host unit tests (`test/test_codec.c`) cover all of the above parsers/builders.

---

## Remaining gaps

### MAJOR

#### 1. Contact management — entirely missing
No way to enumerate or manage the mesh node list from C.

| Feature | CMD | meshcore_py | meshcore_c |
|---|---|---|---|
| GET_CONTACTS (lastmod delta sync) | 4 | ✅ | ❌ |
| ADD_UPDATE_CONTACT | 9 | ✅ | ❌ |
| REMOVE_CONTACT | 15 | ✅ | ❌ |
| SHARE_CONTACT | 16 | ✅ | ❌ |
| EXPORT_CONTACT / IMPORT_CONTACT | 17 / 18 | ✅ | ❌ |
| RESET_PATH | 13 | ✅ | ❌ |
| GET_CONTACT_BY_KEY | 30 | ✅ | ❌ |
| SET_ADVERT_NAME / SET_ADVERT_LATLON | 8 / 14 | ✅ | ❌ |
| GET_ADVERT_PATH | 42 | ✅ | ❌ |
| SET/GET_AUTOADD_CONFIG | 58 / 59 | ✅ | ❌ |
| GET_ALLOWED_REPEAT_FREQ | 60 | ✅ | ❌ |

Response parsing needed: `CONTACT_START` (2), `CONTACT` (3), `CONTACT_END` (4),
`CONTACT_URI` (11), `ADVERT_PATH` (22), `AUTOADD_CONFIG` (25),
`ALLOWED_REPEAT_FREQ` (26), `CONTACT_DELETED` (0x8F), `CONTACTS_FULL` (0x90),
`NEW_ADVERT` push (0x8A).

#### 2. Binary + anonymous requests — entirely missing
Remote interrogation of other nodes.
- `SEND_BINARY_REQ` (50) subtypes: STATUS, TELEMETRY, MMA, ACL, NEIGHBOURS
- `SEND_ANON_REQ` (57) subtypes: REGIONS, OWNER, BASIC (remote clock)
- `BINARY_RESPONSE` push (0x8C) parsing + tag tracking (`expected_ack` →
  response correlation, reusing the `MsgSent` ack-tag + timeout already parsed)
- `TELEMETRY` push (0x8B) parsing (Cayenne LPP — likely decoded by the caller)

### MINOR

#### 3. Direct-message reliability
- `SEND_LOGIN` (26) / `SEND_LOGOUT` (29) / `SEND_STATUS_REQ` (27)
- Retry helper: resend up to N times, auto-switch to flood after K direct
  failures (uses the already-parsed `MsgSent` suggested-timeout).

#### 4. Device-management commands
`REBOOT` (19) · `FACTORY_RESET` (51, two-step) · `SET_DEVICE_PIN` (37) ·
`SET_OTHER_PARAMS` (38: telemetry modes, multi_acks, advert policy) ·
`SET_TUNING_PARAMS` (21) / `GET_TUNING_PARAMS` (43) · `HAS_CONNECTION` (28) ·
`SET_TX_POWER` (12).

#### 5. Crypto & security
`EXPORT_PRIVATE_KEY` (23) / `IMPORT_PRIVATE_KEY` (24) ·
`SIGN_START` (33) / `SIGN_DATA` (34) / `SIGN_FINISH` (35).
(Signature *extraction* from received signed messages is already done.)

#### 6. Network-layer commands
`SEND_RAW_DATA` (25) · `SEND_TRACE_PATH` (36, per-hop SNR) ·
`SEND_CONTROL_DATA` (55) · `SET_FLOOD_SCOPE` (54) /
`SET/GET_DEFAULT_FLOOD_SCOPE` (63 / 64).

#### 7. Remaining response/push parsing
`SIGN_START`/`SIGNATURE` (19/20) · `CUSTOM_VARS` (21) · `TUNING_PARAMS` (23) ·
`DEFAULT_FLOOD_SCOPE` (28) · `LOG_RX_DATA` push (0x88) · `TRACE_DATA` push
(0x89) · `PATH_DISCOVERY` (0x8D) · `CONTROL_DATA` push (0x8E).

#### 8. Smaller data gaps
- **Battery**: only the 2-byte voltage is parsed; the full `BATT_AND_STORAGE`
  response adds `used_kb` + `total_kb`.
- **CUSTOM_VARS**: `GET_CUSTOM_VARS` (40) / `SET_CUSTOM_VAR` (41) + parse.
- **GET_SELF_TELEMETRY**: local-node telemetry request + `TELEMETRY` parse.

### NICE TO HAVE
- Auto-reconnect / connection-manager logic in the Arduino wrapper.
- Event filtering / `wait_for_event(timeout)` helpers (C core stays struct-based).
- Optional contact / self-info / time state tracking in the wrapper.
- Synchronous request→response helpers (lock, await matching event, apply
  suggested timeout) — pairs with #2 binary requests and #3 retry.
- Channel-log AES decryption (heavy; needs crypto — probably out of scope for
  the bare-metal core).
- Cayenne LPP telemetry decode (application layer / caller).
- More examples (chat, contacts sync, telemetry display) and more tests
  (contact parsing, error paths, edge cases).

---

## What meshcore_c does better than meshcore_py
- **Zero dependencies / no malloc / no I/O in the core** — drops into any C99
  target; transport is 100% external (Linux, ESP-IDF, STM32, Arduino, no `#ifdef`).
- **Build simplicity** — one `CMakeLists.txt`, a single `cc`, or `pio test`.
- **Struct-based, caller-owns-memory API**, with an Arduino wrapper that
  auto-drains the radio queue on `MsgWaiting`.

---

## Roadmap

### Phase 2 — Contacts & binary requests (next, major)
1. Contact command builders: GET_CONTACTS, ADD_UPDATE_CONTACT, REMOVE_CONTACT,
   RESET_PATH, SHARE_CONTACT, EXPORT/IMPORT_CONTACT, GET_CONTACT_BY_KEY.
2. Contact response parsing: CONTACT_START / CONTACT / CONTACT_END (+ CONTACT_URI,
   CONTACT_DELETED, CONTACTS_FULL, NEW_ADVERT).
3. Binary-request infrastructure: `mc_cmd_send_binary_req` / `mc_cmd_send_anon_req`,
   `BINARY_RESPONSE` parsing, ack-tag correlation (reuse `MsgSent` tag+timeout).
4. Tests + a `contacts` Linux example.

### Phase 3 — Device management & network commands (minor)
REBOOT / FACTORY_RESET / SET_DEVICE_PIN / SET_OTHER_PARAMS · TUNING_PARAMS
get/set · EXPORT/IMPORT_PRIVATE_KEY · CUSTOM_VARS · SET_ADVERT_NAME/LATLON ·
flood scope get/set · HAS_CONNECTION · BATT_AND_STORAGE storage fields ·
SEND_TRACE_PATH / SEND_RAW_DATA / SEND_CONTROL_DATA.

### Phase 4 — Polish (nice to have)
Connection-manager reconnect · sync request/response + retry-with-flood-fallback
helpers · event filtering · more examples and tests.

---

*Compared against [meshcore_py](https://github.com/meshcore-dev/meshcore_py),
HEAD as of 2026-06-08. Protocol codes cross-checked against meshcore.js
`constants.js` and meshcore_py `packets.py`.*
