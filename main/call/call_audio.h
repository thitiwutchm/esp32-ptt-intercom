/*
 * Audio of a SIP call: 16 kHz device audio <-> 8 kHz G.711, a jitter buffer
 * for the far end, and echo control so both sides can talk at once.
 *
 *   speaker side (audio_rx task):  call_audio_speaker_frame() every 20 ms
 *   mic side     (audio_tx task):  call_audio_mic() every 20 ms
 *   network      (SIP task):       call_audio_rtp_in() per RTP packet
 *
 * Echo control (menuconfig "Echo handling during calls"):
 *   AEC with the ES7210 hardware reference on EchoEar; on boards without one
 *   (CUBE) the played audio is the reference, aligned by a chirp that plays
 *   when the call connects. If that alignment fails, or in ducking mode, the
 *   mic is lowered by 18 dB while the far end talks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t call_audio_init(bool hw_ref);

/* A call became active / ended. Called by the speaker task. */
void call_audio_begin(void);
void call_audio_end(void);

/* Next 20 ms (320 samples, 16 kHz) for the speaker. */
void call_audio_speaker_frame(int16_t *out);

/* 20 ms from the microphone (and the hardware reference, or NULL). Sends RTP. */
void call_audio_mic(const int16_t *mic, const int16_t *hw_ref);

/* Far-end audio from the network. */
void call_audio_rtp_in(uint16_t seq, int pt, const uint8_t *payload, size_t len);
