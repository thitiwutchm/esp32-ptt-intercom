/* Polled, debounced board buttons (active low). */
#pragma once

#include "board.h"

typedef enum {
    BUTTON_PRESS,
    BUTTON_RELEASE,
    BUTTON_CLICK,      /* released before the long-press time */
    BUTTON_LONG_PRESS, /* held for the long-press time; no CLICK follows */
} button_event_t;

typedef void (*button_cb_t)(board_button_role_t role, button_event_t ev);

void buttons_start(const board_t *b, button_cb_t cb);
