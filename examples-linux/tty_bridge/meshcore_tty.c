/*
 * meshcore_tty.c
 *
 * Portable-C example: talk to a MeshCore Companion Radio over any Linux tty
 * (USB-CDC like /dev/ttyACM0, a USB-serial adapter like /dev/ttyUSB0, or a raw
 * UART like /dev/ttyAMA0 / /dev/serial0). It demonstrates that the protocol core
 * in src/meshcore_companion.{c,h} needs nothing but a byte transport: here that
 * transport is POSIX termios + select(), entirely owned by this file.
 *
 *   build:  see CMakeLists.txt at the repo root, or:
 *           cc -std=c99 -Wall -Wextra -I../../src \
 *              meshcore_tty.c ../../src/meshcore_companion.c -o meshcore_tty
 *
 *   run:    ./meshcore_tty /dev/ttyACM0
 *           ./meshcore_tty /dev/ttyUSB0 2 000102030405060708090a0b0c0d0e0f sensors
 *                          ^tty        ^ch ^16-byte PSK as 32 hex chars       ^name
 *
 * The companion radio must run the serial companion firmware (companion_radio_usb)
 * with its interface bound to this port (not BLE, not the WiFi/TCP variant).
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#define _DEFAULT_SOURCE   /* cfmakeraw, B115200 on glibc under -std=c99 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "meshcore_companion.h"

/* ------------------------------------------------------------------ tty I/O */

/* Open `path` and put it in raw 8N1 mode at 115200 baud. Returns fd or -1. */
static int tty_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        fprintf(stderr, "tcgetattr %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);              /* no echo, no canonical, 8-bit clean */
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;          /* non-blocking read: return whatever is there */
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "tcsetattr %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* Frame a command payload and write the whole thing to the radio. */
static int send_payload(int fd, const uint8_t *payload, size_t len)
{
    uint8_t frame[MC_RX_BUFSZ];
    size_t flen = mc_frame_encode(payload, len, frame, sizeof frame);
    if (flen == 0) return -1;

    size_t off = 0;
    while (off < flen) {
        ssize_t w = write(fd, frame + off, flen - off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            fprintf(stderr, "write: %s\n", strerror(errno));
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* --------------------------------------------------------------- arg helpers */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse 32 hex chars into a 16-byte PSK. Returns 0 on success. */
static int parse_psk(const char *hex, uint8_t out[MC_SECRET_LEN])
{
    if (strlen(hex) != (size_t)MC_SECRET_LEN * 2) return -1;
    for (int i = 0; i < MC_SECRET_LEN; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ----------------------------------------------------------- event handling */

static void on_event(const mc_event_t *ev)
{
    switch (ev->code) {
    case MC_RESP_DEVICE_INFO: {
        const mc_device_info_t *d = &ev->u.device_info;
        printf("[radio] model=%s  fw=%d  channels=%u  build=%s\n",
               d->model, d->fw_ver, d->max_channels, d->build_date);
        break;
    }
    case MC_RESP_SELF_INFO:
        printf("[radio] name=%s  freq=%u  bw=%u  sf=%u  cr=%u\n",
               ev->u.self_info.name, ev->u.self_info.radio_freq,
               ev->u.self_info.radio_bw, ev->u.self_info.radio_sf,
               ev->u.self_info.radio_cr);
        break;
    case MC_RESP_CHANNEL_INFO: {
        const mc_channel_info_t *c = &ev->u.channel_info;
        printf("[chan %u] name=%s  secret=%s\n",
               c->channel_idx, c->name, c->have_secret ? "set" : "(none)");
        break;
    }
    case MC_RESP_CHANNEL_MSG_RECV:   /* text body is "SenderName: message" */
        printf("[ch %d] %s\n", ev->u.channel_msg.channel_idx,
               ev->u.channel_msg.text);
        break;
    case MC_RESP_CHANNEL_DATA_RECV: {
        const mc_channel_data_t *d = &ev->u.channel_data;
        printf("[ch %d] %u bytes  type=0x%04X  snr=%.1f dB  %s\n",
               d->channel_idx, d->data_len, d->data_type,
               (double)MC_SNR_DB(d->snr_q4),
               d->path_len == MC_PATH_DIRECT ? "direct" : "flood");
        break;
    }
    case MC_RESP_CURR_TIME:
        printf("[radio] device time = %u (epoch secs)\n", ev->u.curr_time);
        break;
    case MC_RESP_OK:
        break;   /* command acknowledged */
    case MC_RESP_ERR:
        printf("[radio] error response (code=%d)\n", ev->u.err_code);
        break;
    case MC_PUSH_MSG_WAITING:
        /* The radio has queued messages; caller drains via SyncNextMessage. */
        break;
    default:
        printf("[radio] event code 0x%02X\n", ev->code);
        break;
    }
}

/* -------------------------------------------------------------------- main */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <tty> [channel_idx psk_hex [name]]\n"
        "  <tty>         e.g. /dev/ttyACM0, /dev/ttyUSB0, /dev/serial0\n"
        "  channel_idx   optional channel to program (0-255)\n"
        "  psk_hex       optional 16-byte PSK as 32 hex chars\n"
        "  name          optional channel name (default \"channel\")\n",
        argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }

    /* Optional channel programming. */
    int      set_channel = 0;
    uint8_t  channel_idx = 0;
    uint8_t  psk[MC_SECRET_LEN];
    const char *channel_name = "channel";
    if (argc >= 4) {
        channel_idx = (uint8_t)strtoul(argv[2], NULL, 0);
        if (parse_psk(argv[3], psk) != 0) {
            fprintf(stderr, "psk_hex must be exactly 32 hex chars\n");
            return 2;
        }
        if (argc >= 5) channel_name = argv[4];
        set_channel = 1;
    }

    int fd = tty_open(argv[1]);
    if (fd < 0) return 1;

    signal(SIGINT, on_sigint);

    /* Handshake: AppStart triggers SelfInfo, DeviceQuery triggers DeviceInfo. */
    uint8_t cmd[MC_MAX_PAYLOAD];
    size_t  n;
    n = mc_cmd_app_start(cmd, sizeof cmd, "meshcore_tty");
    if (n) send_payload(fd, cmd, n);
    n = mc_cmd_device_query(cmd, sizeof cmd, 1);
    if (n) send_payload(fd, cmd, n);
    if (set_channel) {
        n = mc_cmd_set_channel(cmd, sizeof cmd, channel_idx, channel_name, psk);
        if (n) send_payload(fd, cmd, n);
    }
    n = mc_cmd_get_device_time(cmd, sizeof cmd);
    if (n) send_payload(fd, cmd, n);

    printf("listening on %s (Ctrl-C to quit)\n", argv[1]);

    mc_rx_t rx;
    mc_rx_init(&rx);

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 1, 0 };   /* 1 s tick so Ctrl-C is responsive */

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "select: %s\n", strerror(errno));
            break;
        }
        if (r == 0 || !FD_ISSET(fd, &rfds)) continue;

        uint8_t in[256];
        ssize_t got = read(fd, in, sizeof in);
        if (got < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            fprintf(stderr, "read: %s\n", strerror(errno));
            break;
        }
        if (got == 0) continue;

        mc_rx_feed(&rx, in, (size_t)got);

        uint8_t payload[MC_MAX_PAYLOAD];
        size_t  plen;
        while (mc_rx_poll(&rx, payload, sizeof payload, &plen)) {
            mc_event_t ev;
            if (mc_parse(payload, plen, &ev)) {
                on_event(&ev);
                /* Drain the radio's queue when it says messages are waiting. */
                if (ev.code == MC_PUSH_MSG_WAITING) {
                    n = mc_cmd_sync_next_message(cmd, sizeof cmd);
                    if (n) send_payload(fd, cmd, n);
                } else if (ev.code == MC_RESP_CHANNEL_MSG_RECV ||
                           ev.code == MC_RESP_CHANNEL_DATA_RECV ||
                           ev.code == MC_RESP_CONTACT_MSG_RECV) {
                    /* keep draining until NO_MORE_MESSAGES */
                    n = mc_cmd_sync_next_message(cmd, sizeof cmd);
                    if (n) send_payload(fd, cmd, n);
                }
            }
        }
    }

    printf("\nbye\n");
    close(fd);
    return 0;
}
