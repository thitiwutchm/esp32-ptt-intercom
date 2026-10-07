/* G.711 mu-law (PCMU, RTP payload 0) and A-law (PCMA, payload 8). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t g711_ulaw_encode(int16_t pcm);
int16_t g711_ulaw_decode(uint8_t code);
uint8_t g711_alaw_encode(int16_t pcm);
int16_t g711_alaw_decode(uint8_t code);

/* Encode or decode n samples with RTP payload type pt (0 = PCMU, 8 = PCMA). */
void g711_encode(int pt, const int16_t *pcm, uint8_t *out, int n);
void g711_decode(int pt, const uint8_t *in, int16_t *pcm, int n);

#ifdef __cplusplus
}
#endif
