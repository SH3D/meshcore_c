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

/* little-endian writers for building test payloads */
static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Parse a freshly-built payload through the rx assembler; returns mc_parse result. */
static int feed_parse(mc_rx_t *rx, uint8_t *frame, uint8_t *scratch, size_t scap,
                      const uint8_t *payload, size_t plen, mc_event_t *ev) {
    size_t flen = make_inbound(frame, payload, plen), olen = 0;
    mc_rx_feed(rx, frame, flen);
    if (!mc_rx_poll(rx, scratch, scap, &olen)) return 0;
    return mc_parse(scratch, olen, ev);
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

    printf("== parse: MSG_SENT ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_SENT;
        payload[j++] = 1;                          /* type */
        le32(payload + j, 0x11223344); j += 4;     /* expected_ack */
        le32(payload + j, 5000);       j += 4;     /* suggested_timeout */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_SENT, "parse msg_sent");
        CHECK(ev.u.msg_sent.type == 1 && ev.u.msg_sent.expected_ack == 0x11223344u &&
              ev.u.msg_sent.suggested_timeout == 5000u, "msg_sent fields");
    }

    printf("== parse: STATS (core/radio/packets) ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_STATS; payload[j++] = MC_STATS_CORE;
        le16(payload + j, 4200); j += 2;           /* battery_mv */
        le32(payload + j, 86400); j += 4;          /* uptime_secs */
        le16(payload + j, 3); j += 2;              /* errors */
        payload[j++] = 7;                          /* queue_len */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.u.stats.subtype == MC_STATS_CORE && ev.u.stats.u.core.battery_mv == 4200 &&
              ev.u.stats.u.core.uptime_secs == 86400 && ev.u.stats.u.core.errors == 3 &&
              ev.u.stats.u.core.queue_len == 7, "stats core");

        j = 0;
        payload[j++] = MC_RESP_STATS; payload[j++] = MC_STATS_RADIO;
        le16(payload + j, (uint16_t)(int16_t)-120); j += 2;  /* noise_floor i16 */
        payload[j++] = (uint8_t)(int8_t)-90;                 /* last_rssi i8 */
        payload[j++] = 40;                                   /* last_snr_q4 */
        le32(payload + j, 1000); j += 4;                     /* tx_air_secs */
        le32(payload + j, 2000); j += 4;                     /* rx_air_secs */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.u.stats.subtype == MC_STATS_RADIO && ev.u.stats.u.radio.noise_floor == -120 &&
              ev.u.stats.u.radio.last_rssi == -90 && ev.u.stats.u.radio.last_snr_q4 == 40 &&
              ev.u.stats.u.radio.tx_air_secs == 1000 && ev.u.stats.u.radio.rx_air_secs == 2000,
              "stats radio (signed fields)");

        j = 0;
        payload[j++] = MC_RESP_STATS; payload[j++] = MC_STATS_PACKETS;
        le32(payload + j, 10); j += 4; le32(payload + j, 20); j += 4;
        le32(payload + j, 1);  j += 4; le32(payload + j, 2);  j += 4;
        le32(payload + j, 3);  j += 4; le32(payload + j, 4);  j += 4;
        le32(payload + j, 5);  j += 4;                        /* recv_errors */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.u.stats.u.packets.recv == 10 && ev.u.stats.u.packets.direct_rx == 4 &&
              ev.u.stats.has_recv_errors && ev.u.stats.u.packets.recv_errors == 5,
              "stats packets (+recv_errors)");
    }

    printf("== parse: SELF_INFO extended fields ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_SELF_INFO;
        payload[j++] = 1; payload[j++] = 22; payload[j++] = 30;   /* type, txp, maxtxp */
        for (int i = 0; i < 32; i++) payload[j++] = (uint8_t)i;   /* public_key */
        le32(payload + j, (uint32_t)(int32_t)-37000000); j += 4;  /* adv_lat */
        le32(payload + j, (uint32_t)(int32_t)145000000); j += 4;  /* adv_lon */
        payload[j++] = 1;            /* multi_acks */
        payload[j++] = 2;            /* adv_loc_policy */
        payload[j++] = 0x36;         /* telemetry_mode: base=2 loc=1 env=3 */
        payload[j++] = 1;            /* manual_add_contacts */
        le32(payload + j, 915000); j += 4;   /* radio_freq */
        le32(payload + j, 250000); j += 4;   /* radio_bw */
        payload[j++] = 11;           /* radio_sf */
        payload[j++] = 5;            /* radio_cr */
        memcpy(payload + j, "node", 4); j += 4;
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_SELF_INFO, "parse self_info");
        CHECK(ev.u.self_info.multi_acks == 1 && ev.u.self_info.adv_loc_policy == 2 &&
              ev.u.self_info.telemetry_mode == 0x36 && ev.u.self_info.tm_base == 2 &&
              ev.u.self_info.tm_loc == 1 && ev.u.self_info.tm_env == 3,
              "self_info multi_acks/loc_policy/telemetry split");
        CHECK(ev.u.self_info.adv_lat == -37000000 && ev.u.self_info.adv_lon == 145000000 &&
              ev.u.self_info.manual_add_contacts == 1 && ev.u.self_info.radio_freq == 915000 &&
              ev.u.self_info.radio_bw == 250000 && ev.u.self_info.radio_sf == 11 &&
              ev.u.self_info.radio_cr == 5 && strcmp(ev.u.self_info.name, "node") == 0,
              "self_info numeric + name");
    }

    printf("== parse: CHANNEL_MSG_RECV_V3 (SNR) + base sentinel ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_CHANNEL_MSG_RECV_V3;
        payload[j++] = 40;                       /* SNR q4 */
        payload[j++] = 0; payload[j++] = 0;      /* reserved */
        payload[j++] = 2;                        /* channel_idx */
        payload[j++] = MC_PATH_DIRECT;           /* path_len */
        payload[j++] = MC_TXT_PLAIN;             /* txt_type */
        le32(payload + j, 1700000000u); j += 4;  /* sender_ts */
        memcpy(payload + j, "Alice: hi", 9); j += 9;
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_CHANNEL_MSG_RECV_V3 && ev.u.channel_msg.snr_q4 == 40 &&
              ev.u.channel_msg.channel_idx == 2 && ev.u.channel_msg.path_len == MC_PATH_DIRECT &&
              ev.u.channel_msg.sender_ts == 1700000000u &&
              strcmp(ev.u.channel_msg.text, "Alice: hi") == 0, "channel_msg_v3 fields + SNR");

        j = 0;
        payload[j++] = MC_RESP_CHANNEL_MSG_RECV;
        payload[j++] = 0;             /* channel_idx */
        payload[j++] = 0;             /* path_len */
        payload[j++] = MC_TXT_PLAIN;  /* txt_type */
        le32(payload + j, 1); j += 4; memcpy(payload + j, "B: yo", 5); j += 5;
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.u.channel_msg.snr_q4 == MC_SNR_NONE && strcmp(ev.u.channel_msg.text, "B: yo") == 0,
              "channel_msg base has MC_SNR_NONE");
    }

    printf("== parse: CONTACT_MSG_RECV_V3 + signature skip ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_CONTACT_MSG_RECV_V3;
        payload[j++] = (uint8_t)(int8_t)-8;      /* SNR q4 */
        payload[j++] = 0; payload[j++] = 0;      /* reserved */
        for (int i = 0; i < 6; i++) payload[j++] = (uint8_t)(0xC0 + i);  /* pubkey_prefix */
        payload[j++] = 1;                        /* path_len */
        payload[j++] = MC_TXT_SIGNED_PLAIN;      /* txt_type=2 */
        le32(payload + j, 1700000001u); j += 4;  /* sender_ts */
        payload[j++]=0xAA; payload[j++]=0xBB; payload[j++]=0xCC; payload[j++]=0xDD; /* signature */
        memcpy(payload + j, "signed!", 7); j += 7;
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_CONTACT_MSG_RECV_V3 && ev.u.contact_msg.snr_q4 == -8 &&
              ev.u.contact_msg.txt_type == MC_TXT_SIGNED_PLAIN && ev.u.contact_msg.has_signature &&
              ev.u.contact_msg.signature[0] == 0xAA && ev.u.contact_msg.signature[3] == 0xDD &&
              ev.u.contact_msg.pubkey_prefix[0] == 0xC0 &&
              strcmp(ev.u.contact_msg.text, "signed!") == 0,
              "contact_msg_v3 signed: SNR/sig/text");
    }

    printf("== parse: DEVICE_INFO with ver/repeat/path_hash (fw=10) ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_DEVICE_INFO;
        payload[j++] = 10;                       /* fw_ver */
        payload[j++] = 50;                       /* max_contacts/2 */
        payload[j++] = 8;                        /* max_channels */
        le32(payload + j, 123456); j += 4;       /* ble_pin */
        memset(payload + j, 0, 12); memcpy(payload + j, "1 Jan 2026", 10); j += 12;
        memset(payload + j, 0, 40); memcpy(payload + j, "Heltec V3", 9);  j += 40;
        memset(payload + j, 0, 20); memcpy(payload + j, "v1.7.0", 6);     j += 20;
        payload[j++] = 1;                        /* repeat (fw>=9) */
        payload[j++] = 2;                        /* path_hash_mode (fw>=10) */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.u.device_info.fw_ver == 10 && ev.u.device_info.max_contacts == 100 &&
              strcmp(ev.u.device_info.model, "Heltec V3") == 0 &&
              strcmp(ev.u.device_info.ver, "v1.7.0") == 0 &&
              ev.u.device_info.have_repeat && ev.u.device_info.repeat == 1 &&
              ev.u.device_info.have_path_hash && ev.u.device_info.path_hash_mode == 2,
              "device_info ver/repeat/path_hash");
    }

    printf("== build: send_txt_msg / send_cmd ==\n");
    {
        uint8_t dst[6] = { 1, 2, 3, 4, 5, 6 };
        plen = mc_cmd_send_txt_msg(scratch, sizeof scratch, MC_TXT_PLAIN, 0, 1700000000u, dst, 6, "hi");
        CHECK(plen == 1 + 1 + 1 + 4 + 6 + 2 && scratch[0] == MC_CMD_SEND_TXT_MSG &&
              scratch[1] == MC_TXT_PLAIN && scratch[2] == 0, "send_txt_msg header");
        CHECK(scratch[3] == 0x00 && scratch[4] == 0xF1 && scratch[5] == 0x53 && scratch[6] == 0x65 &&
              scratch[7] == 1 && scratch[12] == 6 && scratch[13] == 'h' && scratch[14] == 'i',
              "send_txt_msg ts LE + dst + body");
        plen = mc_cmd_send_cmd(scratch, sizeof scratch, 1700000000u, dst, 6, "reboot");
        CHECK(plen == 1 + 1 + 1 + 4 + 6 + 6 && scratch[0] == MC_CMD_SEND_TXT_MSG &&
              scratch[1] == MC_TXT_CLI_DATA && scratch[2] == 0 && scratch[13] == 'r',
              "send_cmd uses CLI txt_type");
    }

    printf("== build: contact commands ==\n");
    {
        uint8_t key[32]; for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x10 + i);
        plen = mc_cmd_get_contacts(scratch, sizeof scratch, 0);
        CHECK(plen == 1 && scratch[0] == MC_CMD_GET_CONTACTS, "get_contacts (all)");
        plen = mc_cmd_get_contacts(scratch, sizeof scratch, 0x01020304);
        CHECK(plen == 5 && scratch[1] == 0x04 && scratch[4] == 0x01, "get_contacts (since lastmod LE)");
        plen = mc_cmd_remove_contact(scratch, sizeof scratch, key);
        CHECK(plen == 33 && scratch[0] == MC_CMD_REMOVE_CONTACT && scratch[1] == 0x10 &&
              scratch[32] == 0x2F, "remove_contact (code + 32-byte key)");
        plen = mc_cmd_reset_path(scratch, sizeof scratch, key);
        CHECK(plen == 33 && scratch[0] == MC_CMD_RESET_PATH, "reset_path");
        plen = mc_cmd_get_contact_by_key(scratch, sizeof scratch, key);
        CHECK(plen == 33 && scratch[0] == MC_CMD_GET_CONTACT_BY_KEY, "get_contact_by_key");
        plen = mc_cmd_export_contact(scratch, sizeof scratch, NULL);
        CHECK(plen == 1 && scratch[0] == MC_CMD_EXPORT_CONTACT, "export_contact (self)");
        plen = mc_cmd_export_contact(scratch, sizeof scratch, key);
        CHECK(plen == 33 && scratch[0] == MC_CMD_EXPORT_CONTACT, "export_contact (key)");

        mc_contact_t c; memset(&c, 0, sizeof c);
        memcpy(c.public_key, key, 32);
        c.type = 1; c.flags = 0x80; c.out_path_len = 2;
        c.out_path[0] = 0xAB; c.out_path[1] = 0xCD;
        strcpy(c.adv_name, "RepeaterX");
        c.last_advert = 1700000000u; c.adv_lat = -37000000; c.adv_lon = 145000000;
        plen = mc_cmd_add_update_contact(scratch, sizeof scratch, &c);
        CHECK(plen == 144 && scratch[0] == MC_CMD_ADD_UPDATE_CONTACT && scratch[1] == 0x10 &&
              scratch[33] == 1 && scratch[34] == 0x80 && scratch[35] == 2 &&
              scratch[36] == 0xAB, "add_update_contact header + path");
        CHECK(memcmp(scratch + 100, "RepeaterX", 9) == 0 && scratch[109] == 0 &&
              scratch[132] == 0x00 && scratch[133] == 0xF1,  /* last_advert 0x6553F100 LE */
              "add_update_contact name(32) + last_advert");
    }

    printf("== build: binary / anon requests ==\n");
    {
        uint8_t dst[32]; for (int i = 0; i < 32; i++) dst[i] = (uint8_t)(0x40 + i);
        plen = mc_cmd_send_binary_req(scratch, sizeof scratch, dst, MC_BINARY_REQ_STATUS, NULL, 0);
        CHECK(plen == 34 && scratch[0] == MC_CMD_SEND_BINARY_REQ && scratch[1] == 0x40 &&
              scratch[32] == 0x5F && scratch[33] == MC_BINARY_REQ_STATUS,
              "send_binary_req STATUS (code+dst+type)");
        uint8_t blob[3] = { 0xAA, 0xBB, 0xCC };
        plen = mc_cmd_send_anon_req(scratch, sizeof scratch, dst, MC_ANON_REQ_BASIC, blob, 3);
        CHECK(plen == 37 && scratch[0] == MC_CMD_SEND_ANON_REQ && scratch[33] == MC_ANON_REQ_BASIC &&
              scratch[34] == 0xAA && scratch[36] == 0xCC, "send_anon_req BASIC + data");
    }

    printf("== parse: contact stream (START / CONTACT / END) ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_RESP_CONTACTS_START;
        le32(payload + j, 2); j += 4;
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_CONTACTS_START && ev.u.contacts_count == 2, "contacts_start count");

        j = 0;
        payload[j++] = MC_RESP_CONTACT;
        for (int i = 0; i < 32; i++) payload[j++] = (uint8_t)(0x10 + i);  /* pubkey */
        payload[j++] = 1;                  /* type */
        payload[j++] = 0x80;               /* flags */
        payload[j++] = 2;                  /* out_path_len */
        memset(payload + j, 0, 64); payload[j] = 0xAB; payload[j + 1] = 0xCD; j += 64;
        memset(payload + j, 0, 32); memcpy(payload + j, "RepeaterX", 9); j += 32;
        le32(payload + j, 1700000000u); j += 4;          /* last_advert */
        le32(payload + j, (uint32_t)-37000000); j += 4;  /* adv_lat */
        le32(payload + j, (uint32_t)145000000); j += 4;  /* adv_lon */
        le32(payload + j, 4242u); j += 4;                /* lastmod */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_CONTACT && ev.u.contact.public_key[0] == 0x10 &&
              ev.u.contact.type == 1 && ev.u.contact.flags == 0x80 &&
              ev.u.contact.out_path_len == 2 && ev.u.contact.out_path[1] == 0xCD &&
              strcmp(ev.u.contact.adv_name, "RepeaterX") == 0 &&
              ev.u.contact.last_advert == 1700000000u && ev.u.contact.adv_lat == -37000000 &&
              ev.u.contact.adv_lon == 145000000 && ev.u.contact.lastmod == 4242u,
              "contact record fields");

        j = 0;
        payload[j++] = MC_RESP_END_OF_CONTACTS;
        le32(payload + j, 4242u); j += 4;
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_RESP_END_OF_CONTACTS && ev.u.contacts_lastmod == 4242u,
              "end_of_contacts lastmod");

        j = 0;
        payload[j++] = MC_PUSH_CONTACT_DELETED;
        for (int i = 0; i < 32; i++) payload[j++] = (uint8_t)(0x10 + i);
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_PUSH_CONTACT_DELETED && ev.u.deleted_key[31] == 0x2F,
              "contact_deleted key");
    }

    printf("== parse: BINARY_RESPONSE + STATUS decode ==\n");
    {
        size_t j = 0;
        payload[j++] = MC_PUSH_BINARY_RESP;
        payload[j++] = 0;                       /* reserved */
        le32(payload + j, 0xDEADBEEFu); j += 4; /* tag */
        size_t blob = j;                        /* STATUS blob starts here */
        le16(payload + j, 4200); j += 2;        /* bat */
        le16(payload + j, 1); j += 2;           /* tx_queue_len */
        le16(payload + j, (uint16_t)(int16_t)-115); j += 2;  /* noise_floor */
        le16(payload + j, (uint16_t)(int16_t)-90);  j += 2;  /* last_rssi */
        le32(payload + j, 1000); j += 4; le32(payload + j, 500); j += 4;   /* nb_recv, nb_sent */
        le32(payload + j, 200); j += 4;  le32(payload + j, 3600); j += 4;  /* airtime, uptime */
        le32(payload + j, 10); j += 4; le32(payload + j, 20); j += 4;      /* sent_flood/direct */
        le32(payload + j, 30); j += 4; le32(payload + j, 40); j += 4;      /* recv_flood/direct */
        le16(payload + j, 2); j += 2;           /* full_evts */
        le16(payload + j, 36); j += 2;          /* last_snr (q4 -> 9 dB) */
        le16(payload + j, 1); j += 2; le16(payload + j, 2); j += 2;        /* dups */
        le32(payload + j, 150); j += 4;         /* rx_airtime */
        le32(payload + j, 5); j += 4;           /* recv_errors */
        CHECK(feed_parse(&rx, frame, scratch, sizeof scratch, payload, j, &ev) == 1 &&
              ev.code == MC_PUSH_BINARY_RESP && ev.u.binary_resp.tag == 0xDEADBEEFu &&
              ev.u.binary_resp.data_len == (uint8_t)(j - blob), "binary_resp tag + data_len");

        mc_status_t st;
        CHECK(mc_parse_status(ev.u.binary_resp.data, ev.u.binary_resp.data_len, &st) == 1 &&
              st.bat_mv == 4200 && st.noise_floor == -115 && st.last_rssi == -90 &&
              st.nb_recv == 1000 && st.uptime_secs == 3600 && st.last_snr_q4 == 36 &&
              st.rx_airtime_secs == 150 && st.has_recv_errors && st.recv_errors == 5,
              "parse_status fields");
    }

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
