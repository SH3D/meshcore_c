/*
 * meshcore_info.c
 *
 * One-shot: open a tty, interrogate the attached MeshCore Companion Radio for
 * everything it will tell us -- model/firmware/version, radio parameters,
 * device time, every configured channel (name + PSK), and runtime stats -- then
 * print it and exit. Read-only; sends only query commands.
 *
 *   build:  via CMakeLists.txt at the repo root, or:
 *           cc -std=c99 -Wall -Wextra -I../../src \
 *              meshcore_info.c ../../src/meshcore_companion.c -o meshcore_info
 *
 *   run:    ./meshcore_info /dev/ttyACM0
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#define _DEFAULT_SOURCE   /* cfmakeraw, B115200, clock_gettime on glibc/c99 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "meshcore_companion.h"

#define MAX_CHANNELS 64

/* ------------------------------------------------------------- collected state */
typedef struct {
    int have_device, have_self, have_time;
    mc_device_info_t dev;
    mc_self_info_t   self;
    uint32_t         epoch;

    int               nchan;                 /* channels we queried */
    int               chan_got[MAX_CHANNELS];
    mc_channel_info_t chan[MAX_CHANNELS];

    int        have_core, have_radio, have_packets;
    mc_stats_t core, radio, packets;
} info_t;

/* ------------------------------------------------------------------ tty + I/O */
static int tty_open(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { fprintf(stderr, "tcgetattr: %s\n", strerror(errno)); close(fd); return -1; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { fprintf(stderr, "tcsetattr: %s\n", strerror(errno)); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void send_payload(int fd, const uint8_t *payload, size_t len) {
    uint8_t frame[MC_RX_BUFSZ];
    size_t flen = mc_frame_encode(payload, len, frame, sizeof frame);
    size_t off = 0;
    while (off < flen) {
        ssize_t w = write(fd, frame + off, flen - off);
        if (w < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------ event collection */
static void handle(info_t *c, const mc_event_t *ev) {
    switch (ev->code) {
    case MC_RESP_DEVICE_INFO: c->dev = ev->u.device_info; c->have_device = 1; break;
    case MC_RESP_SELF_INFO:   c->self = ev->u.self_info;   c->have_self = 1;   break;
    case MC_RESP_CURR_TIME:    c->epoch = ev->u.curr_time;  c->have_time = 1;   break;
    case MC_RESP_CHANNEL_INFO: {
        uint8_t i = ev->u.channel_info.channel_idx;
        if (i < MAX_CHANNELS) { c->chan[i] = ev->u.channel_info; c->chan_got[i] = 1; }
        break;
    }
    case MC_RESP_STATS:
        if      (ev->u.stats.subtype == MC_STATS_CORE)    { c->core = ev->u.stats;    c->have_core = 1; }
        else if (ev->u.stats.subtype == MC_STATS_RADIO)   { c->radio = ev->u.stats;   c->have_radio = 1; }
        else if (ev->u.stats.subtype == MC_STATS_PACKETS) { c->packets = ev->u.stats; c->have_packets = 1; }
        break;
    default: break;
    }
}

/* Pump the tty until `done(c)` is satisfied or `cap_ms` elapses. */
static void pump(int fd, mc_rx_t *rx, info_t *c, int (*done)(const info_t *), long cap_ms) {
    long deadline = now_ms() + cap_ms;
    for (;;) {
        if (done && done(c)) return;
        long rem = deadline - now_ms();
        if (rem <= 0) return;

        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { rem / 1000, (rem % 1000) * 1000 };
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; return; }
        if (r == 0) return;                       /* timed out */

        uint8_t in[256];
        ssize_t got = read(fd, in, sizeof in);
        if (got <= 0) { if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue; return; }
        mc_rx_feed(rx, in, (size_t)got);

        uint8_t pl[MC_MAX_PAYLOAD]; size_t pn;
        while (mc_rx_poll(rx, pl, sizeof pl, &pn)) {
            mc_event_t ev;
            if (mc_parse(pl, pn, &ev)) handle(c, &ev);
        }
    }
}

static int identity_ready(const info_t *c) { return c->have_device && c->have_self && c->have_time; }
static int details_ready(const info_t *c) {
    for (int i = 0; i < c->nchan; i++) if (!c->chan_got[i]) return 0;
    return c->have_core && c->have_radio && c->have_packets;
}

/* --------------------------------------------------------------------- output */
static void print_report(const info_t *c) {
    if (!c->have_device && !c->have_self) {
        printf("No response from the radio. Is it the serial companion firmware,\n"
               "bound to this port at 115200 8N1?\n");
        return;
    }

    if (c->have_device) {
        const mc_device_info_t *d = &c->dev;
        printf("Device\n");
        printf("  model         : %s\n", d->model);
        printf("  firmware ver  : %d\n", d->fw_ver);
        if (d->ver[0])        printf("  version       : %s\n", d->ver);
        if (d->build_date[0]) printf("  build date    : %s\n", d->build_date);
        printf("  max channels  : %u\n", d->max_channels);
        printf("  max contacts  : %u\n", d->max_contacts);
        printf("  BLE pin       : %u\n", d->ble_pin);
        if (d->have_repeat)    printf("  repeater mode : %s\n", d->repeat ? "yes" : "no");
        if (d->have_path_hash) printf("  path hash mode: %u\n", d->path_hash_mode);
    }

    if (c->have_self) {
        const mc_self_info_t *s = &c->self;
        printf("\nRadio / identity\n");
        printf("  name          : %s\n", s->name);
        printf("  public key    : ");
        for (int i = 0; i < 8; i++) printf("%02x", s->public_key[i]);
        printf("...\n");
        printf("  frequency     : %.3f MHz\n", s->radio_freq / 1000.0);
        printf("  bandwidth     : %.1f kHz\n", s->radio_bw / 1000.0);
        printf("  spreading     : SF%u\n", s->radio_sf);
        printf("  coding rate   : 4/%u\n", s->radio_cr);
        printf("  tx power      : %u dBm (max %u)\n", s->tx_power, s->max_tx_power);
        if (s->adv_lat || s->adv_lon)
            printf("  advert loc    : %.6f, %.6f\n", s->adv_lat / 1e6, s->adv_lon / 1e6);
        printf("  telemetry mode: base=%u loc=%u env=%u\n", s->tm_base, s->tm_loc, s->tm_env);
        printf("  multi acks    : %u\n", s->multi_acks);
    }

    if (c->have_time) {
        time_t t = (time_t)c->epoch;
        struct tm tmv; char buf[32] = "";
        if (gmtime_r(&t, &tmv)) strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tmv);
        printf("\nClock\n  device time   : %u (%s)\n", c->epoch, buf);
    }

    if (c->nchan > 0) {
        printf("\nChannels (%d)\n", c->nchan);
        for (int i = 0; i < c->nchan; i++) {
            if (!c->chan_got[i]) { printf("  [%2d] (no response)\n", i); continue; }
            const mc_channel_info_t *ch = &c->chan[i];
            int configured = ch->name[0] != 0;
            if (configured) {   // can print unconfigured
                printf("  [%2d] %-16s psk=", i, configured ? ch->name : "(unconfigured)");
                if (ch->have_secret) { for (int k = 0; k < MC_SECRET_LEN; k++) printf("%02x", ch->secret[k]); }
                else                 printf("(none)");
                printf("\n");
            }
        }
    }

    if (c->have_core || c->have_radio || c->have_packets) {
        printf("\nStats\n");
        if (c->have_core)
            printf("  battery       : %u mV   uptime %u s   errors %u   queue %u\n",
                   c->core.u.core.battery_mv, c->core.u.core.uptime_secs,
                   c->core.u.core.errors, c->core.u.core.queue_len);
        if (c->have_radio)
            printf("  radio         : noise %d   last RSSI %d   last SNR %.1f dB   air tx %u s rx %u s\n",
                   c->radio.u.radio.noise_floor, c->radio.u.radio.last_rssi,
                   (double)MC_SNR_DB(c->radio.u.radio.last_snr_q4),
                   c->radio.u.radio.tx_air_secs, c->radio.u.radio.rx_air_secs);
        if (c->have_packets) {
            const mc_stats_t *p = &c->packets;
            printf("  packets       : recv %u   sent %u   flood tx/rx %u/%u   direct tx/rx %u/%u",
                   p->u.packets.recv, p->u.packets.sent, p->u.packets.flood_tx,
                   p->u.packets.flood_rx, p->u.packets.direct_tx, p->u.packets.direct_rx);
            if (p->has_recv_errors) printf("   recv errors %u", p->u.packets.recv_errors);
            printf("\n");
        }
    }
}

/* ----------------------------------------------------------------------- main */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tty>   e.g. /dev/ttyACM0, /dev/ttyUSB0\n", argv[0]);
        return 2;
    }
    int fd = tty_open(argv[1]);
    if (fd < 0) return 1;

    info_t c;
    memset(&c, 0, sizeof c);
    mc_rx_t rx; mc_rx_init(&rx);

    uint8_t cmd[MC_MAX_PAYLOAD]; size_t n;

    /* Identity: AppStart -> SelfInfo, DeviceQuery -> DeviceInfo, plus time. */
    n = mc_cmd_app_start(cmd, sizeof cmd, "meshcore_info"); if (n) send_payload(fd, cmd, n);
    n = mc_cmd_device_query(cmd, sizeof cmd, 1);            if (n) send_payload(fd, cmd, n);
    n = mc_cmd_get_device_time(cmd, sizeof cmd);            if (n) send_payload(fd, cmd, n);
    pump(fd, &rx, &c, identity_ready, 2000);

    /* Now that we know the channel count, enumerate channels + ask for stats. */
    if (c.have_device) {
        c.nchan = c.dev.max_channels;
        if (c.nchan > MAX_CHANNELS) c.nchan = MAX_CHANNELS;
        for (int i = 0; i < c.nchan; i++) {
            n = mc_cmd_get_channel(cmd, sizeof cmd, (uint8_t)i);
            if (n) send_payload(fd, cmd, n);
        }
    }
    n = mc_cmd_get_stats(cmd, sizeof cmd, MC_STATS_CORE);    if (n) send_payload(fd, cmd, n);
    n = mc_cmd_get_stats(cmd, sizeof cmd, MC_STATS_RADIO);   if (n) send_payload(fd, cmd, n);
    n = mc_cmd_get_stats(cmd, sizeof cmd, MC_STATS_PACKETS); if (n) send_payload(fd, cmd, n);
    pump(fd, &rx, &c, details_ready, 3000);

    print_report(&c);
    close(fd);
    return c.have_device || c.have_self ? 0 : 1;
}
