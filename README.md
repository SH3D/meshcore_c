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

## Why talk to a companion radio?

The alternative is to compile the full MeshCore stack into your own MCU and drive
an SX-series LoRa modem (e.g. an SX1262) directly over SPI. That works, but it is
heavy on flash and RAM, and it needs several free GPIOs for the SPI bus plus the
radio's control/IRQ lines — pins many boards (or your own design) simply can't
spare once a display, sensors and other peripherals are wired up.

Running the radio as a *separate companion module* keeps all of that — the LoRa
modem, mesh routing and crypto — on the radio, and lets your application talk to
it over a single UART/USB serial link. Your host stays light, and because the
core is plain C99 with no dependencies it runs equally on a small microcontroller
or on a low-level embedded Linux SBC/SoC where C is the natural language.

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

## Connecting to the radio: USB vs TTL serial

On a desktop or SBC (Linux, Windows, macOS) the companion's USB driver exposes a
serial port you open straight from C — `/dev/ttyACM0` and friends. From a
bare-metal / FreeRTOS host there's no USB stack, so you wire the companion's
**TTL UART** to a hardware serial port on your host. That's straightforward on
most MeshCore companion hardware, but it can't be done from the online flasher —
you have to build the firmware yourself.

### Build a serial companion (XIAO ESP32S3 + SX1262)

The companion firmware is built with [PlatformIO](https://platformio.org/), so
you need its source. Clone the MeshCore firmware repo (VS Code + PlatformIO, or
the `pio` CLI):

```sh
git clone https://github.com/ripplebiz/MeshCore
cd MeshCore
```

A companion compiles in **one** host interface at a time, and the XIAO +
Wio-SX1262 variant ships a ready-made environment for each — pick the **serial**
one (no source edit needed):

| PlatformIO environment | Host link |
|---|---|
| `Xiao_S3_WIO_companion_radio_usb`  | native USB-CDC |
| `Xiao_S3_WIO_companion_radio_ble`  | BLE |
| `Xiao_S3_WIO_companion_radio_wifi` | TCP / WiFi |
| **`Xiao_S3_WIO_companion_radio_serial`** | **hardware UART ← use this** |

The `…_serial` env already routes the command interface to `Serial1`: it sets
`-D SERIAL_TX=D6 -D SERIAL_RX=D7` in `variants/xiao_s3_wio/platformio.ini`. Build
and flash it over USB:

```sh
pio run -e Xiao_S3_WIO_companion_radio_serial -t upload
```

(If the upload can't find the board, put the XIAO in bootloader mode: hold **B**,
tap **R**, release **B**.)

Then wire the radio to your host — companion **D6 (TX, GPIO43) → host RX**,
companion **D7 (RX, GPIO44) ← host TX**, **GND↔GND**. Those host pins are exactly
what `UART_RX_PIN` / `UART_TX_PIN` select in the AutoProvision example. The
Wio-SX1262 hat uses the SPI bus (D8–D10) plus GPIO38–42 for the radio, so D6/D7
(and D0–D5) stay free; to use a different pair just override `SERIAL_TX` /
`SERIAL_RX` in that env's `build_flags`.

### XIAO nRF52840 + SX1262

The `xiao_nrf52` variant is also a XIAO + SX1262, but it only ships
`companion_radio_usb` and `companion_radio_ble` — so how you connect decides how
much work it is:

- **Host over USB → already works.** Flash `Xiao_nrf52_companion_radio_usb`:
  ```sh
  pio run -e Xiao_nrf52_companion_radio_usb -t upload
  ```
  The nRF52840's native USB-CDC enumerates as `/dev/ttyACM*`, which is a serial
  port like any other — the Linux examples and AutoProvision work unchanged. No
  firmware edit needed.

- **Host needs raw TTL UART → small firmware edit.** Unlike the ESP32-S3/RP2040,
  the nRF52 path in `examples/companion_radio/main.cpp` only does
  `serial_interface.begin(Serial)` (USB-CDC); the `SERIAL_RX`/`SERIAL_TX`
  HardwareSerial route is guarded to ESP32/RP2040 only, so adding those flags on
  nRF52 does nothing. To expose a TTL UART, bind `Serial1` in that `#else` branch:
  ```cpp
  Serial1.setPins(/*rx*/D7, /*tx*/D6);
  Serial1.begin(115200);
  serial_interface.begin(Serial1);   // instead of begin(Serial)
  ```
  (On the XIAO nRF52840 `Serial1` defaults to D6/D7 under the Adafruit core, but
  `setPins` makes it explicit.) It's a few lines, mirroring the ESP32 path.

Either way the host-side code here is identical — a USB-CDC companion and a
TTL-UART companion look the same to this library.

### Zero-config provisioning from the host

`examples/AutoProvision/` is a plug-and-go host (basic ESP32-S3 dev board): on
boot it programs the companion the way it wants — node name, the **AU narrow
band** region, and a channel — then loops watching that channel and replies to
any `hello` with a response. Wire it to the companion's UART, flash, and the
radio comes up configured with no manual steps:

```sh
cd examples/AutoProvision
pio run -e esp32-s3-devkitc-1 -t upload    # or -e xiao-esp32s3 / -e xiao-esp32c3
pio device monitor
```

It's a self-contained PlatformIO project (`platformio.ini` + `src/main.cpp`) with
an env per host board — a generic ESP32-S3 dev board and the Seeed **XIAO
ESP32-S3** / **XIAO ESP32-C3**. Their host UART pins differ (the XIAOs don't break
out GPIO17/18), and the XIAOs have only native USB, so their envs route `Serial`
to USB-CDC (`ARDUINO_USB_CDC_ON_BOOT=1`) — without that you get no serial monitor. The region is set the same way you'd compile MeshCore firmware —
`LORA_FREQ` / `LORA_BW` / `LORA_SF` / `LORA_CR` / `LORA_TX_POWER` in
`platformio.ini`'s `build_flags` (916.575 MHz / 62.5 kHz / SF7 / CR4/8 / 20 dBm).
The code is plain Arduino C++, so you can rename `src/main.cpp` to
`AutoProvision.ino` for the Arduino IDE instead.

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
