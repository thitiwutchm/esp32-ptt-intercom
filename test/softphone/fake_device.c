/*
 * The device's SIP stack (components/voip_core) on the host, pointed at
 * tools/sip_phone.py as its PBX, for run.sh. Echoes the audio it receives.
 *
 *   fake_device answer <pbx_port> <local_port>  wait for a call, answer, run until the caller hangs up
 *   fake_device call <pbx_port> <local_port>    call extension 100, talk 3 s, hang up
 *
 * Exit status 0 when the call happened as expected with audio both ways.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rtp.h"
#include "sip_ua.h"

#define RTP_PORT 40002

static int sip_sock, rtp_sock;
static sip_ua_t ua;
static sip_call_info_t info;
static sip_reg_state_t reg;
static int rx_frames, tx_frames;
static bool was_active;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t loopback(void)
{
    uint32_t ip;
    inet_pton(AF_INET, "127.0.0.1", &ip);
    return ip;
}

static void op_send(void *ctx, const char *buf, size_t len, uint32_t ip, uint16_t port)
{
    (void)ctx;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
    sendto(sip_sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static void op_reg(void *ctx, sip_reg_state_t st, int status)
{
    (void)ctx;
    reg = st;
    printf("[device] registration %d (status %d)\n", st, status);
}

static void op_call(void *ctx, const sip_call_info_t *i)
{
    (void)ctx;
    info = *i;
    was_active |= i->state == SIP_CALL_ACTIVE;
    printf("[device] call %s peer=\"%s\" reason=%s pt=%d\n", sip_call_state_name(i->state), i->peer,
           sip_end_reason_name(i->reason), i->pt);
}

static uint32_t op_rand(void *ctx)
{
    (void)ctx;
    return (uint32_t)random();
}

static int udp_bind(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = loopback()};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("bind");
        exit(2);
    }
    return s;
}

/* Run the stack for `ms`; send back every audio frame received during a call. */
static void pump(uint32_t ms)
{
    uint32_t end = now_ms() + ms;
    while ((int32_t)(end - now_ms()) > 0) {
        struct pollfd fds[2] = {{.fd = sip_sock, .events = POLLIN}, {.fd = rtp_sock, .events = POLLIN}};
        poll(fds, 2, 5);
        char buf[4096];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        if (fds[0].revents & POLLIN) {
            ssize_t n = recvfrom(sip_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                sip_ua_on_datagram(&ua, buf, (size_t)n, from.sin_addr.s_addr, ntohs(from.sin_port), now_ms());
            }
        }
        if (fds[1].revents & POLLIN) {
            fl = sizeof(from);
            ssize_t n = recvfrom(rtp_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            rtp_hdr_t h;
            const uint8_t *pl;
            size_t pn;
            if (n > 0 && rtp_parse((uint8_t *)buf, (size_t)n, &h, &pl, &pn) && h.pt == info.pt) {
                rx_frames++;
                if (info.state == SIP_CALL_ACTIVE) {
                    static uint16_t seq;
                    static uint32_t ts;
                    uint8_t pkt[1500];
                    rtp_hdr_t o = {.pt = (uint8_t)info.pt, .seq = seq++, .ts = ts, .ssrc = 0xdecafbad};
                    ts += (uint32_t)pn;
                    size_t m = rtp_build(pkt, sizeof(pkt), &o, pl, pn);
                    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(info.media_port),
                                             .sin_addr.s_addr = info.media_ip};
                    sendto(rtp_sock, pkt, m, 0, (struct sockaddr *)&to, sizeof(to));
                    tx_frames++;
                }
            }
        }
        sip_ua_tick(&ua, now_ms());
    }
}

static bool wait_for(sip_call_state_t st, uint32_t timeout_ms)
{
    uint32_t end = now_ms() + timeout_ms;
    while (info.state != st && (int32_t)(end - now_ms()) > 0) {
        pump(10);
    }
    return info.state == st;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s answer|call <pbx_port> <local_port>\n", argv[0]);
        return 2;
    }
    bool calling = strcmp(argv[1], "call") == 0;
    srandom((unsigned)time(NULL));
    uint16_t local_port = (uint16_t)atoi(argv[3]);
    sip_sock = udp_bind(local_port);
    rtp_sock = udp_bind(RTP_PORT);

    sip_ua_cfg_t cfg = {.domain = "127.0.0.1", .server_port = (uint16_t)atoi(argv[2]), .user = "200",
                        .password = "unused", .display = "Desk CUBE", .local_port = local_port,
                        .rtp_port = RTP_PORT, .expires = 120};
    cfg.server_ip = loopback();
    cfg.local_ip = loopback();
    sip_ua_ops_t ops = {.send = op_send, .reg_changed = op_reg, .call_changed = op_call, .random = op_rand};
    sip_ua_init(&ua, &cfg, &ops, now_ms());
    sip_ua_register(&ua, now_ms());
    uint32_t end = now_ms() + 5000;
    while (reg != SIP_REG_OK && (int32_t)(end - now_ms()) > 0) {
        pump(20);
    }
    if (reg != SIP_REG_OK) {
        printf("[device] FAIL: not registered\n");
        return 1;
    }

    bool ok;
    if (calling) {
        sip_ua_call(&ua, "100", now_ms());
        ok = wait_for(SIP_CALL_ACTIVE, 5000);
        pump(3000);
        sip_ua_hangup(&ua, now_ms());
        pump(500);
        ok = ok && info.state == SIP_CALL_IDLE && info.reason == SIP_END_LOCAL && !ua.aux_tx.active;
    } else {
        ok = wait_for(SIP_CALL_INCOMING, 5000);
        pump(300);
        ok = ok && sip_ua_answer(&ua, now_ms());
        pump(1000);
        ok = ok && !ua.call_tx.active; /* our 200 was ACKed */
        ok = ok && wait_for(SIP_CALL_IDLE, 15000) && info.reason == SIP_END_REMOTE;
    }
    printf("[device] %d frames in, %d echoed\n", rx_frames, tx_frames);
    ok = ok && was_active && rx_frames > 50 && tx_frames > 50;
    printf("[device] %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
