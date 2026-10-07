/*
 * Software echo reference for boards without a hardware loopback channel
 * (the CUBE: separate I2S microphone and amplifier).
 *
 * The speaker task pushes every sample it plays; the microphone task asks
 * for the reference that lines up with its samples. Both I2S ports run from
 * the same clock and are kept streaming during a call, so the offset
 * between "sample k was played" and "its echo reaches the mic" is constant:
 * it is measured once per call with delay_find() on a known sound.
 *
 * One writer and one reader task; the reader only touches samples written
 * long before, so no lock is needed beyond an atomic index.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t *buf;
    uint32_t size; /* power of two */
    volatile uint32_t written; /* total samples pushed (wraps) */
} echo_ref_t;

void echo_ref_init(echo_ref_t *r, int16_t *buf, uint32_t size_pow2);
void echo_ref_push(echo_ref_t *r, const int16_t *pcm, int n);

/* Samples [index, index + n) in push order; zeros for anything not (or no longer) in the ring. */
void echo_ref_read(const echo_ref_t *r, uint32_t index, int16_t *out, int n);

/*
 * Find where the microphone block y best matches the reference x: returns d
 * in [0, x_len - y_len] maximising the normalised correlation of y[i] with
 * x[i + d], and the peak correlation (0..1) in *confidence. -1 on bad input.
 */
int delay_find(const int16_t *x, int x_len, const int16_t *y, int y_len, float *confidence);

#ifdef __cplusplus
}
#endif
