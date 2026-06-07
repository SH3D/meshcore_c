/*
 * SensorChannelBridge.ino
 *
 * Talks to a MeshCore Companion Radio (e.g. a Seeed XIAO + Wio-SX1262 running
 * the companion_radio_usb firmware, with its serial interface routed to a
 * hardware UART) over a Grove connector. Demonstrates the minimum a display
 * board needs: handshake, set the sensors channel PSK, receive the channel
 * message stream, and send on the channel.
 *
 * Wire the Grove connector: VCC, GND, companion-TX -> host-RX, host-TX -> companion-RX.
 * Adjust UART_RX_PIN / UART_TX_PIN to the GPIOs your Grove port exposes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <Arduino.h>
#include "MeshCoreCompanion.h"

// --- Grove UART pins (CHANGE to match your board's free Grove port) ---
static const int UART_RX_PIN = 16;   // host RX  <- companion TX
static const int UART_TX_PIN = 17;   // host TX  -> companion RX
static const uint32_t UART_BAUD = 115200;

// Sensors channel: index + name + 16-byte PSK (here as 32 hex chars).
static const uint8_t SENSORS_CHANNEL_IDX = 2;
static const char*   SENSORS_CHANNEL_NAME = "sensors";
static const char*   SENSORS_CHANNEL_PSK_HEX = "000102030405060708090a0b0c0d0e0f"; // replace

MeshCoreCompanion mc(Serial1);

void setup() {
    Serial.begin(115200);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    // Print the radio identity once it answers the handshake.
    mc.onDeviceInfo([](const mc_device_info_t& d) {
        Serial.printf("[radio] %s  fw=%d  channels=%u  build=%s\n",
                      d.model, d.fw_ver, d.max_channels, d.build_date);
        // Now that we know it's alive, program the sensors channel PSK.
        mc.setChannelHexSecret(SENSORS_CHANNEL_IDX, SENSORS_CHANNEL_NAME, SENSORS_CHANNEL_PSK_HEX);
        mc.getDeviceTime();   // so outgoing messages get a sensible timestamp
    });

    // Incoming channel TEXT messages. Body is "SenderName: text".
    mc.onChannelMessage([](const mc_channel_msg_t& m) {
        Serial.printf("[ch %d] %s\n", m.channel_idx, m.text);
        // -> push m.text to your display here
    });

    // Incoming channel DATA (binary) messages, with SNR / path metadata.
    mc.onChannelData([](const mc_channel_data_t& d) {
        Serial.printf("[ch %d] %u bytes, type=0x%04X, snr=%.1f dB, %s\n",
                      d.channel_idx, d.data_len, d.data_type,
                      MC_SNR_DB(d.snr_q4),
                      d.path_len == MC_PATH_DIRECT ? "direct" : "flood");
    });

    mc.begin();   // resets RX, sends AppStart + DeviceQuery
}

void loop() {
    mc.loop();    // pump serial; auto-drains the queue on MsgWaiting

    // Example: send a heartbeat on the sensors channel every 60 s.
    static uint32_t last = 0;
    if (millis() - last > 60000) {
        last = millis();
        mc.sendChannelText(SENSORS_CHANNEL_IDX, "display online");
    }
}
