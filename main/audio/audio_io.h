/*
 * Microphone and speaker at 16 kHz, mono, 16-bit.
 *
 * Reads and writes block until the I2S DMA has room, so a task that writes
 * one 20 ms frame per loop is paced by the hardware clock.
 * audio_io_read() is called from one task only, audio_io_write() from one
 * (other) task only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_FRAME_MS 20
#define AUDIO_FRAME_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000) /* 320 */

esp_err_t audio_io_init(const board_t *b);

/* Fill pcm with exactly `samples` samples. Returns samples read, 0 on error. */
int audio_io_read(int16_t *pcm, int samples);

/*
 * Same, plus the hardware echo reference (what the speaker is playing, as
 * the ES7210 hears it) when the board has one; zeros otherwise.
 */
int audio_io_read_ref(int16_t *pcm, int16_t *ref, int samples);
bool audio_io_has_hw_ref(void);

/* Play `samples` samples. Returns samples written, 0 on error. */
int audio_io_write(const int16_t *pcm, int samples);

/* Amplifier on/off. Keep it off while transmitting so the mic does not pick up the speaker. */
void audio_io_speaker_enable(bool on);

void audio_io_set_volume(int volume); /* 0..100 */

#ifdef __cplusplus
}
#endif
