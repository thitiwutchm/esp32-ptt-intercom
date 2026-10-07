#include "ptt_floor.h"

#include <string.h>

#include "ptt_proto.h"

void ptt_floor_init(ptt_floor_t *f, uint32_t my_id)
{
    memset(f, 0, sizeof(*f));
    f->my_id = my_id;
}

void ptt_floor_reset(ptt_floor_t *f)
{
    ptt_floor_init(f, f->my_id);
}

static void end_rx(ptt_floor_t *f)
{
    f->ended_valid = true;
    f->ended_talker_id = f->talker_id;
    f->ended_talk_id = f->talk_id;
    f->state = PTT_FLOOR_IDLE;
}

static void begin_rx(ptt_floor_t *f, uint32_t sender_id, uint32_t talk_id, uint32_t now_ms)
{
    f->state = PTT_FLOOR_RX;
    f->talker_id = sender_id;
    f->talk_id = talk_id;
    f->last_rx_ms = now_ms;
}

ptt_floor_event_t ptt_floor_press(ptt_floor_t *f, uint32_t new_talk_id, uint32_t now_ms)
{
    switch (f->state) {
    case PTT_FLOOR_IDLE:
        f->state = PTT_FLOOR_TX;
        f->talker_id = f->my_id;
        f->talk_id = new_talk_id;
        f->tx_start_ms = now_ms;
        return PTT_EV_TX_START;
    case PTT_FLOOR_RX:
        return PTT_EV_DENIED;
    case PTT_FLOOR_TX:
    default:
        return PTT_EV_NONE;
    }
}

ptt_floor_event_t ptt_floor_release(ptt_floor_t *f, uint32_t now_ms)
{
    (void)now_ms;
    if (f->state == PTT_FLOOR_TX) {
        f->state = PTT_FLOOR_IDLE;
        return PTT_EV_TX_STOP;
    }
    return PTT_EV_NONE;
}

ptt_floor_event_t ptt_floor_on_packet(ptt_floor_t *f, uint8_t msg_type, uint32_t sender_id,
                                      uint32_t talk_id, uint32_t now_ms, bool *play)
{
    *play = false;
    if (sender_id == f->my_id) {
        return PTT_EV_NONE; /* our own broadcast echoed back */
    }
    if (f->ended_valid && sender_id == f->ended_talker_id && talk_id == f->ended_talk_id) {
        return PTT_EV_NONE; /* late packet of a talk that already ended */
    }

    if (msg_type == PTT_MSG_TALK_END) {
        if (f->state == PTT_FLOOR_RX && sender_id == f->talker_id && talk_id == f->talk_id) {
            end_rx(f);
            return PTT_EV_RX_END;
        }
        return PTT_EV_NONE;
    }

    if (msg_type != PTT_MSG_TALK_START && msg_type != PTT_MSG_AUDIO) {
        return PTT_EV_NONE;
    }
    bool is_audio = (msg_type == PTT_MSG_AUDIO);

    switch (f->state) {
    case PTT_FLOOR_IDLE:
        begin_rx(f, sender_id, talk_id, now_ms);
        *play = is_audio;
        return PTT_EV_RX_START;

    case PTT_FLOOR_RX:
        if (sender_id == f->talker_id) {
            f->last_rx_ms = now_ms;
            if (talk_id != f->talk_id) {
                /* Same person pressed again and we missed the TALK_END. */
                f->talk_id = talk_id;
                *play = is_audio;
                return PTT_EV_RX_SWITCH;
            }
            *play = is_audio;
            return PTT_EV_NONE;
        }
        if (sender_id < f->talker_id) {
            /* Two others collided and this one wins: follow the winner. */
            begin_rx(f, sender_id, talk_id, now_ms);
            *play = is_audio;
            return PTT_EV_RX_SWITCH;
        }
        return PTT_EV_NONE;

    case PTT_FLOOR_TX:
        if (sender_id < f->my_id) {
            begin_rx(f, sender_id, talk_id, now_ms);
            *play = is_audio;
            return PTT_EV_TX_YIELD;
        }
        return PTT_EV_NONE; /* they will yield to us */
    }
    return PTT_EV_NONE;
}

ptt_floor_event_t ptt_floor_tick(ptt_floor_t *f, uint32_t now_ms)
{
    if (f->state == PTT_FLOOR_RX && (uint32_t)(now_ms - f->last_rx_ms) > PTT_RX_TIMEOUT_MS) {
        end_rx(f);
        return PTT_EV_RX_TIMEOUT;
    }
    if (f->state == PTT_FLOOR_TX && (uint32_t)(now_ms - f->tx_start_ms) > PTT_MAX_TALK_MS) {
        f->state = PTT_FLOOR_IDLE;
        return PTT_EV_TX_TIMEOUT;
    }
    return PTT_EV_NONE;
}

const char *ptt_floor_event_name(ptt_floor_event_t ev)
{
    switch (ev) {
    case PTT_EV_NONE: return "none";
    case PTT_EV_TX_START: return "tx_start";
    case PTT_EV_TX_STOP: return "tx_stop";
    case PTT_EV_TX_TIMEOUT: return "tx_timeout";
    case PTT_EV_TX_YIELD: return "tx_yield";
    case PTT_EV_DENIED: return "denied";
    case PTT_EV_RX_START: return "rx_start";
    case PTT_EV_RX_SWITCH: return "rx_switch";
    case PTT_EV_RX_END: return "rx_end";
    case PTT_EV_RX_TIMEOUT: return "rx_timeout";
    }
    return "?";
}
