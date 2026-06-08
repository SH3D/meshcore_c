/*
 * AutoProvision — main.cpp
 *
 * Zero-config host: plug in an ESP32-S3 dev board wired to a MeshCore companion
 * radio and it programs the radio the way it wants on boot — node name, radio
 * region (AU narrow band here) and a channel — then loops watching that channel
 * and replies to any "hello" with a response.
 *
 * The radio keeps these settings, so this is a one-button way to provision a
 * fresh companion. The LORA_* region values come from platformio.ini build
 * flags (the same defines you'd compile MeshCore firmware with); the #ifndef
 * fallbacks below just document the defaults.
 *
 * Project layout: this is a PlatformIO project (platformio.ini + src/main.cpp).
 * The code is plain Arduino C++ — to use it as an Arduino IDE sketch instead,
 * rename this file to AutoProvision.ino, move it up one level, and drop
 * platformio.ini (set the LORA_* defines below or in the IDE build flags).
 *
 * Wiring: host TX -> companion RX, host RX <- companion TX, GND<->GND. The
 * companion must run the serial companion firmware with its interface on a
 * hardware UART (see the README "Build a companion radio" section).
 *
 * SPDX-License-Identifier: MIT
 */
#include <Arduino.h>
#include "MeshCoreCompanion.h"
#include "config.h"             // pins, region, identity/channel — edit there

MeshCoreCompanion mc(Serial1);

static void provision() {
    mc.setAdvertName(NODE_NAME);
    mc.setRadioParams(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR);
    mc.setTxPower(LORA_TX_POWER);
    mc.setChannelHexSecret(CHANNEL_IDX, CHANNEL_NAME, CHANNEL_PSK_HEX);
    mc.getDeviceTime();          // so our replies get a sensible timestamp
    mc.sendSelfAdvert(true);     // announce ourselves on the new settings
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.printf("[boot] test auto provision started\n");
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    mc.onDeviceInfo([](const mc_device_info_t& d) {
        Serial.printf("[radio] %s fw=%d — provisioning...\n", d.model, d.fw_ver);
        provision();
        Serial.printf("[radio] set name=%s  %.3f MHz / %.1f kHz  SF%d CR4/%d  %ddBm  channel '%s'\n",
                      NODE_NAME, (double)LORA_FREQ, (double)LORA_BW,
                      (int)LORA_SF, (int)LORA_CR, (int)LORA_TX_POWER, CHANNEL_NAME);
    });

    // Watch the channel; reply to any message whose body is "hello".
    mc.onChannelMessage([](const mc_channel_msg_t& m) {
        Serial.printf("[ch %d] %s\n", m.channel_idx, m.text);   // "Sender: body"
        const char* body = strchr(m.text, ':');
        body = body ? body + 1 : m.text;
        while (*body == ' ') body++;
        if (strcasecmp(body, HELLO_TEXT) == 0)
            mc.sendChannelText(CHANNEL_IDX, REPLY_TEXT);
    });

    mc.begin();   // AppStart + DeviceQuery -> onDeviceInfo fires when the radio answers
}

void loop() {
    mc.loop();    // pump + auto-drain
}
