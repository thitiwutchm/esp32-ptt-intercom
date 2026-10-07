/*
 * IMA ADPCM, 4 bits per sample (4:1 over PCM16).
 *
 * Each encoded frame starts with the encoder state (predictor int16 LE,
 * step index, reserved byte), so every frame decodes on its own and a lost
 * packet does not corrupt the ones after it. Two samples per byte, low
 * nibble first.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTT_ADPCM_HDR 4
#define PTT_ADPCM_BYTES(samples) (PTT_ADPCM_HDR + ((samples) + 1) / 2)

typedef struct {
    int16_t predictor;
    uint8_t index;
} ptt_adpcm_state_t;

void ptt_adpcm_init(ptt_adpcm_state_t *st);

/* Encode `samples` PCM samples. Returns bytes written (PTT_ADPCM_BYTES(samples)). */
size_t ptt_adpcm_encode(ptt_adpcm_state_t *st, const int16_t *pcm, int samples, uint8_t *out);

/* Decode one frame. Returns the number of samples written (at most max_samples), 0 on error. */
int ptt_adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm, int max_samples);

#ifdef __cplusplus
}
#endif
