/* Host unit tests for components/voip_core. Run: make -C test/host */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "echo_ref.h"
#include "g711.h"
#include "md5.h"
#include "resample.h"
#include "rtp.h"
#include "sdp.h"
#include "sip_msg.h"
#include "sip_ua.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

static void test_g711(void)
{
    CHECK(g711_ulaw_encode(0) == 0xFF);
    CHECK(g711_alaw_encode(0) == 0xD5);
    /* Every code decodes to a value that encodes back to the same code (mu-law -0 aside). */
    for (int c = 0; c < 256; c++) {
        CHECK(g711_alaw_encode(g711_alaw_decode((uint8_t)c)) == c);
        if (c != 0x7F) {
            CHECK(g711_ulaw_encode(g711_ulaw_decode((uint8_t)c)) == c);
        }
    }
    CHECK(g711_ulaw_decode(0x00) == -32124 && g711_ulaw_decode(0x80) == 32124);
    CHECK(g711_alaw_decode(0xAA) == 32256 && g711_alaw_decode(0x2A) == -32256);
    /* Quantisation error stays within the segment step. */
    for (int v = -32768; v < 32768; v += 97) {
        int e1 = abs(g711_ulaw_decode(g711_ulaw_encode((int16_t)v)) - v);
        int e2 = abs(g711_alaw_decode(g711_alaw_encode((int16_t)v)) - v);
        CHECK(e1 <= 1100 && e2 <= 1100);
        if (abs(v) < 1000) {
            CHECK(e1 <= 32 && e2 <= 32);
        }
    }
}

static void test_md5_digest(void)
{
    char h[33];
    md5_hex(h, 1, "");
    CHECK(strcmp(h, "d41d8cd98f00b204e9800998ecf8427e") == 0);
    md5_hex(h, 1, "The quick brown fox jumps over the lazy dog");
    CHECK(strcmp(h, "9e107d9d372bb6826bd81d3542a419d6") == 0);
    md5_hex(h, 2, "12345678901234567890123456789012345678901234567890", "123456789012345678901234567890");
    CHECK(strcmp(h, "57edf4a22be3c955ac49da2e2107b67a") == 0);

    /* RFC 2617 section 3.5 example. */
    char ha1[33], ha2[33], resp[33];
    md5_hex(ha1, 5, "Mufasa", ":", "testrealm@host.com", ":", "Circle Of Life");
    md5_hex(ha2, 3, "GET", ":", "/dir/index.html");
    md5_hex(resp, 11, ha1, ":", "dcd98b7102dd2f0e8b11d0f600bfb0c093", ":", "00000001", ":", "0a4f113b", ":", "auth",
            ":", ha2);
    CHECK(strcmp(resp, "6629fae49393a05397450978507c4ef1") == 0);
}

static void test_resample(void)
{
    resample_t down, up;
    resample_init(&down);
    resample_init(&up);
    int16_t in[320], mid[160], out[320];
    double sig = 0, err = 0, alias = 0;
    for (int f = 0; f < 20; f++) {
        for (int i = 0; i < 320; i++) {
            in[i] = (int16_t)(10000 * sin(2 * M_PI * 1000 * (f * 320 + i) / 16000.0));
        }
        resample_down2(&down, in, 320, mid);
        resample_up2(&up, mid, 160, out);
        if (f > 2) {
            /* The FIRs delay by 15 + 30 samples at 16 kHz; compare against the delayed input. */
            for (int i = 0; i < 320; i++) {
                double ref = 10000 * sin(2 * M_PI * 1000 * (f * 320 + i - 45) / 16000.0);
                sig += ref * ref;
                err += (out[i] - ref) * (out[i] - ref);
            }
        }
    }
    double snr = 10 * log10(sig / err);
    printf("  resample 1 kHz round trip SNR %.1f dB\n", snr);
    CHECK(snr > 25);

    /* 6 kHz cannot be represented at 8 kHz: it must be filtered out, not aliased to 2 kHz. */
    resample_init(&down);
    for (int f = 0; f < 10; f++) {
        for (int i = 0; i < 320; i++) {
            in[i] = (int16_t)(10000 * sin(2 * M_PI * 6000 * (f * 320 + i) / 16000.0));
        }
        resample_down2(&down, in, 320, mid);
        if (f > 1) {
            for (int i = 0; i < 160; i++) {
                alias += (double)mid[i] * mid[i];
            }
        }
    }
    double alias_db = 10 * log10(alias / (8 * 160) / (10000.0 * 10000 / 2));
    printf("  resample 6 kHz rejection %.1f dB\n", alias_db);
    CHECK(alias_db < -30);
}

static void test_rtp(void)
{
    uint8_t buf[200], pl[160];
    memset(pl, 0x55, sizeof(pl));
    rtp_hdr_t h = {.pt = 8, .marker = true, .seq = 65535, .ts = 0xDEADBEEF, .ssrc = 0x01020304};
    size_t n = rtp_build(buf, sizeof(buf), &h, pl, sizeof(pl));
    CHECK(n == 172 && buf[0] == 0x80 && buf[1] == 0x88);
    rtp_hdr_t r;
    const uint8_t *p;
    size_t pn;
    CHECK(rtp_parse(buf, n, &r, &p, &pn) && r.pt == 8 && r.marker && r.seq == 65535 && r.ts == 0xDEADBEEF &&
          r.ssrc == 0x01020304 && pn == 160 && p == buf + 12);
    /* One CSRC, an extension of one word and 4 bytes of padding. */
    uint8_t x[64] = {0xB1, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 9, 0xBE, 0xDE, 0, 1, 7, 7, 7, 7, 1, 2, 3, 0, 0, 0, 4};
    CHECK(rtp_parse(x, 31, &r, &p, &pn) && pn == 3 && p[0] == 1 && p[2] == 3);
    CHECK(!rtp_parse(x, 11, &r, &p, &pn));
    x[0] = 0x40;
    CHECK(!rtp_parse(x, 31, &r, &p, &pn)); /* version 1 */
}

static const char kInvite[] =
    "INVITE sip:200@192.168.1.50:5060 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 192.168.1.10:5060;rport;branch=z9hG4bKPjabc\r\n"
    "v: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bKsecond\r\n"
    "From: \"Home Assistant\" <sip:100@192.168.1.10>;tag=from-tag-1\r\n"
    "To: <sip:200@192.168.1.10>\r\n"
    "Contact: <sip:asterisk@192.168.1.10:5060>\r\n"
    "Call-ID: abc-123@pbx\r\n"
    "CSeq: 4711 INVITE\r\n"
    "Call-Info: <sip:192.168.1.10>;answer-after=0\r\n"
    "Content-Type: application/sdp\r\n"
    "l: 181\r\n"
    "\r\n"
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.1.10\r\n"
    "s=Asterisk\r\n"
    "c=IN IP4 192.168.1.10\r\n"
    "t=0 0\r\n"
    "m=audio 12000 RTP/AVP 9 8 0 101\r\n"
    "a=rtpmap:9 G722/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "garbage after content-length";

static void test_sip_parse(void)
{
    sip_msg_t m;
    CHECK(sip_parse(kInvite, strlen(kInvite), &m));
    CHECK(m.is_request && sip_str_eq(m.method, "INVITE") && sip_str_eq(m.uri, "sip:200@192.168.1.50:5060"));
    sip_str_t v;
    CHECK(sip_get(&m, "Via", 1, &v) && sip_str_icontains(v, "second"));
    CHECK(!sip_get(&m, "Via", 2, &v));
    unsigned cseq;
    sip_str_t meth;
    CHECK(sip_cseq(&m, &cseq, &meth) && cseq == 4711 && sip_str_eq(meth, "INVITE"));
    char buf[64];
    CHECK(sip_get(&m, "From", 0, &v) && sip_param(v, "tag", buf, sizeof(buf)) && strcmp(buf, "from-tag-1") == 0);
    sip_display(v, buf, sizeof(buf));
    CHECK(strcmp(buf, "Home Assistant") == 0);
    sip_str_t uri;
    CHECK(sip_uri(v, &uri) && sip_str_eq(uri, "sip:100@192.168.1.10"));
    CHECK(sip_str_eq(sip_nameaddr(v), "\"Home Assistant\" <sip:100@192.168.1.10>"));
    CHECK(sip_get(&m, "To", 0, &v) && !sip_param(v, "tag", buf, sizeof(buf)));
    sip_display(v, buf, sizeof(buf));
    CHECK(strcmp(buf, "200") == 0);
    CHECK(m.body.len == 181);

    sdp_media_t sdp;
    CHECK(sdp_parse(m.body.s, m.body.len, &sdp));
    char ip[16];
    ip_format(sdp.ip, ip);
    CHECK(strcmp(ip, "192.168.1.10") == 0 && sdp.port == 12000 && sdp.npts == 4);
    CHECK(sdp_choose_pt(&sdp) == 8); /* G.722 is not supported; PCMA was offered before PCMU */

    const char resp[] = "SIP/2.0 401 Unauthorized\r\n"
                        "WWW-Authenticate: Digest  algorithm=MD5, realm=\"asterisk\",nonce=\"1/abc,def\", qop=\"auth\"\r\n"
                        "Content-Length: 0\r\n\r\n";
    CHECK(sip_parse(resp, strlen(resp), &m) && !m.is_request && m.status == 401);
    CHECK(sip_get(&m, "WWW-Authenticate", 0, &v));
    CHECK(sip_auth_field(v, "nonce", buf, sizeof(buf)) && strcmp(buf, "1/abc,def") == 0);
    CHECK(sip_auth_field(v, "realm", buf, sizeof(buf)) && strcmp(buf, "asterisk") == 0);
    CHECK(sip_auth_field(v, "algorithm", buf, sizeof(buf)) && strcmp(buf, "MD5") == 0);
    CHECK(!sip_auth_field(v, "opaque", buf, sizeof(buf)));

    CHECK(!sip_parse("hello", 5, &m));
    CHECK(!sip_parse("INVITE sip:x SIP/1.0\r\n\r\n", 24, &m));
    CHECK(!sip_parse("SIP/2.0 99 Weird\r\n\r\n", 20, &m));
}

/* ---------------------------------------------------------------- UA against a scripted PBX */

static char g_sent[8][SIP_BUF_SIZE];
static int g_nsent;
static sip_call_info_t g_info;
static int g_call_events;
static sip_reg_state_t g_reg;
static uint32_t g_rand = 1;

static void t_send(void *ctx, const char *buf, size_t len, uint32_t ip, uint16_t port)
{
    (void)ctx;
    (void)ip;
    (void)port;
    if (g_nsent < 8) {
        memcpy(g_sent[g_nsent], buf, len);
        g_sent[g_nsent][len] = '\0';
    }
    g_nsent++;
}
static void t_reg(void *ctx, sip_reg_state_t st, int status)
{
    (void)ctx;
    (void)status;
    g_reg = st;
}
static void t_call(void *ctx, const sip_call_info_t *info)
{
    (void)ctx;
    g_info = *info;
    g_call_events++;
}
static uint32_t t_rand(void *ctx)
{
    (void)ctx;
    return g_rand = g_rand * 1103515245u + 12345u;
}

static uint32_t ip_of(const char *s)
{
    uint32_t ip = 0;
    ip_parse(s, strlen(s), &ip);
    return ip;
}

static void feed(sip_ua_t *ua, const char *msg, uint32_t now)
{
    g_nsent = 0;
    sip_ua_on_datagram(ua, msg, strlen(msg), ip_of("192.168.1.10"), 5060, now);
}

static void test_ua_incoming(void)
{
    sip_ua_cfg_t cfg = {.domain = "192.168.1.10", .server_port = 5060, .user = "200", .password = "pw",
                        .display = "Desk", .local_port = 5060, .rtp_port = 40000, .expires = 300};
    cfg.server_ip = ip_of("192.168.1.10");
    cfg.local_ip = ip_of("192.168.1.50");
    sip_ua_ops_t ops = {.send = t_send, .reg_changed = t_reg, .call_changed = t_call, .random = t_rand};
    static sip_ua_t ua;
    sip_ua_init(&ua, &cfg, &ops, 0);

    /* From a stranger: ignored entirely. */
    g_nsent = 0;
    sip_ua_on_datagram(&ua, kInvite, strlen(kInvite), ip_of("192.168.1.99"), 5060, 0);
    CHECK(g_nsent == 0 && ua.info.state == SIP_CALL_IDLE);

    feed(&ua, kInvite, 0);
    CHECK(g_nsent == 2 && strncmp(g_sent[0], "SIP/2.0 100", 11) == 0 && strncmp(g_sent[1], "SIP/2.0 180", 11) == 0);
    CHECK(strstr(g_sent[1], "Via: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bKsecond\r\n") != NULL); /* every Via, in order */
    CHECK(strstr(g_sent[1], "To: <sip:200@192.168.1.10>;tag=") != NULL);
    CHECK(g_info.state == SIP_CALL_INCOMING && g_info.auto_answer && strcmp(g_info.peer, "Home Assistant") == 0);

    /* INVITE retransmission: 180 again, no new call event. */
    int events = g_call_events;
    feed(&ua, kInvite, 400);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "SIP/2.0 180", 11) == 0 && g_call_events == events);

    /* A second call while ringing gets 486. */
    char other[sizeof(kInvite)];
    strcpy(other, kInvite);
    memcpy(strstr(other, "abc-123"), "xyz-999", 7);
    feed(&ua, other, 500);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "SIP/2.0 486", 11) == 0 && ua.info.state == SIP_CALL_INCOMING);

    /* Answer: 200 OK with our SDP answer (PCMA chosen), retransmitted until ACK. */
    g_nsent = 0;
    CHECK(sip_ua_answer(&ua, 1000));
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "SIP/2.0 200", 11) == 0);
    CHECK(strstr(g_sent[0], "m=audio 40000 RTP/AVP 8\r\n") && strstr(g_sent[0], "c=IN IP4 192.168.1.50"));
    CHECK(strstr(g_sent[0], "Contact: <sip:200@192.168.1.50:5060>"));
    CHECK(g_info.state == SIP_CALL_ACTIVE && g_info.pt == 8 && g_info.media_port == 12000);
    g_nsent = 0;
    sip_ua_tick(&ua, 1499);
    CHECK(g_nsent == 0);
    sip_ua_tick(&ua, 1500);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "SIP/2.0 200", 11) == 0);
    feed(&ua,
         "ACK sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10;branch=z9hG4bKack\r\n"
         "From: <sip:100@192.168.1.10>;tag=from-tag-1\r\nTo: <sip:200@192.168.1.10>;tag=x\r\n"
         "Call-ID: abc-123@pbx\r\nCSeq: 4711 ACK\r\nContent-Length: 0\r\n\r\n",
         1600);
    CHECK(!ua.call_tx.active);
    g_nsent = 0;
    sip_ua_tick(&ua, 5000);
    CHECK(g_nsent == 0);

    /* OPTIONS keepalive. */
    feed(&ua,
         "OPTIONS sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10;branch=z9hG4bKopt\r\n"
         "From: <sip:asterisk@192.168.1.10>;tag=o1\r\nTo: <sip:200@192.168.1.50>\r\nCall-ID: opt-1\r\n"
         "CSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n",
         6000);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "SIP/2.0 200", 11) == 0 && strstr(g_sent[0], "Call-ID: opt-1"));

    /* We hang up: BYE to the remote Contact with both tags, retransmitted until 200. */
    g_nsent = 0;
    sip_ua_hangup(&ua, 7000);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "BYE sip:asterisk@192.168.1.10:5060 SIP/2.0", 42) == 0);
    CHECK(strstr(g_sent[0], "From: <sip:200@192.168.1.10>;tag=") && strstr(g_sent[0], "To: \"Home Assistant\" <sip:100@192.168.1.10>;tag=from-tag-1"));
    CHECK(g_info.state == SIP_CALL_IDLE && g_info.reason == SIP_END_LOCAL);
    char bye_ok[1024];
    char *cs = strstr(g_sent[0], "CSeq: ");
    unsigned bye_cseq = (unsigned)atoi(cs + 6);
    snprintf(bye_ok, sizeof(bye_ok),
             "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@192.168.1.10>;tag=a\r\n"
             "To: <sip:100@192.168.1.10>;tag=from-tag-1\r\nCall-ID: abc-123@pbx\r\nCSeq: %u BYE\r\n"
             "Content-Length: 0\r\n\r\n",
             bye_cseq);
    feed(&ua, bye_ok, 7100);
    CHECK(!ua.aux_tx.active);

    /* A late retransmission of the finished INVITE does not ring again. */
    feed(&ua, kInvite, 8000);
    CHECK(g_nsent == 0 && ua.info.state == SIP_CALL_IDLE);
}

static void test_ua_cancel_and_outgoing(void)
{
    sip_ua_cfg_t cfg = {.domain = "pbx.lan", .server_port = 5060, .user = "200", .password = "secret",
                        .local_port = 5060, .rtp_port = 40000, .expires = 120};
    cfg.server_ip = ip_of("192.168.1.10");
    cfg.local_ip = ip_of("192.168.1.50");
    sip_ua_ops_t ops = {.send = t_send, .reg_changed = t_reg, .call_changed = t_call, .random = t_rand};
    static sip_ua_t ua;
    sip_ua_init(&ua, &cfg, &ops, 0);

    /* Incoming call cancelled by the caller: missed call, 200 to CANCEL then 487. */
    feed(&ua, kInvite, 0);
    feed(&ua,
         "CANCEL sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10:5060;rport;branch=z9hG4bKPjabc\r\n"
         "From: \"Home Assistant\" <sip:100@192.168.1.10>;tag=from-tag-1\r\nTo: <sip:200@192.168.1.10>\r\n"
         "Call-ID: abc-123@pbx\r\nCSeq: 4711 CANCEL\r\nContent-Length: 0\r\n\r\n",
         2000);
    CHECK(g_nsent == 2 && strncmp(g_sent[0], "SIP/2.0 200", 11) == 0 && strncmp(g_sent[1], "SIP/2.0 487", 11) == 0);
    CHECK(g_info.state == SIP_CALL_IDLE && g_info.reason == SIP_END_MISSED);

    /* Outgoing call while the 487 still waits for its ACK: refused. */
    CHECK(!sip_ua_call(&ua, "100", 2100));
    sip_ua_tick(&ua, 2000 + 33000); /* ACK never came: transaction times out */
    CHECK(!ua.call_tx.active);

    /* Outgoing call: INVITE, 401 challenge, ACK + authenticated INVITE, 180, 200, ACK. */
    g_nsent = 0;
    CHECK(sip_ua_call(&ua, "100", 40000));
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "INVITE sip:100@pbx.lan SIP/2.0", 30) == 0);
    CHECK(strstr(g_sent[0], "m=audio 40000 RTP/AVP 0 8"));
    CHECK(g_info.state == SIP_CALL_OUTGOING);
    char callid[100];
    sip_msg_t m;
    sip_parse(g_sent[0], strlen(g_sent[0]), &m);
    sip_str_t v;
    sip_get(&m, "Call-ID", 0, &v);
    sip_str_copy(v, callid, sizeof(callid));

    char resp[1500];
    snprintf(resp, sizeof(resp),
             "SIP/2.0 401 Unauthorized\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@pbx.lan>;tag=a\r\n"
             "To: <sip:100@pbx.lan>;tag=t401\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\n"
             "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"n1\", algorithm=MD5, qop=\"auth\"\r\n"
             "Content-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 40100);
    CHECK(g_nsent == 2 && strncmp(g_sent[0], "ACK sip:100@pbx.lan", 19) == 0 && strstr(g_sent[0], "tag=t401"));
    CHECK(strncmp(g_sent[1], "INVITE", 6) == 0 && strstr(g_sent[1], "CSeq: 2 INVITE") &&
          strstr(g_sent[1], "Authorization: Digest username=\"200\", realm=\"asterisk\", nonce=\"n1\", uri=\"sip:100@pbx.lan\""));

    snprintf(resp, sizeof(resp),
             "SIP/2.0 180 Ringing\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@pbx.lan>;tag=a\r\n"
             "To: <sip:100@pbx.lan>;tag=rt\r\nCall-ID: %s\r\nCSeq: 2 INVITE\r\nContent-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 40200);
    CHECK(g_info.state == SIP_CALL_RINGBACK);
    g_nsent = 0;
    sip_ua_tick(&ua, 45000);
    CHECK(g_nsent == 0); /* no INVITE retransmission after a provisional */

    const char *sdp = "v=0\r\no=- 2 2 IN IP4 192.168.1.10\r\ns=-\r\nc=IN IP4 192.168.1.10\r\nt=0 0\r\n"
                      "m=audio 14000 RTP/AVP 0\r\n";
    snprintf(resp, sizeof(resp),
             "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@pbx.lan>;tag=a\r\n"
             "To: <sip:100@pbx.lan>;tag=rt\r\nCall-ID: %s\r\nCSeq: 2 INVITE\r\nContact: <sip:100@192.168.1.10:5060>\r\n"
             "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
             callid, (unsigned)strlen(sdp), sdp);
    feed(&ua, resp, 46000);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "ACK sip:100@192.168.1.10:5060 SIP/2.0", 37) == 0 &&
          strstr(g_sent[0], "CSeq: 2 ACK"));
    CHECK(g_info.state == SIP_CALL_ACTIVE && g_info.pt == 0 && g_info.media_port == 14000);
    feed(&ua, resp, 46500); /* 200 retransmitted: ACK again */
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "ACK", 3) == 0);

    /* Remote hangs up. */
    snprintf(resp, sizeof(resp),
             "BYE sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10;branch=z9hG4bKbye\r\n"
             "From: <sip:100@pbx.lan>;tag=rt\r\nTo: <sip:200@pbx.lan>;tag=a\r\nCall-ID: %s\r\nCSeq: 7 BYE\r\n"
             "Content-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 50000);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "SIP/2.0 200", 11) == 0);
    CHECK(g_info.state == SIP_CALL_IDLE && g_info.reason == SIP_END_REMOTE);

    /* Call, then cancel before any provisional: CANCEL waits for the 100. */
    g_nsent = 0;
    CHECK(sip_ua_call(&ua, "100", 60000));
    sip_parse(g_sent[0], strlen(g_sent[0]), &m);
    sip_get(&m, "Call-ID", 0, &v);
    sip_str_copy(v, callid, sizeof(callid));
    g_nsent = 0;
    sip_ua_hangup(&ua, 60100);
    CHECK(g_nsent == 0 && g_info.state == SIP_CALL_IDLE);
    snprintf(resp, sizeof(resp),
             "SIP/2.0 100 Trying\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@pbx.lan>;tag=a\r\n"
             "To: <sip:100@pbx.lan>\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 60200);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "CANCEL sip:100@pbx.lan SIP/2.0", 30) == 0 && strstr(g_sent[0], "CSeq: 1 CANCEL"));
    snprintf(resp, sizeof(resp),
             "SIP/2.0 487 Request Terminated\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@pbx.lan>;tag=a\r\n"
             "To: <sip:100@pbx.lan>;tag=z\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 60300);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "ACK", 3) == 0 && !ua.call_tx.active);

    /* Busy callee. */
    g_nsent = 0;
    CHECK(sip_ua_call(&ua, "300", 70000));
    sip_parse(g_sent[0], strlen(g_sent[0]), &m);
    sip_get(&m, "Call-ID", 0, &v);
    sip_str_copy(v, callid, sizeof(callid));
    snprintf(resp, sizeof(resp),
             "SIP/2.0 486 Busy Here\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@pbx.lan>;tag=a\r\n"
             "To: <sip:300@pbx.lan>;tag=b\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 70100);
    CHECK(g_info.state == SIP_CALL_IDLE && g_info.reason == SIP_END_BUSY && g_info.status == 486);

    /* No answer from the PBX at all: retransmit, then give up after 32 s. */
    g_nsent = 0;
    CHECK(sip_ua_call(&ua, "100", 80000));
    g_nsent = 0;
    for (uint32_t t = 80000; t <= 80000 + 33000; t += 100) {
        sip_ua_tick(&ua, t);
    }
    CHECK(g_nsent >= 6 && g_info.state == SIP_CALL_IDLE && g_info.reason == SIP_END_UNREACHABLE);
}

static void test_register(void)
{
    sip_ua_cfg_t cfg = {.domain = "192.168.1.10", .server_port = 5060, .user = "200", .password = "pw",
                        .local_port = 5060, .rtp_port = 40000, .expires = 300};
    cfg.server_ip = ip_of("192.168.1.10");
    cfg.local_ip = ip_of("192.168.1.50");
    sip_ua_ops_t ops = {.send = t_send, .reg_changed = t_reg, .call_changed = t_call, .random = t_rand};
    static sip_ua_t ua;
    sip_ua_init(&ua, &cfg, &ops, 0);
    g_nsent = 0;
    sip_ua_register(&ua, 0);
    CHECK(g_nsent == 1 && strncmp(g_sent[0], "REGISTER sip:192.168.1.10 SIP/2.0", 33) == 0 && g_reg == SIP_REG_TRYING);
    sip_msg_t m;
    sip_parse(g_sent[0], strlen(g_sent[0]), &m);
    sip_str_t v;
    char callid[64];
    sip_get(&m, "Call-ID", 0, &v);
    sip_str_copy(v, callid, sizeof(callid));
    char resp[800];
    snprintf(resp, sizeof(resp),
             "SIP/2.0 401 Unauthorized\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@x>;tag=a\r\n"
             "To: <sip:200@x>;tag=b\r\nCall-ID: %s\r\nCSeq: 1 REGISTER\r\n"
             "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"abc\"\r\nContent-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 100);
    CHECK(g_nsent == 1 && strstr(g_sent[0], "CSeq: 2 REGISTER") && strstr(g_sent[0], "Authorization: Digest"));
    CHECK(!strstr(g_sent[0], "qop=")); /* challenge had no qop */
    snprintf(resp, sizeof(resp),
             "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@x>;tag=a\r\n"
             "To: <sip:200@x>;tag=b\r\nCall-ID: %s\r\nCSeq: 2 REGISTER\r\n"
             "Contact: <sip:200@192.168.1.50:5060>;expires=60\r\nContent-Length: 0\r\n\r\n",
             callid);
    feed(&ua, resp, 200);
    CHECK(g_reg == SIP_REG_OK);
    g_nsent = 0;
    sip_ua_tick(&ua, 200 + 47000);
    CHECK(g_nsent == 0);
    sip_ua_tick(&ua, 200 + 48000); /* refresh at 80 % of 60 s */
    CHECK(g_nsent == 1 && strstr(g_sent[0], "CSeq: 3 REGISTER"));
    /* Wrong password: two challenges in a row -> failed, retry later. */
    snprintf(resp, sizeof(resp),
             "SIP/2.0 401 Unauthorized\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@x>;tag=a\r\n"
             "To: <sip:200@x>;tag=b\r\nCall-ID: %s\r\nCSeq: %%u REGISTER\r\n"
             "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"abc\"\r\nContent-Length: 0\r\n\r\n",
             callid);
    char r2[800];
    for (unsigned cseq = 3; cseq <= 5; cseq++) {
        snprintf(r2, sizeof(r2), resp, cseq);
        feed(&ua, r2, 49000 + cseq);
    }
    CHECK(g_reg == SIP_REG_FAILED);
}

static void test_echo_ref(void)
{
    static int16_t ring[4096];
    echo_ref_t r;
    echo_ref_init(&r, ring, 4096);
    int16_t chunk[100], out[100];
    for (int i = 0; i < 100; i++) {
        chunk[i] = (int16_t)i;
    }
    for (int k = 0; k < 50; k++) {
        echo_ref_push(&r, chunk, 100); /* 5000 samples: the first 904 fell out */
    }
    echo_ref_read(&r, 4950, out, 100);
    CHECK(out[0] == 50 && out[49] == 99 && out[50] == 0); /* beyond what was written: zeros */
    echo_ref_read(&r, 100, out, 10);
    CHECK(out[0] == 0 && out[9] == 0); /* overwritten: zeros */
    echo_ref_read(&r, 1000, out, 1);
    CHECK(out[0] == 0); /* sample 1000 = chunk[0] */

    /* A chirp played into a room: mic = 0.3 * delayed + reflection + noise. Find the delay. */
    enum { XN = 9600, YN = 4000, DELAY = 2345 };
    static int16_t x[XN], y[YN];
    unsigned seed = 7;
    for (int i = 0; i < XN; i++) {
        double t = i / 16000.0;
        x[i] = (int16_t)(12000 * sin(2 * M_PI * (300 * t + 2500 * t * t)));
    }
    for (int i = 0; i < YN; i++) {
        seed = seed * 1103515245u + 12345u;
        double noise = ((int)(seed >> 16) % 2000) - 1000;
        y[i] = (int16_t)(0.3 * x[i + DELAY] + 0.1 * x[i + DELAY - 40] + noise);
    }
    float conf;
    int d = delay_find(x, XN, y, YN, &conf);
    printf("  delay found %d (expected %d), confidence %.2f\n", d, DELAY, conf);
    CHECK(d == DELAY && conf > 0.6f);

    /* Pure noise: low confidence. */
    for (int i = 0; i < YN; i++) {
        seed = seed * 1103515245u + 12345u;
        y[i] = (int16_t)(((int)(seed >> 16) % 2000) - 1000);
    }
    d = delay_find(x, XN, y, YN, &conf);
    CHECK(conf < 0.3f);
    CHECK(delay_find(x, 10, y, YN, &conf) == -1);
}

int main(void)
{
    test_g711();
    test_md5_digest();
    test_resample();
    test_rtp();
    test_sip_parse();
    test_register();
    test_ua_incoming();
    test_ua_cancel_and_outgoing();
    test_echo_ref();
    printf("voip: %d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
