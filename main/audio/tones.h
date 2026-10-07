/* Short cue tones, generated on the fly at AUDIO_SAMPLE_RATE. */
#pragma once

#include <stdint.h>

typedef enum {
    TONE_RX_START, /* someone started talking */
    TONE_ROGER,    /* the talker released */
} tone_t;

/* Number of 20 ms frames the tone lasts. */
int tone_frames(tone_t tone);

/* Fill one 20 ms frame (AUDIO_FRAME_SAMPLES) of the tone. */
void tone_frame(tone_t tone, int frame_index, int16_t *pcm);
