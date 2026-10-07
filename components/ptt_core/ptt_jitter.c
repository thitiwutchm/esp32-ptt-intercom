#include "ptt_jitter.h"

#include <string.h>

#define PTT_JB_MAX_QUEUE(jb) ((jb)->prefill * 2 + 2)

void ptt_jb_init(ptt_jitter_t *jb, int prefill)
{
    memset(jb, 0, sizeof(*jb));
    if (prefill < 1) {
        prefill = 1;
    }
    if (prefill > PTT_JB_SLOTS - 1) {
        prefill = PTT_JB_SLOTS - 1;
    }
    jb->prefill = prefill;
}

void ptt_jb_reset(ptt_jitter_t *jb)
{
    ptt_jb_init(jb, jb->prefill);
}

int ptt_jb_count(const ptt_jitter_t *jb)
{
    int n = 0;
    for (int i = 0; i < PTT_JB_SLOTS; i++) {
        n += jb->slots[i].used;
    }
    return n;
}

static void drop_older_than(ptt_jitter_t *jb, uint16_t seq)
{
    for (int i = 0; i < PTT_JB_SLOTS; i++) {
        if (jb->slots[i].used && (int16_t)(jb->slots[i].seq - seq) < 0) {
            jb->slots[i].used = false;
        }
    }
}

bool ptt_jb_put(ptt_jitter_t *jb, uint16_t seq, uint8_t codec, const uint8_t *data, uint16_t len)
{
    if (len > PTT_JB_FRAME_MAX) {
        return false;
    }
    if (!jb->started) {
        jb->started = true;
        jb->play_seq = seq;
    }
    int16_t ahead = (int16_t)(seq - jb->play_seq);
    if (ahead < 0) {
        jb->stat_late++;
        return false;
    }
    if (ahead >= PTT_JB_SLOTS) {
        /* We fell far behind (long stall): restart from this frame. */
        jb->play_seq = seq;
        jb->playing = false;
        drop_older_than(jb, seq);
    }
    ptt_jb_slot_t *s = &jb->slots[seq % PTT_JB_SLOTS];
    s->used = true;
    s->seq = seq;
    s->len = len;
    s->codec = codec;
    memcpy(s->data, data, len);
    jb->stat_received++;
    return true;
}

ptt_jb_result_t ptt_jb_get(ptt_jitter_t *jb, uint8_t *out, uint16_t *len, uint8_t *codec)
{
    int queued = ptt_jb_count(jb);
    if (!jb->playing) {
        if (!jb->started || queued < jb->prefill) {
            return PTT_JB_BUFFERING;
        }
        jb->playing = true;
    }

    /* A burst after a stall leaves too much queued: drop the oldest to win the latency back. */
    if (queued > PTT_JB_MAX_QUEUE(jb)) {
        ptt_jb_slot_t *old = &jb->slots[jb->play_seq % PTT_JB_SLOTS];
        if (old->used && old->seq == jb->play_seq) {
            old->used = false;
            queued--;
        }
        jb->play_seq++;
        jb->stat_trimmed++;
    }

    ptt_jb_slot_t *s = &jb->slots[jb->play_seq % PTT_JB_SLOTS];
    if (s->used && s->seq == jb->play_seq) {
        memcpy(out, s->data, s->len);
        *len = s->len;
        *codec = s->codec;
        s->used = false;
        jb->play_seq++;
        return PTT_JB_FRAME;
    }
    if (queued > 0) {
        jb->stat_lost++;
        jb->play_seq++;
        return PTT_JB_LOST;
    }
    /* Ran dry: wait for prefill again. play_seq stays so the next frame lines up. */
    jb->playing = false;
    jb->stat_underruns++;
    return PTT_JB_BUFFERING;
}
