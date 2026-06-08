# MeshCoreCompanion

A client library for the **MeshCore Companion Radio** serial protocol, for when
the radio lives on one MCU and your application (display, sensor hub, gateway)
lives on another. Talk to a separate companion radio — e.g. a Seeed XIAO +
Wio-SX1262 — over a UART/USB serial link and exchange channel messages, set
channel PSKs, and read link metadata.

This is the inverse of the phone/web app: your host MCU plays the *client*, the
companion radio plays the *server*.

## Design: portable C core + Arduino wrapper

Two layers, so the protocol is reusable and testable far beyond Arduino:

- **`meshcore_companion.h/.c`** — a portable **C99** core. No I/O, no `malloc`,
  no Arduino. It assembles inbound frames from a byte stream, builds outbound
  command frames into buffers you own, and parses payloads into plain structs.
  It compiles and runs on a desktop for unit testing (see `test/`), and drops
  into ESP-IDF, bare nRF52/STM32, or a host bridge unchanged.
- **`MeshCoreCompanion.h/.cpp`** — a thin C++ wrapper. Inject any `Stream`
  (`Serial1` on a Grove UART, USB CDC, `SoftwareSerial`), call `loop()` often,
  register lambda callbacks. It also **auto-drains** the radio's queue: on the
  `MsgWaiting` push it loops `SyncNextMessage` until empty, so you just receive
  `onChannelMessage(...)`.

Transport is injected; the core never touches a port. "Physical serial only" is
simply which `Stream` you hand the wrapper.

## Wire protocol

`frame = [type:u8][len:u16 LE][payload:len bytes]`, type `0x3C` for app→radio,
`0x3E` for radio→app. `payload[0]` is the command/response/push code. Derived
from the MeshCore companion protocol and the `meshcore.js` reference client.

## Install (PlatformIO)

Drop this folder into your project's `lib/`, or add to `platformio.ini`:

```ini
lib_deps = symlink:///path/to/MeshCoreCompanion
```

The companion radio must be flashed with the **serial** companion variant
(`companion_radio_usb`) and have its serial interface bound to the UART you wire
to — not BLE, and not the WiFi/TCP variant. Companions compile in one interface
at a time. If you are using an OS (e.g. Linux) you can use USB variant as the drivers will present a /dev/ttyX interface that looks same as serial. If wanting to use bare metal evices such as AVR/ESP32/nRF52/STM serial, then see notes below about changes to radio.

## Quick start

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

## Linux / host build (CMake)

The portable C core needs no Arduino and no hardware — it builds and runs on any
desktop. A `CMakeLists.txt` at the repo root builds the core, the host unit test,
and the Linux examples (the Arduino C++ wrapper is skipped here — it needs
`<Arduino.h>`):

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure      # run the codec unit test
```

### Linux examples (`examples-linux/`)

Both talk to a companion radio over **any tty** — a USB-CDC device
(`/dev/ttyACM0`), a USB-serial adapter (`/dev/ttyUSB0`), or a raw UART
(`/dev/serial0`):

- **`tty_bridge`** — live: receive/echo channel traffic and send messages.
  ```sh
  ./build/meshcore_tty /dev/ttyACM0
  ```
- **`info`** — one-shot: dump everything the radio reports (model, firmware,
  radio params, device time, channels + PSKs, stats), then exit.
  ```sh
  ./build/meshcore_info /dev/ttyACM0
  ```

No CMake? Compile a single example directly:

```sh
cc -std=c99 -Wall -Wextra -Isrc \
   examples-linux/info/meshcore_info.c src/meshcore_companion.c -o meshcore_info
```

The same C core also ships starter examples for other toolchains in
`examples-esp-idf/` (ESP-IDF UART driver) and `examples-stm32/` (STM32 HAL).

## What's covered

| Need | API |
|------|-----|
| Handshake / identity | `begin()`, `appStart()`, `deviceQuery()` → `onDeviceInfo`, `onSelfInfo` |
| Receive channel text | `onChannelMessage` (auto-drained) |
| Receive channel data + metadata | `onChannelData` (SNR, path length, data type) |
| Send on a channel | `sendChannelText(idx, text)` |
| Set / read channel PSK | `setChannel`, `setChannelHexSecret`, `getChannel` → `onChannelInfo` |
| Device time | `getDeviceTime`, `setDeviceTime`, `deviceEpochNow()` |
| Adverts | `sendSelfAdvert(flood)` |
| Radio params / stats | `getStats`, plus `mc_cmd_set_radio_params` in the core |

The C core also exposes raw command builders and `mc_parse` for codes the
wrapper doesn't surface yet (contacts, battery, send-confirmed, etc.) — adding a
new command is one builder plus one `case`.

## Testing the core (no hardware)

Run everything at once with `./testall.sh` — it runs the CMake/ctest, PlatformIO
native, and Arduino-compile suites, and auto-skips whichever tools aren't
installed (so it works on a cmake-only or PlatformIO-only box):

```sh
./testall.sh
```

Or run a suite individually:

```sh
ctest --test-dir build --output-on-failure       # via CMake (see above)
pio test -e native                               # via PlatformIO's native env
cd test && cc -std=c99 -Wall -Wextra -I../src test_codec.c ../src/meshcore_companion.c -o t && ./t
```

The unit test exercises command encoding, frame reassembly across split reads,
every parser, and resync past line garbage.

## Notes

- **Timestamps:** channel messages carry an epoch-seconds sender timestamp.
  Call `getDeviceTime()` (or `setDeviceTime()`) once so `sendChannelText` can
  stamp outgoing messages; otherwise it sends `0`.
- **PSK derivation:** `setChannel` takes a raw 16-byte secret. If you need to
  derive one from a passphrase, do it host-side (e.g. SHA-256 prefix, as
  `meshcore.js`'s transport-key util shows) — kept out of the core to stay
  dependency-free.
- **Buffer sizes:** override `MC_MAX_PAYLOAD`, `MC_MAX_TEXT`, `MC_MAX_DATA`
  before including the header if your traffic needs more.

## License

MIT. Protocol derived from MeshCore and `meshcore.js` (MIT, © Liam Cottle).
