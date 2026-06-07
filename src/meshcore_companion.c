/*
 * meshcore_companion.c  -- portable C99 core, no I/O, no malloc.
 * SPDX-License-Identifier: MIT
 */
#include "meshcore_companion.h"
#include <string.h>

/* ---- little-endian helpers ---- */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Copy a fixed-width NUL-padded cstring field of `field_len` into dst (cap incl
 * terminator). Advances nothing; returns field_len consumed by caller. */
static void copy_cstring(char *dst, size_t dst_cap, const uint8_t *src, size_t field_len) {
    size_t i = 0;
    for (; i < field_len && i + 1 < dst_cap; i++) {
        if (src[i] == 0) break;
        dst[i] = (char)src[i];
    }
    dst[i] = 0;
}

/* Copy remaining bytes [off..len) as a NUL-terminated string. */
static void copy_rest_string(char *dst, size_t dst_cap, const uint8_t *src, size_t off, size_t len) {
    size_t n = (off < len) ? (len - off) : 0, i = 0;
    for (; i < n && i + 1 < dst_cap; i++) dst[i] = (char)src[off + i];
    dst[i] = 0;
}

/* ======================================================================== */
void mc_rx_init(mc_rx_t *rx) { rx->len = 0; }

size_t mc_rx_feed(mc_rx_t *rx, const uint8_t *data, size_t n) {
    size_t space = sizeof(rx->buf) - rx->len, take = (n < space) ? n : space;
    memcpy(rx->buf + rx->len, data, take);
    rx->len += take;
    return take;
}

static void rx_drop_front(mc_rx_t *rx, size_t k) {
    if (k >= rx->len) { rx->len = 0; return; }
    memmove(rx->buf, rx->buf + k, rx->len - k);
    rx->len -= k;
}

int mc_rx_poll(mc_rx_t *rx, uint8_t *out, size_t out_cap, size_t *out_len) {
    while (rx->len >= 3) {
        uint8_t type = rx->buf[0];
        if (type != MC_FRAME_RADIO_TO_APP && type != MC_FRAME_APP_TO_RADIO) {
            rx_drop_front(rx, 1);            /* not a frame lead, resync */
            continue;
        }
        uint16_t flen = get_u16(rx->buf + 1);
        if (flen == 0) { rx_drop_front(rx, 1); continue; }
        if (flen > MC_MAX_PAYLOAD) {         /* cannot hold it; skip lead byte */
            rx_drop_front(rx, 1);
            continue;
        }
        size_t need = (size_t)3 + flen;
        if (rx->len < need) return 0;        /* wait for the rest */
        size_t copy = (flen < out_cap) ? flen : out_cap;
        memcpy(out, rx->buf + 3, copy);
        *out_len = copy;
        rx_drop_front(rx, need);
        return 1;
    }
    return 0;
}

/* ======================================================================== */
size_t mc_frame_encode(const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t out_cap) {
    if (payload_len > 0xFFFF || out_cap < payload_len + 3) return 0;
    out[0] = MC_FRAME_APP_TO_RADIO;
    put_u16(out + 1, (uint16_t)payload_len);
    memcpy(out + 3, payload, payload_len);
    return payload_len + 3;
}

/* ---- command builders ---- */
size_t mc_cmd_app_start(uint8_t *out, size_t cap, const char *app_name) {
    size_t nlen = app_name ? strlen(app_name) : 0;
    size_t total = 1 + 1 + 6 + nlen;
    if (cap < total) return 0;
    size_t i = 0;
    out[i++] = MC_CMD_APP_START;
    out[i++] = 1;                       /* app version */
    memset(out + i, 0, 6); i += 6;      /* reserved */
    memcpy(out + i, app_name, nlen); i += nlen;
    return i;
}

size_t mc_cmd_device_query(uint8_t *out, size_t cap, uint8_t app_target_ver) {
    if (cap < 2) return 0;
    out[0] = MC_CMD_DEVICE_QUERY; out[1] = app_target_ver; return 2;
}

size_t mc_cmd_get_device_time(uint8_t *out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = MC_CMD_GET_DEVICE_TIME;
    return 1;
}

size_t mc_cmd_set_device_time(uint8_t *out, size_t cap, uint32_t epoch_secs) {
    if (cap < 5) return 0;
    out[0] = MC_CMD_SET_DEVICE_TIME; put_u32(out + 1, epoch_secs); return 5;
}

size_t mc_cmd_sync_next_message(uint8_t *out, size_t cap) {
    if (cap < 1) return 0;
    out[0] = MC_CMD_SYNC_NEXT_MESSAGE;
    return 1;
}

size_t mc_cmd_send_self_advert(uint8_t *out, size_t cap, uint8_t advert_type) {
    if (cap < 2) return 0;
    out[0] = MC_CMD_SEND_SELF_ADVERT; out[1] = advert_type; return 2;
}

size_t mc_cmd_get_channel(uint8_t *out, size_t cap, uint8_t channel_idx) {
    if (cap < 2) return 0;
    out[0] = MC_CMD_GET_CHANNEL; out[1] = channel_idx; return 2;
}

size_t mc_cmd_set_channel(uint8_t *out, size_t cap, uint8_t channel_idx,
                          const char *name, const uint8_t secret[MC_SECRET_LEN]) {
    size_t total = 1 + 1 + MC_NAME_LEN + MC_SECRET_LEN;
    if (cap < total) return 0;
    size_t i = 0;
    out[i++] = MC_CMD_SET_CHANNEL;
    out[i++] = channel_idx;
    /* 32-byte NUL-padded name, last byte forced NUL (matches meshcore.js) */
    memset(out + i, 0, MC_NAME_LEN);
    if (name) {
        size_t nlen = strlen(name);
        if (nlen > MC_NAME_LEN - 1) nlen = MC_NAME_LEN - 1;
        memcpy(out + i, name, nlen);
    }
    i += MC_NAME_LEN;
    memcpy(out + i, secret, MC_SECRET_LEN); i += MC_SECRET_LEN;
    return i;
}

size_t mc_cmd_send_channel_text(uint8_t *out, size_t cap, uint8_t txt_type,
                                uint8_t channel_idx, uint32_t sender_ts,
                                const char *text) {
    size_t tlen = text ? strlen(text) : 0;
    size_t total = 1 + 1 + 1 + 4 + tlen;
    if (cap < total) return 0;
    size_t i = 0;
    out[i++] = MC_CMD_SEND_CHANNEL_TXT_MSG;
    out[i++] = txt_type;
    out[i++] = channel_idx;
    put_u32(out + i, sender_ts); i += 4;
    memcpy(out + i, text, tlen); i += tlen;
    return i;
}

size_t mc_cmd_set_radio_params(uint8_t *out, size_t cap, uint32_t freq_hz_x1000,
                               uint32_t bw, uint8_t sf, uint8_t cr) {
    if (cap < 11) return 0;
    size_t i = 0;
    out[i++] = MC_CMD_SET_RADIO_PARAMS;
    put_u32(out + i, freq_hz_x1000); i += 4;
    put_u32(out + i, bw);            i += 4;
    out[i++] = sf; out[i++] = cr;
    return i;
}

size_t mc_cmd_get_stats(uint8_t *out, size_t cap, uint8_t stats_type) {
    if (cap < 2) return 0;
    out[0] = MC_CMD_GET_STATS; out[1] = stats_type; return 2;
}

/* ======================================================================== */
int mc_parse(const uint8_t *p, size_t len, mc_event_t *ev) {
    if (len < 1) return 0;
    memset(ev, 0, sizeof(*ev));
    ev->code = p[0];
    const uint8_t *b = p + 1;          /* body after code byte */
    size_t n = len - 1;

    switch (ev->code) {
    case MC_RESP_OK:
    case MC_RESP_DISABLED:
    case MC_RESP_NO_MORE_MESSAGES:
    case MC_PUSH_MSG_WAITING:
        return 1;

    case MC_RESP_ERR:
        ev->u.err_code = (n >= 1) ? (int8_t)b[0] : (int8_t)-1;
        return 1;

    case MC_RESP_CURR_TIME:
        if (n < 4) return 0;
        ev->u.curr_time = get_u32(b);
        return 1;

    case MC_RESP_BATTERY_VOLTAGE:
        if (n < 2) return 0;
        ev->u.battery_mv = get_u16(b);
        return 1;

    case MC_RESP_DEVICE_INFO: {
        /* [fw_ver:i8][max_contacts/2:u8][max_channels:u8][ble_pin:u32]
           [build_date:cstr12][model:rest] */
        if (n < 1) return 0;
        mc_device_info_t *d = &ev->u.device_info;
        d->fw_ver = (int8_t)b[0];
        size_t off = 1;
        if (n >= off + 6) {
            d->max_contacts = (uint16_t)(b[off] * 2); off += 1;
            d->max_channels = b[off]; off += 1;
            d->ble_pin = get_u32(b + off); off += 4;
        }
        if (n >= off + 12) { copy_cstring(d->build_date, sizeof(d->build_date), b + off, 12); off += 12; }
        copy_rest_string(d->model, sizeof(d->model), b, off, n);
        return 1;
    }

    case MC_RESP_CHANNEL_MSG_RECV: {
        if (n < 7) return 0;
        mc_channel_msg_t *m = &ev->u.channel_msg;
        m->channel_idx = (int8_t)b[0];
        m->path_len    = b[1];
        m->txt_type    = b[2];
        m->sender_ts   = get_u32(b + 3);
        copy_rest_string(m->text, sizeof(m->text), b, 7, n);
        return 1;
    }

    case MC_RESP_CONTACT_MSG_RECV: {
        if (n < 12) return 0;
        mc_contact_msg_t *m = &ev->u.contact_msg;
        memcpy(m->pubkey_prefix, b, 6);
        m->path_len  = b[6];
        m->txt_type  = b[7];
        m->sender_ts = get_u32(b + 8);
        copy_rest_string(m->text, sizeof(m->text), b, 12, n);
        return 1;
    }

    case MC_RESP_CHANNEL_DATA_RECV: {
        if (n < 8) return 0;
        mc_channel_data_t *d = &ev->u.channel_data;
        d->snr_q4      = (int8_t)b[0];
        /* b[1], b[2] reserved */
        d->channel_idx = (int8_t)b[3];
        d->path_len    = b[4];
        d->data_type   = get_u16(b + 5);
        d->data_len    = b[7];
        size_t avail = n - 8;
        size_t dl = d->data_len;
        if (dl > avail) dl = avail;
        if (dl > sizeof(d->data)) dl = sizeof(d->data);
        memcpy(d->data, b + 8, dl);
        d->data_len = (uint8_t)dl;
        return 1;
    }

    case MC_RESP_CHANNEL_INFO: {
        if (n < 1 + MC_NAME_LEN) return 0;
        mc_channel_info_t *c = &ev->u.channel_info;
        c->channel_idx = b[0];
        copy_cstring(c->name, sizeof(c->name), b + 1, MC_NAME_LEN);
        size_t off = 1 + MC_NAME_LEN;
        if (n - off >= MC_SECRET_LEN) {
            memcpy(c->secret, b + off, MC_SECRET_LEN);
            c->have_secret = 1;
        }
        return 1;
    }

    case MC_RESP_SELF_INFO: {
        if (n < 55) return 0;
        mc_self_info_t *s = &ev->u.self_info;
        s->type = b[0]; s->tx_power = b[1]; s->max_tx_power = b[2];
        memcpy(s->public_key, b + 3, 32);
        s->adv_lat = (int32_t)get_u32(b + 35);
        s->adv_lon = (int32_t)get_u32(b + 39);
        /* b[43..45] reserved */
        s->manual_add_contacts = b[46];
        s->radio_freq = get_u32(b + 47);
        s->radio_bw   = get_u32(b + 51);
        s->radio_sf   = (n > 55) ? b[55] : 0;
        s->radio_cr   = (n > 56) ? b[56] : 0;
        copy_rest_string(s->name, sizeof(s->name), b, 57, n);
        return 1;
    }

    case MC_PUSH_ADVERT:
    case MC_PUSH_PATH_UPDATED:
        if (n < 32) return 0;
        memcpy(ev->u.pubkey32, b, 32);
        return 1;

    case MC_PUSH_SEND_CONFIRMED:
        if (n < 8) return 0;
        ev->u.send_confirmed.ack_code   = get_u32(b);
        ev->u.send_confirmed.round_trip = get_u32(b + 4);
        return 1;

    default:
        return 0;   /* recognised code byte set in ev->code, body not parsed */
    }
}
