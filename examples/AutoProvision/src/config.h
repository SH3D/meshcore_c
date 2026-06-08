/*
 * config.h — user-tunable settings for the AutoProvision example.
 *
 * Pins and the radio region default here but are normally set per board in
 * platformio.ini build_flags; the #ifndef guards let either win. The identity,
 * channel and echo strings are plain constants — edit them for your setup.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AUTOPROVISION_CONFIG_H
#define AUTOPROVISION_CONFIG_H

#include <stdint.h>

// --- Host UART to the companion (override per board in platformio.ini) ---
#ifndef UART_RX_PIN
#define UART_RX_PIN  17          // host RX <- companion TX
#endif
#ifndef UART_TX_PIN
#define UART_TX_PIN  18          // host TX -> companion RX
#endif
#ifndef UART_BAUD
#define UART_BAUD    115200
#endif

// --- Radio region: AU narrow band (override in platformio.ini build_flags) ---
#ifndef LORA_FREQ
#define LORA_FREQ      916.575   // MHz
#endif
#ifndef LORA_BW
#define LORA_BW        62.5      // kHz
#endif
#ifndef LORA_SF
#define LORA_SF        7
#endif
#ifndef LORA_CR
#define LORA_CR        8
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER  20        // dBm
#endif

// --- Identity + channel to provision ---
static const char*   NODE_NAME       = "auto-host";
static const uint8_t CHANNEL_IDX     = 2;
static const char*   CHANNEL_NAME    = "auto";
static const char*   CHANNEL_PSK_HEX = "000102030405060708090a0b0c0d0e0f"; // replace

static const char*   HELLO_TEXT      = "hello";
static const char*   REPLY_TEXT      = "hi there, auto-host here";

#endif // AUTOPROVISION_CONFIG_H
