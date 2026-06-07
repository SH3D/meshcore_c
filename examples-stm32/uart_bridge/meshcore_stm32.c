/*
 * meshcore_stm32.c -- STM32 HAL integration example.
 *
 * Unlike the Linux and ESP-IDF examples, STM32 builds are board- and
 * toolchain-specific (the CubeMX-generated HAL, startup code and linker script
 * belong to your project), so this is an *integration example* rather than a
 * standalone build. It shows the only two things the portable core needs from a
 * platform: write some bytes, and read whatever bytes have arrived.
 *
 * How to use (STM32CubeIDE / CubeMX):
 *   1. Generate a project with one USART enabled at 115200 8N1 (e.g. USART1).
 *      Wire it to the companion radio (host TX -> radio RX, host RX <- radio TX).
 *   2. Add src/meshcore_companion.c and src/meshcore_companion.h to the project
 *      (Core/Src and Core/Inc, or add this repo's src/ to the include paths).
 *   3. Add this file to the project.
 *   4. In the generated main(): call meshcore_setup() once after MX_USARTx_UART_Init(),
 *      then call meshcore_poll() every iteration of the main while(1) loop.
 *
 * Byte-at-a-time polling is fine for the companion's low data rate; for high
 * throughput switch the transport to interrupt/DMA RX into a ring buffer and
 * feed that buffer to mc_rx_feed() — the core code does not change.
 *
 * Logging here uses printf(); retarget it to a *separate* debug UART or SWO/ITM
 * (do not point it at the companion UART). On many CubeIDE projects that means
 * implementing _write() to HAL_UART_Transmit on USART2 (the ST-Link VCP).
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#include "main.h"          /* CubeMX-generated: pulls in stm32xxxx_hal.h + handles */

#include <stdio.h>
#include <stdlib.h>

#include "meshcore_companion.h"

/* The USART you enabled in CubeMX and wired to the companion radio. */
extern UART_HandleTypeDef huart1;
#define MC_UART (&huart1)

static mc_rx_t s_rx;

static void send_payload(const uint8_t *payload, size_t len)
{
    uint8_t frame[MC_RX_BUFSZ];
    size_t flen = mc_frame_encode(payload, len, frame, sizeof frame);
    if (flen) HAL_UART_Transmit(MC_UART, frame, (uint16_t)flen, HAL_MAX_DELAY);
}

static void on_event(const mc_event_t *ev)
{
    switch (ev->code) {
    case MC_RESP_DEVICE_INFO:
        printf("radio model=%s fw=%d channels=%u build=%s\r\n",
               ev->u.device_info.model, ev->u.device_info.fw_ver,
               (unsigned)ev->u.device_info.max_channels,
               ev->u.device_info.build_date);
        break;
    case MC_RESP_CHANNEL_MSG_RECV:   /* body is "SenderName: message" */
        printf("[ch %d] %s\r\n", ev->u.channel_msg.channel_idx,
               ev->u.channel_msg.text);
        break;
    case MC_RESP_CHANNEL_DATA_RECV: {
        int centi = ev->u.channel_data.snr_q4 * 25;   /* q4 -> /4 then *100 */
        const char *sign = centi < 0 ? "-" : "";
        printf("[ch %d] %u bytes type=0x%04X snr=%s%d.%02d dB %s\r\n",
               ev->u.channel_data.channel_idx,
               (unsigned)ev->u.channel_data.data_len,
               (unsigned)ev->u.channel_data.data_type,
               sign, abs(centi) / 100, abs(centi) % 100,
               ev->u.channel_data.path_len == MC_PATH_DIRECT ? "direct" : "flood");
        break;
    }
    case MC_RESP_CURR_TIME:
        printf("device time = %u (epoch secs)\r\n", (unsigned)ev->u.curr_time);
        break;
    case MC_RESP_ERR:
        printf("radio error response (code=%d)\r\n", ev->u.err_code);
        break;
    default:
        break;
    }
}

/* Call once after MX_USARTx_UART_Init(). */
void meshcore_setup(void)
{
    mc_rx_init(&s_rx);

    uint8_t cmd[MC_MAX_PAYLOAD];
    size_t  n;
    n = mc_cmd_app_start(cmd, sizeof cmd, "stm32");   if (n) send_payload(cmd, n);
    n = mc_cmd_device_query(cmd, sizeof cmd, 1);       if (n) send_payload(cmd, n);
    n = mc_cmd_get_device_time(cmd, sizeof cmd);       if (n) send_payload(cmd, n);
}

/* Call from your main while(1) loop. Non-blocking. */
void meshcore_poll(void)
{
    uint8_t cmd[MC_MAX_PAYLOAD];
    size_t  n;
    uint8_t b;

    /* Drain every byte currently available (timeout 0 = return immediately). */
    while (HAL_UART_Receive(MC_UART, &b, 1, 0) == HAL_OK) {
        mc_rx_feed(&s_rx, &b, 1);

        uint8_t payload[MC_MAX_PAYLOAD];
        size_t  plen;
        while (mc_rx_poll(&s_rx, payload, sizeof payload, &plen)) {
            mc_event_t ev;
            if (!mc_parse(payload, plen, &ev)) continue;
            on_event(&ev);
            if (ev.code == MC_PUSH_MSG_WAITING ||
                ev.code == MC_RESP_CHANNEL_MSG_RECV ||
                ev.code == MC_RESP_CHANNEL_DATA_RECV ||
                ev.code == MC_RESP_CONTACT_MSG_RECV) {
                n = mc_cmd_sync_next_message(cmd, sizeof cmd);
                if (n) send_payload(cmd, n);
            }
        }
    }
}
