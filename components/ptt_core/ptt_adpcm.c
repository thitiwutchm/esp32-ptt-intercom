#include "ptt_adpcm.h"

static const int8_t kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};

void ptt_adpcm_init(ptt_adpcm_state_t *st)
{
    st->predictor = 0;
    st->index = 0;
}

static int clamp_index(int i)
{
    return i < 0 ? 0 : (i > 88 ? 88 : i);
}

static int16_t clamp16(int v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* Apply one 4-bit code to (pred, index); shared by encoder and decoder so they never drift. */
static void step(int *pred, int *index, uint8_t code)
{
    int s = kStepTable[*index];
    int diff = s >> 3;
    if (code & 4) diff += s;
    if (code & 2) diff += s >> 1;
    if (code & 1) diff += s >> 2;
    *pred = clamp16((code & 8) ? *pred - diff : *pred + diff);
    *index = clamp_index(*index + kIndexTable[code]);
}

static uint8_t encode_sample(int *pred, int *index, int16_t sample)
{
    int s = kStepTable[*index];
    int diff = sample - *pred;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= s) { code |= 4; diff -= s; }
    s >>= 1;
    if (diff >= s) { code |= 2; diff -= s; }
    s >>= 1;
    if (diff >= s) { code |= 1; }
    step(pred, index, code);
    return code;
}

size_t ptt_adpcm_encode(ptt_adpcm_state_t *st, const int16_t *pcm, int samples, uint8_t *out)
{
    int pred = st->predictor;
    int index = st->index;
    out[0] = (uint8_t)(pred & 0xff);
    out[1] = (uint8_t)((pred >> 8) & 0xff);
    out[2] = (uint8_t)index;
    out[3] = 0;
    uint8_t *p = out + PTT_ADPCM_HDR;
    for (int i = 0; i < samples; i += 2) {
        uint8_t lo = encode_sample(&pred, &index, pcm[i]);
        uint8_t hi = (i + 1 < samples) ? encode_sample(&pred, &index, pcm[i + 1]) : 0;
        *p++ = (uint8_t)(lo | (hi << 4));
    }
    st->predictor = (int16_t)pred;
    st->index = (uint8_t)index;
    return PTT_ADPCM_BYTES(samples);
}

int ptt_adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm, int max_samples)
{
    if (len < PTT_ADPCM_HDR || in[2] > 88) {
        return 0;
    }
    int pred = (int16_t)(in[0] | (in[1] << 8));
    int index = in[2];
    int n = 0;
    for (size_t i = PTT_ADPCM_HDR; i < len && n < max_samples; i++) {
        step(&pred, &index, in[i] & 0x0f);
        pcm[n++] = (int16_t)pred;
        if (n < max_samples) {
            step(&pred, &index, in[i] >> 4);
            pcm[n++] = (int16_t)pred;
        }
    }
    return n;
}
