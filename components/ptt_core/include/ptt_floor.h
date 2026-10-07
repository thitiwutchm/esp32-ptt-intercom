/*
 * Floor control: decides who may talk on a channel. Half duplex, one talker
 * at a time. Pure logic driven by the caller with timestamps, so it is unit
 * tested on a PC.
 *
 * Collision rule: if two devices start talking at the same time, the one
 * with the LOWER device_id keeps the floor. Every device applies the same
 * rule, so all of them agree on the winner without extra messages.
 *
 * Not thread safe: the caller serialises access.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTT_RX_TIMEOUT_MS 500    /* talker considered gone after this much silence */
#define PTT_MAX_TALK_MS 60000    /* a held button is released automatically */

typedef enum {
    PTT_FLOOR_IDLE = 0,
    PTT_FLOOR_TX,
    PTT_FLOOR_RX,
} ptt_floor_state_t;

typedef enum {
    PTT_EV_NONE = 0,
    PTT_EV_TX_START,   /* we got the floor: start sending */
    PTT_EV_TX_STOP,    /* button released: send TALK_END */
    PTT_EV_TX_TIMEOUT, /* held too long: send TALK_END */
    PTT_EV_TX_YIELD,   /* collision lost: stop sending at once, now receiving */
    PTT_EV_DENIED,     /* button pressed while someone else talks */
    PTT_EV_RX_START,   /* someone started talking: open the speaker */
    PTT_EV_RX_SWITCH,  /* a different talk replaced the current one: reset the jitter buffer */
    PTT_EV_RX_END,     /* talker released (TALK_END) */
    PTT_EV_RX_TIMEOUT, /* talker went silent */
} ptt_floor_event_t;

typedef struct {
    ptt_floor_state_t state;
    uint32_t my_id;
    uint32_t talker_id; /* valid in TX (= my_id) and RX */
    uint32_t talk_id;   /* valid in TX and RX */
    uint32_t tx_start_ms;
    uint32_t last_rx_ms;
    /* The last talk that ended here; its late packets must not reopen RX. */
    bool ended_valid;
    uint32_t ended_talker_id;
    uint32_t ended_talk_id;
} ptt_floor_t;

void ptt_floor_init(ptt_floor_t *f, uint32_t my_id);

ptt_floor_event_t ptt_floor_press(ptt_floor_t *f, uint32_t new_talk_id, uint32_t now_ms);
ptt_floor_event_t ptt_floor_release(ptt_floor_t *f, uint32_t now_ms);

/*
 * Feed a TALK_START, AUDIO or TALK_END from another device on our channel.
 * *play is set when an AUDIO frame belongs to the current talk and should be
 * queued for playback.
 */
ptt_floor_event_t ptt_floor_on_packet(ptt_floor_t *f, uint8_t msg_type, uint32_t sender_id,
                                      uint32_t talk_id, uint32_t now_ms, bool *play);

/* Call periodically (every 20-100 ms) to apply timeouts. */
ptt_floor_event_t ptt_floor_tick(ptt_floor_t *f, uint32_t now_ms);

/* Drop any floor state, e.g. when changing channel. */
void ptt_floor_reset(ptt_floor_t *f);

const char *ptt_floor_event_name(ptt_floor_event_t ev);

#ifdef __cplusplus
}
#endif
