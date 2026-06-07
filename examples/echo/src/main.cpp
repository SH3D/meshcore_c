/*
 * echo example -- decode a MeshCore channel to the USB serial console.
 *
 * Listens to a companion radio on a hardware UART (Serial1) and echoes every
 * decoded channel message and data frame to the USB serial monitor (Serial).
 * A read-only "what's on the mesh" window: no sending, no heartbeat.
 *
 * Built as a self-contained PlatformIO project (see platformio.ini next to this
 * file) that depends on the library at the repo root via a relative symlink.
 *
 *   cd examples/echo && pio run -t upload && pio device monitor
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#include <Arduino.h>
#include "MeshCoreCompanion.h"

// --- UART to the companion radio (CHANGE to free GPIOs on your S3 board) ---
static const int      UART_RX_PIN = 16;   // host RX <- companion TX
static const int      UART_TX_PIN = 17;   // host TX -> companion RX
static const uint32_t UART_BAUD   = 115200;

// Channel to listen on. The radio decodes channel traffic, so it needs the PSK;
// we program it on connect. Replace with your real 16-byte PSK (32 hex chars).
static const uint8_t  CHANNEL_IDX      = 2;
static const char*    CHANNEL_NAME     = "sensors";
static const char*    CHANNEL_PSK_HEX  = "000102030405060708090a0b0c0d0e0f";

MeshCoreCompanion mc(Serial1);

void setup() {
    Serial.begin(115200);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    mc.onDeviceInfo([](const mc_device_info_t& d) {
        Serial.printf("[radio] %s  fw=%d  channels=%u  build=%s\n",
                      d.model, d.fw_ver, d.max_channels, d.build_date);
        // Make sure the radio is on the channel we want to echo.
        mc.setChannelHexSecret(CHANNEL_IDX, CHANNEL_NAME, CHANNEL_PSK_HEX);
    });

    // Decoded channel TEXT -> serial. Body is "SenderName: message".
    mc.onChannelMessage([](const mc_channel_msg_t& m) {
        Serial.printf("[ch %d] %s\n", m.channel_idx, m.text);
    });

    // Decoded channel DATA -> serial, with link metadata.
    mc.onChannelData([](const mc_channel_data_t& d) {
        Serial.printf("[ch %d] %u bytes  type=0x%04X  snr=%.1f dB  %s\n",
                      d.channel_idx, d.data_len, d.data_type,
                      MC_SNR_DB(d.snr_q4),
                      d.path_len == MC_PATH_DIRECT ? "direct" : "flood");
    });

    mc.begin();   // resets RX, sends AppStart + DeviceQuery
    Serial.println("echo: listening...");
}

void loop() {
    mc.loop();    // pump serial; auto-drains the radio queue on MsgWaiting
}
