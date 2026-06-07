/*
 * test_codec.c -- host-side unit test for the portable core.
 * Build & run:  cc -std=c99 -Wall -Wextra -I../src test_codec.c ../src/meshcore_companion.c -o t && ./t
 * SPDX-License-Identifier: MIT
 */
#include "meshcore_companion.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else      { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* Build a radio->app frame (0x3E + len + payload) for feeding the assembler. */
static size_t make_inbound(uint8_t *out, const uint8_t *payload, size_t plen) {
    out[0] = MC_FRAME_RADIO_TO_APP;
    out[1] = (uint8_t)plen; out[2] = (uint8_t)(plen >> 8);
    memcpy(out + 3, payload, plen);
    return plen + 3;
}

int main(void) {
    uint8_t scratch[512], frame[512], payload[300];
    size_t plen, flen, olen;

    printf("== command builders ==\n");
    plen = mc_cmd_device_query(scratch, sizeof scratch, 1);
    CHECK(plen == 2 && scratch[0] == MC_CMD_DEVICE_QUERY && scratch[1] == 1, "device_query payload");

    flen = mc_frame_encode(scratch, plen, frame, sizeof frame);
    CHECK(flen == 5 && frame[0] == MC_FRAME_APP_TO_RADIO && frame[1] == 2 && frame[2] == 0,
          "frame_encode wraps with 0x3C + len16");

    /* set_channel: 1 + 1 + 32 + 16 = 50 bytes */
    uint8_t secret[16];
    for (int i = 0; i < 16; i++) secret[i] = (uint8_t)(0xA0 + i);
    plen = mc_cmd_set_channel(scratch, sizeof scratch, 2, "sensors", secret);
    CHECK(plen == 50 && scratch[0] == MC_CMD_SET_CHANNEL && scratch[1] == 2, "set_channel length & header");
    CHECK(memcmp(scratch + 2, "sensors", 7) == 0 && scratch[2 + 7] == 0, "set_channel name NUL-padded");
    CHECK(memcmp(scratch + 2 + 32, secret, 16) == 0, "set_channel secret tail");

    plen = mc_cmd_send_channel_text(scratch, sizeof scratch, MC_TXT_PLAIN, 2, 0x11223344, "tank=87%");
    CHECK(scratch[0] == MC_CMD_SEND_CHANNEL_TXT_MSG && scratch[1] == MC_TXT_PLAIN && scratch[2] == 2,
          "send_channel_text header");
    CHECK(scratch[3] == 0x44 && scratch[6] == 0x11, "send_channel_text timestamp LE");
    CHECK(memcmp(scratch + 7, "tank=87%", 8) == 0, "send_channel_text body");

    printf("== rx assembler + parse: DeviceInfo ==\n");
    mc_rx_t rx; mc_rx_init(&rx);
    /* fw_ver=8, maxc/2=50, maxch=8, blepin=123456, build="7 Jun 2026", model="XIAO-S3" */
    size_t k = 0;
    payload[k++] = MC_RESP_DEVICE_INFO;
    payload[k++] = 8;
    payload[k++] = 50;
    payload[k++] = 8;
    payload[k++] = 0x40; payload[k++] = 0xE2; payload[k++] = 0x01; payload[k++] = 0x00; /* 123456 LE */
    memcpy(payload + k, "7 Jun 2026\0", 12); k += 12;  /* 12-byte cstring field */
    memcpy(payload + k, "XIAO-S3", 7); k += 7;
    flen = make_inbound(frame, payload, k);
    /* feed in two awkward chunks to exercise reassembly */
    mc_rx_feed(&rx, frame, 4);
    mc_rx_feed(&rx, frame + 4, flen - 4);
    mc_event_t ev;
    int got = mc_rx_poll(&rx, scratch, sizeof scratch, &olen);
    CHECK(got == 1, "poll produced a frame from split feed");
    CHECK(mc_parse(scratch, olen, &ev) == 1 && ev.code == MC_RESP_DEVICE_INFO, "parse device_info");
    CHECK(ev.u.device_info.fw_ver == 8 && ev.u.device_info.max_contacts == 100 &&
          ev.u.device_info.max_channels == 8 && ev.u.device_info.ble_pin == 123456,
          "device_info numeric fields");
    CHECK(strcmp(ev.u.device_info.build_date, "7 Jun 2026") == 0, "device_info build date");
    CHECK(strcmp(ev.u.device_info.model, "XIAO-S3") == 0, "device_info model");

    printf("== parse: ChannelMsgRecv (text) ==\n");
    k = 0;
    payload[k++] = MC_RESP_CHANNEL_MSG_RECV;
    payload[k++] = 2;              /* channel idx */
    payload[k++] = MC_PATH_DIRECT; /* path len */
    payload[k++] = MC_TXT_PLAIN;
    payload[k++]=0x44;payload[k++]=0x33;payload[k++]=0x22;payload[k++]=0x11; /* ts */
    memcpy(payload + k, "node3: tank=87%", 15); k += 15;
    flen = make_inbound(frame, payload, k);
    mc_rx_feed(&rx, frame, flen);
    got = mc_rx_poll(&rx, scratch, sizeof scratch, &olen);
    CHECK(got == 1 && mc_parse(scratch, olen, &ev) == 1 && ev.code == MC_RESP_CHANNEL_MSG_RECV,
          "parse channel_msg");
    CHECK(ev.u.channel_msg.channel_idx == 2 && ev.u.channel_msg.path_len == MC_PATH_DIRECT &&
          ev.u.channel_msg.sender_ts == 0x11223344, "channel_msg fields");
    CHECK(strcmp(ev.u.channel_msg.text, "node3: tank=87%") == 0, "channel_msg text (Name: body)");

    printf("== parse: ChannelDataRecv (metadata) ==\n");
    k = 0;
    payload[k++] = MC_RESP_CHANNEL_DATA_RECV;
    payload[k++] = (uint8_t)40;    /* snr q4 = 40 -> 10.0 dB */
    payload[k++] = 0; payload[k++] = 0;
    payload[k++] = 2;              /* channel idx */
    payload[k++] = 3;              /* path len (flood, 3 hops) */
    payload[k++] = 0xFF; payload[k++] = 0xFF; /* data type 0xFFFF (Dev) */
    payload[k++] = 4;              /* data len */
    payload[k++]=0xDE;payload[k++]=0xAD;payload[k++]=0xBE;payload[k++]=0xEF;
    flen = make_inbound(frame, payload, k);
    mc_rx_feed(&rx, frame, flen);
    got = mc_rx_poll(&rx, scratch, sizeof scratch, &olen);
    CHECK(got == 1 && mc_parse(scratch, olen, &ev) == 1 && ev.code == MC_RESP_CHANNEL_DATA_RECV,
          "parse channel_data");
    CHECK(ev.u.channel_data.snr_q4 == 40 && MC_SNR_DB(ev.u.channel_data.snr_q4) == 10.0f,
          "channel_data SNR x4 decode");
    CHECK(ev.u.channel_data.path_len == 3 && ev.u.channel_data.data_type == 0xFFFF &&
          ev.u.channel_data.data_len == 4 && ev.u.channel_data.data[0] == 0xDE &&
          ev.u.channel_data.data[3] == 0xEF, "channel_data payload");

    printf("== resync: garbage before a valid frame ==\n");
    uint8_t junk[3] = { 0x00, 0x99, 0x01 };
    mc_rx_feed(&rx, junk, 3);
    uint8_t okp[1] = { MC_RESP_OK };
    flen = make_inbound(frame, okp, 1);
    mc_rx_feed(&rx, frame, flen);
    got = mc_rx_poll(&rx, scratch, sizeof scratch, &olen);
    CHECK(got == 1 && mc_parse(scratch, olen, &ev) == 1 && ev.code == MC_RESP_OK,
          "resync past junk to find OK frame");

    printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
