/*
 * meshcore_companion.h
 *
 * Portable C99 client for the MeshCore Companion Radio serial protocol.
 *
 * This core has NO I/O, NO dynamic allocation and NO Arduino dependency.
 * You feed it bytes received from the radio and it hands you decoded frames;
 * you ask it to build command frames and it writes them into a buffer you own.
 * The transport (UART, USB-CDC, TCP, a unit-test harness) is entirely yours.
 *
 * Wire format (verified against meshcore.js, MIT, (c) Liam Cottle):
 *   frame = [type:u8][len:u16 LE][payload:len bytes]
 *   type 0x3C ('<')  app  -> radio   (commands we send)
 *   type 0x3E ('>')  radio -> app    (responses / push notifications we receive)
 *   payload[0] is the command code (outbound) or response/push code (inbound).
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 * Protocol reference: https://github.com/meshcore-dev/meshcore.js
 */
#ifndef MESHCORE_COMPANION_H
#define MESHCORE_COMPANION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time sizing (override before including if you need more) ---- */
#ifndef MC_MAX_PAYLOAD
#define MC_MAX_PAYLOAD   255   /* largest companion payload we will buffer    */
#endif
#ifndef MC_MAX_TEXT
#define MC_MAX_TEXT      184   /* max message text we keep (incl. NUL)        */
#endif
#ifndef MC_MAX_DATA
#define MC_MAX_DATA      184   /* max channel-data payload we keep            */
#endif
#ifndef MC_MAX_MODEL
#define MC_MAX_MODEL     48    /* DeviceInfo model/manufacturer string        */
#endif
#define MC_NAME_LEN      32    /* channel / advert name field width (cstring) */
#define MC_SECRET_LEN    16    /* 128-bit channel secret (PSK)                */
#define MC_RX_BUFSZ      (MC_MAX_PAYLOAD + 8)

/* ---- Frame lead bytes ---- */
#define MC_FRAME_APP_TO_RADIO  0x3C
#define MC_FRAME_RADIO_TO_APP  0x3E

/* ---- Command codes (app -> radio) ---- */
enum {
    MC_CMD_APP_START            = 1,
    MC_CMD_SEND_TXT_MSG         = 2,
    MC_CMD_SEND_CHANNEL_TXT_MSG = 3,
    MC_CMD_GET_CONTACTS         = 4,
    MC_CMD_GET_DEVICE_TIME      = 5,
    MC_CMD_SET_DEVICE_TIME      = 6,
    MC_CMD_SEND_SELF_ADVERT     = 7,
    MC_CMD_SET_ADVERT_NAME      = 8,
    MC_CMD_SYNC_NEXT_MESSAGE    = 10,
    MC_CMD_SET_RADIO_PARAMS     = 11,
    MC_CMD_SET_TX_POWER         = 12,
    MC_CMD_DEVICE_QUERY         = 22,
    MC_CMD_GET_CHANNEL          = 31,
    MC_CMD_SET_CHANNEL          = 32,
    MC_CMD_GET_STATS            = 56
};

/* ---- Response codes (radio -> app) ---- */
enum {
    MC_RESP_OK               = 0,
    MC_RESP_ERR              = 1,
    MC_RESP_CONTACTS_START   = 2,
    MC_RESP_CONTACT          = 3,
    MC_RESP_END_OF_CONTACTS  = 4,
    MC_RESP_SELF_INFO        = 5,
    MC_RESP_SENT             = 6,
    MC_RESP_CONTACT_MSG_RECV = 7,
    MC_RESP_CHANNEL_MSG_RECV = 8,
    MC_RESP_CURR_TIME        = 9,
    MC_RESP_NO_MORE_MESSAGES = 10,
    MC_RESP_EXPORT_CONTACT   = 11,
    MC_RESP_BATTERY_VOLTAGE  = 12,
    MC_RESP_DEVICE_INFO      = 13,
    MC_RESP_PRIVATE_KEY      = 14,
    MC_RESP_DISABLED         = 15,
    MC_RESP_CHANNEL_INFO     = 18,
    MC_RESP_STATS            = 24,
    MC_RESP_CHANNEL_DATA_RECV= 27
};

/* ---- Push codes (unsolicited, radio -> app) ---- */
enum {
    MC_PUSH_ADVERT        = 0x80,
    MC_PUSH_PATH_UPDATED  = 0x81,
    MC_PUSH_SEND_CONFIRMED= 0x82,
    MC_PUSH_MSG_WAITING   = 0x83,  /* "drain me": loop SYNC_NEXT_MESSAGE       */
    MC_PUSH_RAW_DATA      = 0x84,
    MC_PUSH_LOGIN_SUCCESS = 0x85,
    MC_PUSH_LOGIN_FAIL    = 0x86,
    MC_PUSH_STATUS_RESP   = 0x87,
    MC_PUSH_LOG_RX_DATA   = 0x88,
    MC_PUSH_TRACE_DATA    = 0x89,
    MC_PUSH_NEW_ADVERT    = 0x8A,
    MC_PUSH_TELEMETRY     = 0x8B,
    MC_PUSH_BINARY_RESP   = 0x8C
};

/* ---- Text message subtypes ---- */
enum { MC_TXT_PLAIN = 0, MC_TXT_CLI_DATA = 1, MC_TXT_SIGNED_PLAIN = 2 };
/* ---- Self-advert flood mode ---- */
enum { MC_ADVERT_ZERO_HOP = 0, MC_ADVERT_FLOOD = 1 };

#define MC_PATH_DIRECT 0xFF   /* path_len value meaning "received direct"     */
/* SNR is transmitted as a signed int8 scaled x4. Recover dB with this. */
#define MC_SNR_DB(q4)  ((float)(q4) / 4.0f)

/* ======================================================================== *
 *  Receive side: streaming frame assembler
 * ======================================================================== */
typedef struct {
    uint8_t buf[MC_RX_BUFSZ];
    size_t  len;
} mc_rx_t;

void mc_rx_init(mc_rx_t *rx);

/* Append received bytes. Returns the number of bytes accepted (bytes beyond
 * the buffer capacity are dropped; this only happens if a peer floods us with
 * a frame larger than MC_MAX_PAYLOAD, which the poller will resync past). */
size_t mc_rx_feed(mc_rx_t *rx, const uint8_t *data, size_t n);

/* Pull the next complete payload (the bytes after the 3-byte header) into
 * out[]. Returns 1 and sets *out_len if a frame is ready, 0 if more bytes are
 * needed. Call repeatedly until it returns 0. Garbage / oversized frames are
 * resynced automatically by skipping one byte at a time. */
int mc_rx_poll(mc_rx_t *rx, uint8_t *out, size_t out_cap, size_t *out_len);

/* ======================================================================== *
 *  Transmit side: wrap a payload into an on-wire app->radio frame
 * ======================================================================== */
/* Writes [0x3C][len LE][payload] into out[]. Returns total bytes, or 0 on
 * overflow / oversize. */
size_t mc_frame_encode(const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t out_cap);

/* ======================================================================== *
 *  Command payload builders (write payload only; wrap with mc_frame_encode)
 *  Each returns the payload length, or 0 if it would overflow `cap`.
 * ======================================================================== */
size_t mc_cmd_app_start        (uint8_t *out, size_t cap, const char *app_name);
size_t mc_cmd_device_query     (uint8_t *out, size_t cap, uint8_t app_target_ver);
size_t mc_cmd_get_device_time  (uint8_t *out, size_t cap);
size_t mc_cmd_set_device_time  (uint8_t *out, size_t cap, uint32_t epoch_secs);
size_t mc_cmd_sync_next_message(uint8_t *out, size_t cap);
size_t mc_cmd_send_self_advert (uint8_t *out, size_t cap, uint8_t advert_type);
size_t mc_cmd_get_channel      (uint8_t *out, size_t cap, uint8_t channel_idx);
size_t mc_cmd_set_channel      (uint8_t *out, size_t cap, uint8_t channel_idx,
                                const char *name, const uint8_t secret[MC_SECRET_LEN]);
size_t mc_cmd_send_channel_text(uint8_t *out, size_t cap, uint8_t txt_type,
                                uint8_t channel_idx, uint32_t sender_ts,
                                const char *text);
size_t mc_cmd_set_radio_params (uint8_t *out, size_t cap, uint32_t freq_hz_x1000,
                                uint32_t bw, uint8_t sf, uint8_t cr);
size_t mc_cmd_get_stats        (uint8_t *out, size_t cap, uint8_t stats_type);

/* ======================================================================== *
 *  Parsed events
 * ======================================================================== */
typedef struct {
    int8_t   fw_ver;
    uint16_t max_contacts;      /* already x2 from the wire field */
    uint8_t  max_channels;
    uint32_t ble_pin;
    char     build_date[16];
    char     model[MC_MAX_MODEL];
} mc_device_info_t;

typedef struct {
    int8_t   channel_idx;
    uint8_t  path_len;          /* MC_PATH_DIRECT or flood hop count */
    uint8_t  txt_type;
    uint32_t sender_ts;
    char     text[MC_MAX_TEXT]; /* for channel msgs this is "Name: body" */
} mc_channel_msg_t;

typedef struct {
    int8_t   snr_q4;            /* divide by 4 for dB; see MC_SNR_DB() */
    int8_t   channel_idx;
    uint8_t  path_len;
    uint16_t data_type;
    uint8_t  data_len;
    uint8_t  data[MC_MAX_DATA];
} mc_channel_data_t;

typedef struct {
    uint8_t  pubkey_prefix[6];
    uint8_t  path_len;
    uint8_t  txt_type;
    uint32_t sender_ts;
    char     text[MC_MAX_TEXT];
} mc_contact_msg_t;

typedef struct {
    uint8_t  channel_idx;
    char     name[MC_NAME_LEN + 1];
    uint8_t  secret[MC_SECRET_LEN];
    int      have_secret;
} mc_channel_info_t;

typedef struct {
    uint8_t  type, tx_power, max_tx_power;
    uint8_t  public_key[32];
    int32_t  adv_lat, adv_lon;
    uint8_t  manual_add_contacts;
    uint32_t radio_freq, radio_bw;
    uint8_t  radio_sf, radio_cr;
    char     name[MC_MAX_TEXT];
} mc_self_info_t;

typedef struct {
    uint8_t code;               /* response or push code (first payload byte) */
    union {
        mc_device_info_t  device_info;
        mc_channel_msg_t  channel_msg;
        mc_channel_data_t channel_data;
        mc_contact_msg_t  contact_msg;
        mc_channel_info_t channel_info;
        mc_self_info_t    self_info;
        uint32_t          curr_time;        /* epoch secs                    */
        uint16_t          battery_mv;
        int8_t            err_code;          /* MC_RESP_ERR (-1 if absent)    */
        uint8_t           pubkey32[32];      /* advert / path-updated pushes  */
        struct { uint32_t ack_code, round_trip; } send_confirmed;
    } u;
} mc_event_t;

/* Decode one received payload. Returns 1 if recognised (ev->code set and the
 * matching union member filled), 0 if the code is unknown to this build. */
int mc_parse(const uint8_t *payload, size_t len, mc_event_t *ev);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* MESHCORE_COMPANION_H */
