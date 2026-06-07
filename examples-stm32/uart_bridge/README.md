# STM32 example: uart_bridge

Integration example showing how to drive a MeshCore Companion Radio from an STM32
using the CubeMX-generated HAL. The protocol logic is the repo's portable C core
(`src/meshcore_companion.c`); this file supplies only the UART transport and two
entry points you call from your generated `main()`.

Because an STM32 firmware build is tied to your specific MCU, clocks, pins and
linker script (all produced by STM32CubeIDE / CubeMX), this is **not** a
standalone buildable project — it is a drop-in.

## Steps (STM32CubeIDE / CubeMX)

1. Generate a project with one USART enabled at **115200 8N1** (e.g. `USART1`).
   Wire it to the companion radio: host TX → radio RX, host RX ← radio TX, GND↔GND.
   The radio must run the serial companion firmware (`companion_radio_usb`).
2. Add `src/meshcore_companion.c` and `src/meshcore_companion.h` from this repo to
   your project (or add this repo's `src/` to the include paths).
3. Add `meshcore_stm32.c` to your project. If your USART handle isn't `huart1`,
   change the `extern UART_HandleTypeDef huart1;` line near the top.
4. In the generated `main.c`:
   ```c
   /* USER CODE BEGIN 2 */
   meshcore_setup();
   /* USER CODE END 2 */

   while (1) {
       /* USER CODE BEGIN 3 */
       meshcore_poll();
   }
   ```

## Notes

- `meshcore_poll()` polls the UART one byte at a time, which is fine for the
  companion's low data rate. For high throughput, switch the RX side to
  interrupt/DMA into a ring buffer and feed that to `mc_rx_feed()` — the core
  code is unchanged.
- Logging uses `printf()`. Retarget it to a **separate** debug UART or SWO/ITM
  (commonly USART2 = the ST-Link VCP) by implementing `_write()`; do not point it
  at the companion UART.
- The same two-function transport pattern (`send bytes` / `read available bytes`)
  ports directly to bare-metal STM32, nRF52 (nRF5 SDK or Zephyr), or any other
  MCU — only the HAL calls change.
