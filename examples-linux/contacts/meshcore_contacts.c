/*
 * meshcore_contacts.c
 *
 * One-shot: open a tty, ask the companion radio for its contact list, print
 * each contact (name, key prefix, type, path, location, last-seen), then exit.
 * Demonstrates the Phase 2 contact API: GET_CONTACTS -> CONTACTS_START, a stream
 * of CONTACT records, then END_OF_CONTACTS.
 *
 *   build:  via CMakeLists.txt at the repo root, or:
 *           cc -std=c99 -Wall -Wextra -I../../src \
 *              meshcore_contacts.c ../../src/meshcore_companion.c -o meshcore_contacts
 *
 *   run:    ./meshcore_contacts /dev/ttyACM0
 *
 * SPDX-License-Identifier: MIT
 * Author: Scott Penrose / Digital Dimensions.
 */
#define _DEFAULT_SOURCE

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

static int tty_open(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { close(fd); return -1; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200); cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void send_payload(int fd, const uint8_t *payload, size_t len) {
    uint8_t frame[MC_RX_BUFSZ];
    size_t flen = mc_frame_encode(payload, len, frame, sizeof frame), off = 0;
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

static int    g_expected = -1;   /* CONTACTS_START count, or -1 if not seen */
static int    g_got = 0;
static int    g_done = 0;

static void print_contact(const mc_contact_t *c) {
    printf("  %-20s ", c->adv_name[0] ? c->adv_name : "(unnamed)");
    for (int i = 0; i < 6; i++) printf("%02x", c->public_key[i]);
    printf("  type=%u", c->type);
    if (c->out_path_len == 0xFF) printf("  path=direct");
    else                         printf("  path=%u hop(s)", c->out_path_len);
    if (c->adv_lat || c->adv_lon)
        printf("  loc=%.5f,%.5f", c->adv_lat / 1e6, c->adv_lon / 1e6);
    printf("\n");
}

static void handle(const mc_event_t *ev) {
    switch (ev->code) {
    case MC_RESP_CONTACTS_START:
        g_expected = (int)ev->u.contacts_count;
        printf("Contacts (%d)\n", g_expected);
        break;
    case MC_RESP_CONTACT:
        g_got++;
        print_contact(&ev->u.contact);
        break;
    case MC_RESP_END_OF_CONTACTS:
        g_done = 1;
        break;
    default: break;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <tty>\n", argv[0]); return 2; }
    int fd = tty_open(argv[1]);
    if (fd < 0) return 1;

    mc_rx_t rx; mc_rx_init(&rx);
    uint8_t cmd[MC_MAX_PAYLOAD]; size_t n;
    n = mc_cmd_app_start(cmd, sizeof cmd, "meshcore_contacts"); if (n) send_payload(fd, cmd, n);
    n = mc_cmd_device_query(cmd, sizeof cmd, 1);                if (n) send_payload(fd, cmd, n);
    n = mc_cmd_get_contacts(cmd, sizeof cmd, 0);                if (n) send_payload(fd, cmd, n);

    long deadline = now_ms() + 4000;
    while (!g_done && now_ms() < deadline) {
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        long rem = deadline - now_ms();
        struct timeval tv = { rem / 1000, (rem % 1000) * 1000 };
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        uint8_t in[256];
        ssize_t got = read(fd, in, sizeof in);
        if (got <= 0) { if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue; break; }
        mc_rx_feed(&rx, in, (size_t)got);
        uint8_t pl[MC_MAX_PAYLOAD]; size_t pn;
        while (mc_rx_poll(&rx, pl, sizeof pl, &pn)) {
            mc_event_t ev;
            if (mc_parse(pl, pn, &ev)) handle(&ev);
        }
    }

    if (g_expected < 0) printf("No contact list received (is the radio connected?).\n");
    else if (!g_done)   printf("(timed out after %d of %d contacts)\n", g_got, g_expected);
    close(fd);
    return g_expected >= 0 ? 0 : 1;
}
