/*
 * The intercom itself: ties floor control, peers, jitter buffer, audio,
 * network and UI together.
 *
 * Tasks:
 *   core 1  audio_tx  (prio 20)  mic -> encode -> unicast to every peer on the channel
 *   core 1  audio_rx  (prio 19)  jitter buffer -> decode -> speaker
 *   core 0  net_rx    (prio 18)  UDP -> ptt_app packet handler
 *   core 0  app       (prio 5)   button/touch events, timeouts, HELLO every 2 s, UI
 */
#pragma once

#include "board.h"
#include "esp_err.h"

esp_err_t ptt_app_start(const board_t *b);
