/* Host unit tests for ptt_core. Run: make -C test/host */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptt_adpcm.h"
#include "ptt_floor.h"
#include "ptt_jitter.h"
#include "ptt_peers.h"
#include "ptt_proto.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;
static int checks;

#define CHECK(cond)                                                   \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                             \
    } while (0)

/* ---------------------------------------------------------------- proto */

static void test_proto_roundtrip(void)
{
    uint8_t buf[PTT_MAX_PACKET];
    uint8_t payload[5] = {1, 2, 3, 4, 5};
    ptt_hdr_t h = {
        .type = PTT_MSG_AUDIO, .device_id = 0xA1B2C3D4, .talk_id = 0x01020304,
        .seq = 65535, .channel = 7, .codec = PTT_CODEC_IMA_ADPCM, .payload_len = sizeof(payload),
    };
    size_t n = ptt_proto_build(buf, sizeof(buf), &h, payload);
    CHECK(n == PTT_HDR_LEN + 5);
    /* Wire format is little endian and fixed: guard against accidental layout changes. */
    CHECK(buf[0] == 'P' && buf[1] == 'T' && buf[2] == 1 && buf[3] == PTT_MSG_AUDIO);
    CHECK(buf[4] == 0xD4 && buf[7] == 0xA1);
    CHECK(buf[12] == 0xFF && buf[13] == 0xFF && buf[14] == 7);

    ptt_hdr_t r;
    const uint8_t *pl;
    CHECK(ptt_proto_parse(buf, n, &r, &pl));
    CHECK(r.type == h.type && r.device_id == h.device_id && r.talk_id == h.talk_id);
    CHECK(r.seq == h.seq && r.channel == h.channel && r.codec == h.codec);
    CHECK(r.payload_len == 5 && memcmp(pl, payload, 5) == 0);
}

static void test_proto_rejects_bad_input(void)
{
    uint8_t buf[PTT_MAX_PACKET];
    ptt_hdr_t h = {.type = PTT_MSG_HELLO, .device_id = 1, .channel = 1, .payload_len = 4};
    uint8_t pl[4] = {0};
    size_t n = ptt_proto_build(buf, sizeof(buf), &h, pl);
    ptt_hdr_t r;
    const uint8_t *p;

    CHECK(!ptt_proto_parse(buf, PTT_HDR_LEN - 1, &r, &p)); /* short */
    CHECK(!ptt_proto_parse(buf, n - 1, &r, &p));           /* truncated payload */
    buf[0] = 'X';
    CHECK(!ptt_proto_parse(buf, n, &r, &p)); /* magic */
    buf[0] = 'P';
    buf[2] = 9;
    CHECK(!ptt_proto_parse(buf, n, &r, &p)); /* version */
    buf[2] = PTT_PROTO_VERSION;
    buf[14] = 0;
    CHECK(!ptt_proto_parse(buf, n, &r, &p)); /* channel 0 */
    buf[14] = PTT_MAX_CHANNEL + 1;
    CHECK(!ptt_proto_parse(buf, n, &r, &p)); /* channel too high */
    buf[14] = 1;
    buf[3] = 99;
    CHECK(!ptt_proto_parse(buf, n, &r, &p)); /* unknown type */
    buf[3] = PTT_MSG_HELLO;
    CHECK(ptt_proto_parse(buf, n, &r, &p));

    /* Payload larger than the buffer we were given must not build. */
    h.payload_len = 100;
    uint8_t big[100] = {0};
    CHECK(ptt_proto_build(buf, PTT_HDR_LEN + 99, &h, big) == 0);
}

static void test_hello(void)
{
    uint8_t buf[PTT_HELLO_LEN];
    ptt_hello_t in = {.battery = 87, .state = PTT_PEER_TALKING};
    strcpy(in.name, "Kitchen");
    CHECK(ptt_hello_encode(buf, sizeof(buf), &in) == PTT_HELLO_LEN);
    ptt_hello_t out;
    CHECK(ptt_hello_decode(buf, sizeof(buf), &out));
    CHECK(strcmp(out.name, "Kitchen") == 0 && out.battery == 87 && out.state == PTT_PEER_TALKING);

    /* A full-length name with no NUL and a control character stays printable and terminated. */
    memset(buf, 'A', PTT_NAME_LEN);
    buf[3] = '\n';
    CHECK(ptt_hello_decode(buf, sizeof(buf), &out));
    CHECK(strlen(out.name) == PTT_NAME_LEN && out.name[3] == '?');
    CHECK(!ptt_hello_decode(buf, PTT_HELLO_LEN - 1, &out));
}

/* ---------------------------------------------------------------- peers */

static void test_peers(void)
{
    ptt_peers_t t;
    ptt_peers_init(&t);
    ptt_hello_t h = {.battery = 50};
    strcpy(h.name, "A");
    CHECK(ptt_peers_update(&t, 10, 0x0100A8C0, 1, &h, 1000));
    strcpy(h.name, "B");
    CHECK(ptt_peers_update(&t, 20, 0x0200A8C0, 2, &h, 1000));
    ptt_peers_touch(&t, 30, 0x0300A8C0, 1, 1000); /* audio before HELLO */

    CHECK(ptt_peers_count(&t, 0) == 3);
    CHECK(ptt_peers_count(&t, 1) == 2);
    uint32_t ips[4];
    CHECK(ptt_peers_ips(&t, 1, ips, 4) == 2);
    CHECK(strcmp(ptt_peers_find(&t, 30)->name, "?") == 0);

    /* Peer 10 moves to channel 2 and stays alive; the others expire. */
    strcpy(h.name, "A");
    ptt_peers_update(&t, 10, 0x0100A8C0, 2, &h, 6000);
    CHECK(ptt_peers_expire(&t, 1000 + PTT_PEER_TIMEOUT_MS + 1) == 2);
    CHECK(ptt_peers_count(&t, 0) == 1 && ptt_peers_count(&t, 2) == 1);

    /* Table full. */
    ptt_peers_init(&t);
    for (uint32_t i = 0; i < PTT_MAX_PEERS; i++) {
        CHECK(ptt_peers_update(&t, 100 + i, i, 1, NULL, 0));
    }
    CHECK(!ptt_peers_update(&t, 999, 0, 1, NULL, 0));
    CHECK(ptt_peers_update(&t, 100, 0, 1, NULL, 0)); /* existing still updates */
}

/* ---------------------------------------------------------------- floor */

static void test_floor_basic_talk(void)
{
    ptt_floor_t f;
    bool play;
    ptt_floor_init(&f, 50);

    CHECK(ptt_floor_press(&f, 111, 0) == PTT_EV_TX_START);
    CHECK(f.state == PTT_FLOOR_TX && f.talk_id == 111);
    CHECK(ptt_floor_press(&f, 222, 10) == PTT_EV_NONE); /* already talking */
    CHECK(ptt_floor_release(&f, 100) == PTT_EV_TX_STOP);
    CHECK(f.state == PTT_FLOOR_IDLE);
    CHECK(ptt_floor_release(&f, 110) == PTT_EV_NONE);

    /* Someone talks: RX, our press is denied, TALK_END returns to idle. */
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_TALK_START, 70, 5, 200, &play) == PTT_EV_RX_START);
    CHECK(!play);
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 70, 5, 220, &play) == PTT_EV_NONE && play);
    CHECK(ptt_floor_press(&f, 333, 230) == PTT_EV_DENIED);
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_TALK_END, 70, 5, 240, &play) == PTT_EV_RX_END);
    CHECK(f.state == PTT_FLOOR_IDLE);

    /* A late AUDIO frame of the talk that just ended must not reopen RX. */
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 70, 5, 250, &play) == PTT_EV_NONE && !play);
    CHECK(f.state == PTT_FLOOR_IDLE);
    /* But the same person pressing again (new talk id) is heard. */
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 70, 6, 300, &play) == PTT_EV_RX_START && play);
}

static void test_floor_missed_start_and_timeout(void)
{
    ptt_floor_t f;
    bool play;
    ptt_floor_init(&f, 50);

    /* TALK_START lost: the first AUDIO frame opens RX. */
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 70, 9, 0, &play) == PTT_EV_RX_START && play);
    CHECK(ptt_floor_tick(&f, PTT_RX_TIMEOUT_MS) == PTT_EV_NONE);
    /* TALK_END lost: silence times out. */
    CHECK(ptt_floor_tick(&f, PTT_RX_TIMEOUT_MS + 1) == PTT_EV_RX_TIMEOUT);
    CHECK(f.state == PTT_FLOOR_IDLE);

    /* Talker pressed again and we missed the TALK_END in between. */
    ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 70, 10, 1000, &play);
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 70, 11, 1020, &play) == PTT_EV_RX_SWITCH && play);
    CHECK(f.talk_id == 11);

    /* Our own packets are ignored. */
    ptt_floor_init(&f, 50);
    CHECK(ptt_floor_on_packet(&f, PTT_MSG_AUDIO, 50, 1, 0, &play) == PTT_EV_NONE && !play);
    CHECK(f.state == PTT_FLOOR_IDLE);
}

static void test_floor_tx_timeout(void)
{
    ptt_floor_t f;
    ptt_floor_init(&f, 50);
    ptt_floor_press(&f, 1, 1000);
    CHECK(ptt_floor_tick(&f, 1000 + PTT_MAX_TALK_MS) == PTT_EV_NONE);
    CHECK(ptt_floor_tick(&f, 1000 + PTT_MAX_TALK_MS + 1) == PTT_EV_TX_TIMEOUT);
    CHECK(f.state == PTT_FLOOR_IDLE);
    /* Button still held: releasing later is harmless. */
    CHECK(ptt_floor_release(&f, 99999) == PTT_EV_NONE);
}

static void test_floor_collision(void)
{
    /* Devices 30 and 80 press at the same moment; 30 has the lower id and wins on both. */
    ptt_floor_t a, b;
    bool play;
    ptt_floor_init(&a, 30);
    ptt_floor_init(&b, 80);
    ptt_floor_press(&a, 1000, 0);
    ptt_floor_press(&b, 2000, 0);

    CHECK(ptt_floor_on_packet(&a, PTT_MSG_TALK_START, 80, 2000, 5, &play) == PTT_EV_NONE);
    CHECK(a.state == PTT_FLOOR_TX);
    CHECK(ptt_floor_on_packet(&b, PTT_MSG_TALK_START, 30, 1000, 5, &play) == PTT_EV_TX_YIELD);
    CHECK(b.state == PTT_FLOOR_RX && b.talker_id == 30);
    /* b keeps hearing a and its own release does nothing. */
    CHECK(ptt_floor_on_packet(&b, PTT_MSG_AUDIO, 30, 1000, 25, &play) == PTT_EV_NONE && play);
    CHECK(ptt_floor_release(&b, 30) == PTT_EV_NONE);

    /* A third device listening to 80 first switches to 30 once it hears it. */
    ptt_floor_t c;
    ptt_floor_init(&c, 99);
    CHECK(ptt_floor_on_packet(&c, PTT_MSG_AUDIO, 80, 2000, 1, &play) == PTT_EV_RX_START);
    CHECK(ptt_floor_on_packet(&c, PTT_MSG_AUDIO, 30, 1000, 2, &play) == PTT_EV_RX_SWITCH && play);
    CHECK(ptt_floor_on_packet(&c, PTT_MSG_AUDIO, 80, 2000, 3, &play) == PTT_EV_NONE && !play);
    CHECK(c.talker_id == 30);
}

/* ---------------------------------------------------------------- jitter */

static void put_frame(ptt_jitter_t *jb, uint16_t seq)
{
    uint8_t d[2] = {(uint8_t)seq, (uint8_t)(seq >> 8)};
    ptt_jb_put(jb, seq, 0, d, 2);
}

static int get_seq(ptt_jitter_t *jb, ptt_jb_result_t *res)
{
    uint8_t out[PTT_JB_FRAME_MAX];
    uint16_t len = 0;
    uint8_t codec;
    *res = ptt_jb_get(jb, out, &len, &codec);
    return *res == PTT_JB_FRAME ? (out[0] | (out[1] << 8)) : -1;
}

static void test_jitter_reorder_and_loss(void)
{
    ptt_jitter_t jb;
    ptt_jb_result_t r;
    ptt_jb_init(&jb, 3);

    put_frame(&jb, 100);
    CHECK(get_seq(&jb, &r) == -1 && r == PTT_JB_BUFFERING);
    put_frame(&jb, 102); /* 101 arrives late, out of order */
    CHECK(get_seq(&jb, &r) == -1 && r == PTT_JB_BUFFERING);
    put_frame(&jb, 101);
    CHECK(get_seq(&jb, &r) == 100);
    CHECK(get_seq(&jb, &r) == 101);
    put_frame(&jb, 104); /* 103 lost */
    CHECK(get_seq(&jb, &r) == 102);
    CHECK(get_seq(&jb, &r) == -1 && r == PTT_JB_LOST);
    CHECK(get_seq(&jb, &r) == 104);
    CHECK(jb.stat_lost == 1);

    /* 103 shows up after its slot was concealed: dropped as late. */
    uint8_t d[2] = {0};
    CHECK(!ptt_jb_put(&jb, 103, 0, d, 2));
    CHECK(jb.stat_late == 1);

    /* Dry: back to buffering until prefill is reached again. */
    CHECK(get_seq(&jb, &r) == -1 && r == PTT_JB_BUFFERING);
    CHECK(jb.stat_underruns == 1);
    put_frame(&jb, 105);
    put_frame(&jb, 106);
    CHECK(get_seq(&jb, &r) == -1 && r == PTT_JB_BUFFERING);
    put_frame(&jb, 107);
    CHECK(get_seq(&jb, &r) == 105);
}

static void test_jitter_wrap_and_jump(void)
{
    ptt_jitter_t jb;
    ptt_jb_result_t r;
    ptt_jb_init(&jb, 2);

    put_frame(&jb, 65534);
    put_frame(&jb, 65535);
    put_frame(&jb, 0);
    CHECK(get_seq(&jb, &r) == 65534);
    CHECK(get_seq(&jb, &r) == 65535);
    CHECK(get_seq(&jb, &r) == 0);

    /* A long stall: the next frame is far ahead. Skip forward instead of playing stale gaps. */
    put_frame(&jb, 500);
    put_frame(&jb, 501);
    CHECK(get_seq(&jb, &r) == 500);
    CHECK(get_seq(&jb, &r) == 501);

    /* A burst of 10 frames with prefill 2 (max queue 6): latency is trimmed back. */
    for (uint16_t s = 502; s < 512; s++) {
        put_frame(&jb, s);
    }
    CHECK(get_seq(&jb, &r) == 503); /* 502 trimmed, 8 left */
    CHECK(get_seq(&jb, &r) == 505); /* 504 trimmed, 6 left: within limit */
    CHECK(get_seq(&jb, &r) == 506);
    CHECK(get_seq(&jb, &r) == 507);
    CHECK(jb.stat_trimmed == 2);

    /* Oversized frame rejected. */
    uint8_t big[PTT_JB_FRAME_MAX + 1] = {0};
    CHECK(!ptt_jb_put(&jb, 502, 0, big, sizeof(big)));

    ptt_jb_reset(&jb);
    CHECK(ptt_jb_count(&jb) == 0 && jb.prefill == 2 && !jb.started);
}

/* ---------------------------------------------------------------- adpcm */

static void test_adpcm_quality(void)
{
    enum { N = 320 };
    int16_t pcm[N], out[N];
    uint8_t enc[PTT_ADPCM_BYTES(N)];
    ptt_adpcm_state_t st;
    ptt_adpcm_init(&st);

    double sig = 0, err = 0;
    for (int frame = 0; frame < 20; frame++) {
        for (int i = 0; i < N; i++) {
            int t = frame * N + i;
            pcm[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * t / 16000.0) +
                               3000 * sin(2 * M_PI * 1300 * t / 16000.0));
        }
        CHECK(ptt_adpcm_encode(&st, pcm, N, enc) == sizeof(enc));
        CHECK(ptt_adpcm_decode(enc, sizeof(enc), out, N) == N);
        if (frame >= 2) { /* let the step size adapt first */
            for (int i = 0; i < N; i++) {
                sig += (double)pcm[i] * pcm[i];
                err += (double)(pcm[i] - out[i]) * (pcm[i] - out[i]);
            }
        }
    }
    double snr = 10 * log10(sig / err);
    printf("  adpcm SNR %.1f dB\n", snr);
    CHECK(snr > 20.0);

    /* Frames decode independently: decoding a later frame alone gives the same samples. */
    int16_t again[N];
    CHECK(ptt_adpcm_decode(enc, sizeof(enc), again, N) == N);
    CHECK(memcmp(again, out, sizeof(out)) == 0);

    /* Corrupt header rejected; short output buffer respected. */
    uint8_t bad[PTT_ADPCM_HDR + 1] = {0, 0, 89, 0, 0};
    CHECK(ptt_adpcm_decode(bad, sizeof(bad), out, N) == 0);
    CHECK(ptt_adpcm_decode(enc, sizeof(enc), out, 7) == 7);

    /* Full-scale square wave does not overflow. */
    for (int i = 0; i < N; i++) {
        pcm[i] = (i / 8) % 2 ? 32767 : -32768;
    }
    ptt_adpcm_init(&st);
    for (int k = 0; k < 5; k++) {
        ptt_adpcm_encode(&st, pcm, N, enc);
    }
    CHECK(ptt_adpcm_decode(enc, sizeof(enc), out, N) == N);
}

int main(void)
{
    test_proto_roundtrip();
    test_proto_rejects_bad_input();
    test_hello();
    test_peers();
    test_floor_basic_talk();
    test_floor_missed_start_and_timeout();
    test_floor_tx_timeout();
    test_floor_collision();
    test_jitter_reorder_and_loss();
    test_jitter_wrap_and_jump();
    test_adpcm_quality();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
