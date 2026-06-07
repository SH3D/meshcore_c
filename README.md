# meshcore_c

A client for the **MeshCore Companion Radio** serial protocol, for when the radio
lives on one device and your application (display, sensor hub, gateway, desktop
tool) lives on another. Talk to a separate companion radio — e.g. a Seeed XIAO +
Wio-SX1262 — over a UART / USB-serial link and exchange channel messages, set
channel PSKs, and read link metadata. Your host plays the *client*, the companion
radio plays the *server* — the inverse of the phone/web app.

This is the **C / C++** one. Sibling implementations in other languages:

- **JavaScript** — [`meshcore.js`](https://github.com/meshcore-dev/meshcore.js)
- **Python** — [`meshcore_py`](https://github.com/meshcore-dev/meshcore_py)
- **Rust** — [`meshcore-rs`](https://github.com/andrewdavidmackenzie/meshcore-rs)

## Two ways to use this repo

The protocol logic is a single portable **C99 core** with no I/O, no `malloc`, and
no Arduino dependency. Everything else is a thin layer over it, so one repo serves
two audiences from **one source of truth** (`src/meshcore_companion.{c,h}`):

1. **Portable C library** — drop `src/meshcore_companion.{c,h}` into any project.
   It assembles inbound frames from a byte stream, builds outbound command frames
   into buffers you own, and parses payloads into plain structs. You supply the
   transport. The same core drops into Linux, ESP-IDF, bare STM32/nRF52, or any
   host bridge unchanged — see the example for each platform under `examples-*/`,
   plus the host unit test in `test/` that runs with no hardware.

2. **C++ Arduino library** — `src/MeshCoreCompanion.{h,cpp}` wrap the core in an
   Arduino-friendly class: inject any `Stream` (`Serial1` on a Grove UART, USB
   CDC, `SoftwareSerial`), call `loop()` often, register lambda callbacks. It
   **auto-drains** the radio's queue: on the `MsgWaiting` push it loops
   `SyncNextMessage` until empty, so you just receive `onChannelMessage(...)`.
   The repo root is a publishable Arduino / PlatformIO library
   (`library.properties` + `library.json`).

## Layout

```
meshcore_c/
├── library.properties   # Arduino IDE / Library Manager metadata
├── library.json         # PlatformIO metadata
├── CMakeLists.txt       # Linux / native (host) build of the C core + examples + test
├── src/
│   ├── meshcore_companion.h/.c   # portable C99 core — single source of truth
│   └── MeshCoreCompanion.h/.cpp  # C++ Arduino wrapper (built by Arduino/PlatformIO)
├── examples/
│   └── SensorChannelBridge/      # Arduino sketch (.ino)
├── examples-linux/
│   └── tty_bridge/               # portable C: any Linux tty (USB or raw UART)
├── examples-esp-idf/
│   └── tty_bridge/               # portable C: ESP-IDF UART driver (esp32/s3/c3)
├── examples-stm32/
│   └── uart_bridge/              # portable C: STM32 HAL UART drop-in
└── test/
    └── test_codec.c              # host unit test (no hardware)
```

The portable-C examples all compile the *same* `src/meshcore_companion.c`; only
the byte transport differs per platform.

The Arduino/PlatformIO build only compiles `src/` (the manifests' `srcFilter` is
scoped there), so the Linux example and host test never enter a firmware build.
The CMake build only compiles the pure-C parts and ignores the Arduino wrapper
(which needs `<Arduino.h>`) and the manifests.

## Wire protocol

`frame = [type:u8][len:u16 LE][payload:len bytes]`, type `0x3C` for app→radio,
`0x3E` for radio→app. `payload[0]` is the command/response/push code. Derived from
the MeshCore companion protocol and the `meshcore.js` reference client.

The companion radio must be flashed with the **serial** companion variant
(`companion_radio_usb`) and have its serial interface bound to the port you wire
to — not BLE, and not the WiFi/TCP variant. Companions compile in one interface at
a time.

## Build & run the C library (Linux / host)

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure     # run the codec unit test

./build/meshcore_tty /dev/ttyACM0              # USB-CDC companion
./build/meshcore_tty /dev/ttyUSB0 2 000102030405060708090a0b0c0d0e0f sensors
#                    ^tty         ^ch ^16-byte PSK (32 hex chars)        ^name
```

Or compile the example directly, no CMake:

```sh
cc -std=c99 -Wall -Wextra -Isrc \
   examples-linux/tty_bridge/meshcore_tty.c src/meshcore_companion.c -o meshcore_tty
```

## Other platform examples (same C core)

Each example compiles `src/meshcore_companion.c` directly and supplies only a
platform-specific UART transport — no copy of the core is kept anywhere.

- **ESP-IDF** — `examples-esp-idf/tty_bridge/`. A full IDF project using the UART
  driver:
  ```sh
  cd examples-esp-idf/tty_bridge
  idf.py set-target esp32s3 && idf.py build flash monitor
  ```
- **STM32** — `examples-stm32/uart_bridge/`. A HAL drop-in (STM32 builds are
  board/toolchain specific): add `meshcore_stm32.c` + `src/` to a CubeMX/CubeIDE
  project and call `meshcore_setup()` / `meshcore_poll()` from `main()`. See its
  README.

The transport is just two operations — *write bytes* and *read available bytes* —
so porting to bare-metal STM32, nRF52 (nRF5 SDK or Zephyr), or any other MCU only
changes those calls; the protocol code is identical.

## Use the Arduino library

Install via PlatformIO (`lib_deps = symlink:///path/to/meshcore_c`), the Arduino
Library Manager (once published), or by copying the repo into your `libraries/`
folder. Then:

```cpp
#include "MeshCoreCompanion.h"
MeshCoreCompanion mc(Serial1);

void setup() {
    Serial1.begin(115200, SERIAL_8N1, /*RX*/16, /*TX*/17);

    mc.onDeviceInfo([](const mc_device_info_t& d){
        mc.setChannelHexSecret(2, "sensors", "000102...0f");  // 32 hex chars
        mc.getDeviceTime();
    });
    mc.onChannelMessage([](const mc_channel_msg_t& m){
        // m.text is "SenderName: body"
    });
    mc.onChannelData([](const mc_channel_data_t& d){
        // d.data / d.data_len, plus MC_SNR_DB(d.snr_q4), d.path_len
    });

    mc.begin();                 // AppStart + DeviceQuery
}

void loop() {
    mc.loop();                  // pump + auto-drain
    // mc.sendChannelText(2, "hello");
}
```

See `examples/SensorChannelBridge/` for a complete sketch.

## What's covered

| Need | C core API | Arduino wrapper |
|------|------------|-----------------|
| Handshake / identity | `mc_cmd_app_start`, `mc_cmd_device_query` | `begin()`, `appStart()`, `deviceQuery()` → `onDeviceInfo`/`onSelfInfo` |
| Receive channel text | `mc_parse` → `MC_RESP_CHANNEL_MSG_RECV` | `onChannelMessage` (auto-drained) |
| Receive channel data + metadata | `mc_parse` → `MC_RESP_CHANNEL_DATA_RECV` | `onChannelData` (SNR, path, type) |
| Send on a channel | `mc_cmd_send_channel_text` | `sendChannelText(idx, text)` |
| Set / read channel PSK | `mc_cmd_set_channel`, `mc_cmd_get_channel` | `setChannel`, `setChannelHexSecret`, `getChannel` → `onChannelInfo` |
| Device time | `mc_cmd_get/set_device_time` | `getDeviceTime`, `setDeviceTime`, `deviceEpochNow()` |
| Adverts | `mc_cmd_send_self_advert` | `sendSelfAdvert(flood)` |
| Radio params / stats | `mc_cmd_set_radio_params`, `mc_cmd_get_stats` | `getStats` |

Adding a command the wrapper doesn't surface yet is one builder plus one `case`.

## Notes

- **Timestamps:** channel messages carry an epoch-seconds sender timestamp. Call
  `getDeviceTime()` once so outgoing text can be stamped; otherwise it sends `0`.
- **PSK derivation:** `set_channel` takes a raw 16-byte secret. Derive one from a
  passphrase host-side (e.g. SHA-256 prefix, as `meshcore.js`'s transport-key util
  shows) — kept out of the core to stay dependency-free.
- **Buffer sizes:** override `MC_MAX_PAYLOAD`, `MC_MAX_TEXT`, `MC_MAX_DATA` before
  including the header if your traffic needs more.

## License

MIT. Protocol derived from MeshCore and `meshcore.js` (MIT, © Liam Cottle).
