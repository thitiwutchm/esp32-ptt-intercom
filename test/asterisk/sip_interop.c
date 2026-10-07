/*
 * Runs components/voip_core against a real Asterisk on 127.0.0.1:5070
 * (config in test/asterisk/etc, started by run.sh). Exercises what the
 * desk device does: register, call out, receive calls, RTP both ways.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "g711.h"
#include "rtp.h"
#include "sip_ua.h"

#define SIP_PORT 5080
#define RTP_PORT 40000

static int sip_sock, rtp_sock;
static sip_ua_t ua;
static sip_call_info_t info;
static int info_events;
static sip_reg_state_t reg;
static const char *asterisk_conf;
static int failures;

/* media */
static double tone_hz = 0; /* what we send, 0 = silence */
static uint16_t tx_seq;
static uint32_t tx_ts, tx_ssrc = 0x12345678;
static uint32_t rx_frames;
static double rx_energy, rx_tone_energy;
static double rx_tone_hz = 1000;
static uint32_t phase;

#define CHECK(c, what)                                         \
    do {                                                       \
        if (c) {                                               \
            printf("  ok    %s\n", what);                      \
        } else {                                               \
            failures++;                                        \
            printf("  FAIL  %s (%s:%d)\n", what, __FILE__, __LINE__); \
        }                                                      \
    } while (0)

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
    if (getenv("SIP_TRACE")) {
        printf(">>>>\n%.*s\n", (int)len, buf);
    }
}

static void op_reg(void *ctx, sip_reg_state_t st, int status)
{
    (void)ctx;
    reg = st;
    printf("  .. registration %d (status %d)\n", st, status);
}

static void op_call(void *ctx, const sip_call_info_t *i)
{
    (void)ctx;
    info = *i;
    info_events++;
    printf("  .. call %s peer=\"%s\" auto=%d reason=%s status=%d pt=%d\n", sip_call_state_name(i->state), i->peer,
           i->auto_answer, sip_end_reason_name(i->reason), i->status, i->pt);
    if (i->state == SIP_CALL_ACTIVE) {
        rx_frames = 0;
        rx_energy = rx_tone_energy = 0;
    }
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
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("bind");
        exit(2);
    }
    return s;
}

/* Goertzel power of `hz` in an 8 kHz block, relative to the block energy. */
static void analyse(const int16_t *pcm, int n)
{
    double w = 2 * M_PI * rx_tone_hz / 8000, c = 2 * cos(w), s1 = 0, s2 = 0, e = 0;
    for (int i = 0; i < n; i++) {
        double s0 = pcm[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        e += (double)pcm[i] * pcm[i];
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2; /* = (A n / 2)^2 for a pure tone */
    rx_energy += e;
    rx_tone_energy += 2 * p / n;
}

static void send_rtp_frame(void)
{
    int16_t pcm[160];
    for (int i = 0; i < 160; i++, phase++) {
        pcm[i] = tone_hz > 0 ? (int16_t)(8000 * sin(2 * M_PI * tone_hz * phase / 8000)) : 0;
    }
    uint8_t payload[160], pkt[200];
    g711_encode(info.pt, pcm, payload, 160);
    rtp_hdr_t h = {.pt = (uint8_t)info.pt, .seq = tx_seq++, .ts = tx_ts, .ssrc = tx_ssrc};
    tx_ts += 160;
    size_t n = rtp_build(pkt, sizeof(pkt), &h, payload, 160);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(info.media_port), .sin_addr.s_addr = info.media_ip};
    sendto(rtp_sock, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
}

static void pump(uint32_t ms)
{
    uint32_t end = now_ms() + ms;
    static uint32_t next_rtp;
    while ((int32_t)(end - now_ms()) > 0) {
        struct pollfd fds[2] = {{.fd = sip_sock, .events = POLLIN}, {.fd = rtp_sock, .events = POLLIN}};
        poll(fds, 2, 5);
        char buf[4096];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        if (fds[0].revents & POLLIN) {
            ssize_t n = recvfrom(sip_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                if (getenv("SIP_TRACE")) {
                    printf("<<<<\n%.*s\n", (int)n, buf);
                }
                sip_ua_on_datagram(&ua, buf, (size_t)n, from.sin_addr.s_addr, ntohs(from.sin_port), now_ms());
            }
        }
        if (fds[1].revents & POLLIN) {
            ssize_t n = recvfrom(rtp_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            rtp_hdr_t h;
            const uint8_t *pl;
            size_t pn;
            if (n > 0 && rtp_parse((uint8_t *)buf, (size_t)n, &h, &pl, &pn) && (h.pt == 0 || h.pt == 8) && pn <= 320) {
                int16_t pcm[320];
                g711_decode(h.pt, pl, pcm, (int)pn);
                analyse(pcm, (int)pn);
                rx_frames++;
            }
        }
        uint32_t t = now_ms();
        sip_ua_tick(&ua, t);
        if (info.state == SIP_CALL_ACTIVE && (int32_t)(t - next_rtp) >= 0) {
            send_rtp_frame();
            next_rtp = t + 20;
        }
    }
}

static bool wait_state(sip_call_state_t st, uint32_t timeout_ms)
{
    uint32_t end = now_ms() + timeout_ms;
    while (info.state != st && (int32_t)(end - now_ms()) > 0) {
        pump(10);
    }
    return info.state == st;
}

static void ast(const char *cmd)
{
    char line[512];
    snprintf(line, sizeof(line), "asterisk -C %s -rx \"%s\" >/dev/null 2>&1", asterisk_conf, cmd);
    if (system(line) != 0) {
        printf("  !! asterisk command failed: %s\n", cmd);
    }
}

static double tone_ratio(void)
{
    return rx_energy > 0 ? rx_tone_energy / rx_energy : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <asterisk.conf>\n", argv[0]);
        return 2;
    }
    asterisk_conf = argv[1];
    srandom((unsigned)time(NULL));
    sip_sock = udp_bind(SIP_PORT);
    rtp_sock = udp_bind(RTP_PORT);

    sip_ua_cfg_t cfg = {.domain = "127.0.0.1", .server_port = 5070, .user = "200", .password = "secret200",
                        .display = "Desk CUBE", .local_port = SIP_PORT, .rtp_port = RTP_PORT, .expires = 120};
    cfg.server_ip = loopback();
    cfg.local_ip = loopback();
    sip_ua_ops_t ops = {.send = op_send, .reg_changed = op_reg, .call_changed = op_call, .random = op_rand};
    sip_ua_init(&ua, &cfg, &ops, now_ms());

    printf("register\n");
    sip_ua_register(&ua, now_ms());
    uint32_t end = now_ms() + 5000;
    while (reg != SIP_REG_OK && (int32_t)(end - now_ms()) > 0) {
        pump(20);
    }
    CHECK(reg == SIP_REG_OK, "REGISTER with digest auth accepted");
    pump(1500); /* let Asterisk qualify us with OPTIONS */

    printf("call 600 (echo)\n");
    tone_hz = 1000;
    rx_tone_hz = 1000;
    CHECK(sip_ua_call(&ua, "600", now_ms()), "INVITE sent");
    CHECK(wait_state(SIP_CALL_ACTIVE, 5000), "answered after INVITE auth challenge");
    pump(2500);
    printf("  .. %u frames back, %.0f%% of the energy at 1 kHz\n", rx_frames, 100 * tone_ratio());
    CHECK(rx_frames > 80 && tone_ratio() > 0.8, "our 1 kHz tone comes back through the PBX");
    sip_ua_hangup(&ua, now_ms());
    pump(500);
    CHECK(info.state == SIP_CALL_IDLE && info.reason == SIP_END_LOCAL && !ua.aux_tx.active, "BYE answered");

    printf("call 602 (busy)\n");
    tone_hz = 0;
    sip_ua_call(&ua, "602", now_ms());
    wait_state(SIP_CALL_IDLE, 8000);
    CHECK(info.state == SIP_CALL_IDLE && info.reason == SIP_END_BUSY, "busy reported");
    pump(300);

    printf("call 601 (recording, remote hangs up)\n");
    sip_ua_call(&ua, "601", now_ms());
    CHECK(wait_state(SIP_CALL_ACTIVE, 5000), "answered");
    CHECK(wait_state(SIP_CALL_IDLE, 20000) && info.reason == SIP_END_REMOTE, "remote BYE handled");
    printf("  .. %u frames received\n", rx_frames);
    CHECK(rx_frames > 30 && rx_energy > 0, "recording received");
    pump(300);

    printf("incoming call with auto-answer header\n");
    ast("channel originate Local/200@internal extension s@tone");
    CHECK(wait_state(SIP_CALL_INCOMING, 5000), "INVITE rings here");
    CHECK(info.auto_answer && strcmp(info.peer, "Home Assistant") == 0, "caller name and auto-answer request seen");
    pump(800);
    rx_tone_hz = 1000;
    CHECK(sip_ua_answer(&ua, now_ms()) && info.state == SIP_CALL_ACTIVE, "answered");
    CHECK(wait_state(SIP_CALL_IDLE, 10000) && info.reason == SIP_END_REMOTE, "caller hung up after its tone");
    printf("  .. %u frames, %.0f%% at 1 kHz\n", rx_frames, 100 * tone_ratio());
    CHECK(rx_frames > 60 && tone_ratio() > 0.8, "caller's 1 kHz tone received");
    CHECK(!ua.call_tx.active, "our 200 OK was ACKed");
    pump(300);

    printf("incoming call cancelled by the caller\n");
    ast("channel originate Local/200@internal extension s@tone");
    CHECK(wait_state(SIP_CALL_INCOMING, 5000), "rings");
    pump(1000);
    ast("channel request hangup all");
    CHECK(wait_state(SIP_CALL_IDLE, 5000) && info.reason == SIP_END_MISSED, "CANCEL -> missed call");
    pump(1000);
    CHECK(!ua.call_tx.active, "487 was ACKed");

    printf("incoming call rejected here\n");
    ast("channel originate Local/200@internal extension s@tone");
    CHECK(wait_state(SIP_CALL_INCOMING, 5000), "rings");
    sip_ua_hangup(&ua, now_ms());
    pump(1000);
    CHECK(info.state == SIP_CALL_IDLE && !ua.call_tx.active, "486 sent and ACKed");

    printf("hang up an outgoing call straight away\n");
    /* Depending on timing this is a CANCEL, or ACK + BYE if the echo test already answered. */
    sip_ua_call(&ua, "600", now_ms());
    pump(30);
    sip_ua_hangup(&ua, now_ms());
    pump(2000);
    CHECK(info.state == SIP_CALL_IDLE && !ua.call_tx.active && !ua.aux_tx.active,
          "cancelled call cleaned up (CANCEL or ACK+BYE)");

    printf("still registered: %s\n", reg == SIP_REG_OK ? "yes" : "no");
    CHECK(reg == SIP_REG_OK, "registration kept");
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
