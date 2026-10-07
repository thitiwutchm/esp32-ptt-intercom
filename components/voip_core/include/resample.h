/*
 * 2:1 rate conversion between the 16 kHz audio path and 8 kHz G.711.
 * Half-band style windowed-sinc FIR, state carried across frames.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESAMPLE_TAPS 31

typedef struct {
    float hist[RESAMPLE_TAPS];
    int pos;
} resample_t;

void resample_init(resample_t *r);

/* 16 kHz -> 8 kHz: n_in must be even; writes n_in / 2 samples. */
void resample_down2(resample_t *r, const int16_t *in, int n_in, int16_t *out);

/* 8 kHz -> 16 kHz: writes 2 * n_in samples. */
void resample_up2(resample_t *r, const int16_t *in, int n_in, int16_t *out);

#ifdef __cplusplus
}
#endif
