/*
 * BLE-only firmware (CONFIG_PTT_PRESENCE_ONLY): a slim app that brings up the
 * display, Wi-Fi (with phone setup) and the Bluetooth presence logger, and
 * nothing of the intercom - no microphone, speaker, walkie-talkie or SIP. It
 * leaves the audio-heavy subsystems uninitialised, which frees the internal
 * RAM that running them alongside NimBLE would need.
 */
#pragma once

#include "board.h"
#include "esp_err.h"

esp_err_t presence_app_start(const board_t *b);
