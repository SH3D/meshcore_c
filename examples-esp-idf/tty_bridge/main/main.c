/*
 * main.c -- ESP-IDF example: drive a MeshCore Companion Radio over a UART.
 *
 * Same portable C99 core as every other example (src/meshcore_companion.{c,h}),
 * here with the ESP-IDF UART driver as the byte transport. Wire the companion
 * radio's serial lines to MC_UART_TX_PIN / MC_UART_RX_PIN and flash this onto an
 * ESP32 / ESP32-S3 / ESP32-C3 etc.
 *
 *   idf.py set-target esp32s3
 *   idf.py build flash monitor
 *
 * The companion radio must run the serial companion firmware (companion_radio_usb)
 * with its interface bound to this UART.
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "meshcore_companion.h"

/* --- UART wiring: change to free pins on your board --- */
#define MC_UART_NUM     UART_NUM_1
#define MC_UART_TX_PIN  17           /* host TX -> companion RX */
#define MC_UART_RX_PIN  16           /* host RX <- companion TX */
#define MC_UART_BAUD    115200
#define MC_UART_BUFSZ   1024

static const char *TAG = "meshcore";

static void send_payload(const uint8_t *payload, size_t len)
{
    uint8_t frame[MC_RX_BUFSZ];
    size_t flen = mc_frame_encode(payload, len, frame, sizeof frame);
    if (flen) uart_write_bytes(MC_UART_NUM, (const char *)frame, flen);
}

static void on_event(const mc_event_t *ev)
{
    switch (ev->code) {
    case MC_RESP_DEVICE_INFO:
        ESP_LOGI(TAG, "radio model=%s fw=%d channels=%u build=%s",
                 ev->u.device_info.model, ev->u.device_info.fw_ver,
                 (unsigned)ev->u.device_info.max_channels,
                 ev->u.device_info.build_date);
        break;
    case MC_RESP_CHANNEL_MSG_RECV:   /* body is "SenderName: message" */
        ESP_LOGI(TAG, "[ch %d] %s", ev->u.channel_msg.channel_idx,
                 ev->u.channel_msg.text);
        break;
    case MC_RESP_CHANNEL_DATA_RECV: {
        /* SNR is q4 (x4). Print exact dB without floating-point printf. */
        int centi = ev->u.channel_data.snr_q4 * 25;   /* /4 then *100 */
        const char *sign = centi < 0 ? "-" : "";
        ESP_LOGI(TAG, "[ch %d] %u bytes type=0x%04X snr=%s%d.%02d dB %s",
                 ev->u.channel_data.channel_idx,
                 (unsigned)ev->u.channel_data.data_len,
                 (unsigned)ev->u.channel_data.data_type,
                 sign, abs(centi) / 100, abs(centi) % 100,
                 ev->u.channel_data.path_len == MC_PATH_DIRECT ? "direct" : "flood");
        break;
    }
    case MC_RESP_CURR_TIME:
        ESP_LOGI(TAG, "device time = %u (epoch secs)", (unsigned)ev->u.curr_time);
        break;
    case MC_RESP_ERR:
        ESP_LOGW(TAG, "radio error response (code=%d)", ev->u.err_code);
        break;
    default:
        break;
    }
}

void app_main(void)
{
    const uart_config_t cfg = {
        .baud_rate  = MC_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MC_UART_NUM, MC_UART_BUFSZ, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MC_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(MC_UART_NUM, MC_UART_TX_PIN, MC_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* Handshake: AppStart -> SelfInfo, DeviceQuery -> DeviceInfo. */
    uint8_t cmd[MC_MAX_PAYLOAD];
    size_t  n;
    n = mc_cmd_app_start(cmd, sizeof cmd, "esp-idf");   if (n) send_payload(cmd, n);
    n = mc_cmd_device_query(cmd, sizeof cmd, 1);        if (n) send_payload(cmd, n);
    n = mc_cmd_get_device_time(cmd, sizeof cmd);        if (n) send_payload(cmd, n);

    mc_rx_t rx;
    mc_rx_init(&rx);
    ESP_LOGI(TAG, "listening on UART%d (rx=%d tx=%d)",
             MC_UART_NUM, MC_UART_RX_PIN, MC_UART_TX_PIN);

    for (;;) {
        uint8_t in[128];
        int got = uart_read_bytes(MC_UART_NUM, in, sizeof in, pdMS_TO_TICKS(100));
        if (got <= 0) continue;

        mc_rx_feed(&rx, in, (size_t)got);

        uint8_t payload[MC_MAX_PAYLOAD];
        size_t  plen;
        while (mc_rx_poll(&rx, payload, sizeof payload, &plen)) {
            mc_event_t ev;
            if (!mc_parse(payload, plen, &ev)) continue;
            on_event(&ev);
            /* Drain the radio's queue when it signals waiting messages. */
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
