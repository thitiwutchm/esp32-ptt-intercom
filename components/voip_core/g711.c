/* After the classic Sun Microsystems reference implementation (g711.c). */
#include "g711.h"

static int segment(int val, const int16_t *table, int size)
{
    for (int i = 0; i < size; i++) {
        if (val <= table[i]) {
            return i;
        }
    }
    return size;
}

static const int16_t kSegAEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
static const int16_t kSegUEnd[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};

uint8_t g711_alaw_encode(int16_t pcm)
{
    int val = pcm >> 3;
    int mask;
    if (val >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        val = -val - 1;
    }
    int seg = segment(val, kSegAEnd, 8);
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    int aval = seg << 4;
    aval |= seg < 2 ? (val >> 1) & 0x0F : (val >> seg) & 0x0F;
    return (uint8_t)(aval ^ mask);
}

int16_t g711_alaw_decode(uint8_t code)
{
    code ^= 0x55;
    int t = (code & 0x0F) << 4;
    int seg = (code & 0x70) >> 4;
    switch (seg) {
    case 0:
        t += 8;
        break;
    case 1:
        t += 0x108;
        break;
    default:
        t += 0x108;
        t <<= seg - 1;
    }
    return (int16_t)((code & 0x80) ? t : -t);
}

#define ULAW_BIAS 0x84

uint8_t g711_ulaw_encode(int16_t pcm)
{
    int val = pcm >> 2;
    int mask;
    if (val < 0) {
        val = -val;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    if (val > 8159) {
        val = 8159;
    }
    val += ULAW_BIAS >> 2;
    int seg = segment(val, kSegUEnd, 8);
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);
    }
    int uval = (seg << 4) | ((val >> (seg + 1)) & 0x0F);
    return (uint8_t)(uval ^ mask);
}

int16_t g711_ulaw_decode(uint8_t code)
{
    code = (uint8_t)~code;
    int t = ((code & 0x0F) << 3) + ULAW_BIAS;
    t <<= (code & 0x70) >> 4;
    return (int16_t)((code & 0x80) ? (ULAW_BIAS - t) : (t - ULAW_BIAS));
}

void g711_encode(int pt, const int16_t *pcm, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        out[i] = pt == 8 ? g711_alaw_encode(pcm[i]) : g711_ulaw_encode(pcm[i]);
    }
}

void g711_decode(int pt, const uint8_t *in, int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) {
        pcm[i] = pt == 8 ? g711_alaw_decode(in[i]) : g711_ulaw_decode(in[i]);
    }
}
