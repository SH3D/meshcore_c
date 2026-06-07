# ESP-IDF example: tty_bridge

Drives a MeshCore Companion Radio over a hardware UART using the ESP-IDF UART
driver. The protocol logic is the repo's portable C core
(`src/meshcore_companion.c`) — this example only supplies the transport and an
`app_main()` loop.

## Wiring

| Companion radio | ESP32 (default pins) |
|-----------------|----------------------|
| TX → host RX    | GPIO16 (`MC_UART_RX_PIN`) |
| RX ← host TX    | GPIO17 (`MC_UART_TX_PIN`) |
| GND             | GND |

Change the pins/UART at the top of `main/main.c`. The radio must run the serial
companion firmware (`companion_radio_usb`) bound to this UART.

## Build

Requires an installed [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)
(v5.x). From this directory:

```sh
idf.py set-target esp32s3      # or esp32, esp32c3, ...
idf.py build flash monitor
```

`main/CMakeLists.txt` compiles `../../../src/meshcore_companion.c` directly, so
there is no copy of the core to keep in sync.
