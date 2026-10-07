/*
 * LVGL screen. Works on the 360x360 round EchoEar panel and the 240x240
 * square CUBE panel; on-screen buttons appear only when the board has touch.
 */
#pragma once

#include <stdbool.h>

#include "board.h"
#include "esp_err.h"
#include "ptt_proto.h"

typedef enum {
    UI_MODE_WIFI = 0, /* waiting for Wi-Fi */
    UI_MODE_IDLE,
    UI_MODE_TX,
    UI_MODE_RX,
    UI_MODE_CALL_IN,  /* phone call ringing here */
    UI_MODE_CALL_OUT, /* we are calling */
    UI_MODE_CALL,     /* in a phone call */
} ui_mode_t;

typedef enum {
    UI_EV_PTT_DOWN = 0,
    UI_EV_PTT_UP,
    UI_EV_CH_UP,
    UI_EV_CH_DOWN,
    UI_EV_VOL_UP,
    UI_EV_VOL_DOWN,
    UI_EV_CALL,   /* call / answer / hang up, whichever fits */
    UI_EV_REJECT, /* decline a ringing call */
} ui_event_t;

typedef struct {
    ui_mode_t mode;
    int channel;
    int online;  /* other devices on this channel */
    int battery; /* -1 unknown */
    char name[PTT_NAME_LEN + 1];
    char talker[PTT_NAME_LEN + 1]; /* RX only */
    char notice[32];               /* short-lived message, empty for none */
    bool notice_warn;              /* show the notice in warning colour */
    int sip;                       /* -1 SIP disabled, 0 not registered, 1 registered */
    char call_peer[32];            /* CALL_* modes */
    bool call_ringing;             /* CALL_OUT: the other side rings */
    int call_secs;                 /* CALL: duration */
} ui_view_t;

/* Called from the LVGL task. Must not block: post to a queue. */
typedef void (*ui_event_cb_t)(ui_event_t ev);

esp_err_t ui_init(const board_t *b, ui_event_cb_t cb);

/* Redraw from a snapshot. Safe from any task; do not call while holding locks LVGL callbacks take. */
void ui_update(const ui_view_t *v);
