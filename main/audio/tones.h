/* Short cue tones, generated on the fly at AUDIO_SAMPLE_RATE. */
#pragma once

#include <stdint.h>

typedef enum {
    TONE_RX_START, /* someone started talking */
    TONE_ROGER,    /* the talker released */
    TONE_RING,     /* incoming call, repeat while ringing (3.2 s cycle) */
    TONE_RINGBACK, /* our call rings at the other end, repeat (5 s cycle) */
    TONE_HANGUP,   /* call ended */
} tone_t;

/* Number of 20 ms frames the tone lasts. */
int tone_frames(tone_t tone);

/* Fill one 20 ms frame (AUDIO_FRAME_SAMPLES) of the tone. */
void tone_frame(tone_t tone, int frame_index, int16_t *pcm);
