/*
 * Jitter buffer for 20 ms voice frames, keyed by the 16-bit sequence number.
 *
 * Wi-Fi delivers frames in bursts and sometimes out of order. The buffer
 * waits until `prefill` frames are queued, then hands out exactly one frame
 * per call in sequence order. A gap is reported as PTT_JB_LOST so the caller
 * can conceal it; running dry returns to buffering. If a burst leaves more
 * than 2 * prefill + 2 frames queued, the oldest are dropped one per call
 * so latency does not stay high after a Wi-Fi stall.
 *
 * Not thread safe: the caller serialises access.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTT_JB_SLOTS 16
#define PTT_JB_FRAME_MAX 640

typedef enum {
    PTT_JB_FRAME = 0,  /* *out holds the next frame */
    PTT_JB_LOST,       /* the next frame is missing but later ones exist: conceal one frame */
    PTT_JB_BUFFERING,  /* not enough queued: play silence */
} ptt_jb_result_t;

typedef struct {
    bool used;
    uint16_t seq;
    uint16_t len;
    uint8_t codec;
    uint8_t data[PTT_JB_FRAME_MAX];
} ptt_jb_slot_t;

typedef struct {
    ptt_jb_slot_t slots[PTT_JB_SLOTS];
    int prefill;
    bool started; /* play_seq is valid */
    bool playing; /* prefill reached */
    uint16_t play_seq;
    uint32_t stat_received;
    uint32_t stat_lost;
    uint32_t stat_late;
    uint32_t stat_underruns;
    uint32_t stat_trimmed; /* frames dropped to cut latency */
} ptt_jitter_t;

void ptt_jb_init(ptt_jitter_t *jb, int prefill);
void ptt_jb_reset(ptt_jitter_t *jb); /* keeps prefill, clears frames and stats */

/* Returns false if the frame was dropped (late, duplicate of a played frame, or too big). */
bool ptt_jb_put(ptt_jitter_t *jb, uint16_t seq, uint8_t codec, const uint8_t *data, uint16_t len);

ptt_jb_result_t ptt_jb_get(ptt_jitter_t *jb, uint8_t *out, uint16_t *len, uint8_t *codec);

int ptt_jb_count(const ptt_jitter_t *jb);

#ifdef __cplusplus
}
#endif
