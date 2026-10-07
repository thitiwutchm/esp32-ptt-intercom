#include "resample.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float s_taps[RESAMPLE_TAPS];
static int s_ready;

/* Low-pass at 3.6 kHz for 16 kHz input (keeps the 300-3400 Hz telephone band). */
static void make_taps(void)
{
    const float fc = 3600.0f / 16000.0f;
    const int mid = RESAMPLE_TAPS / 2;
    float sum = 0;
    for (int i = 0; i < RESAMPLE_TAPS; i++) {
        int k = i - mid;
        float sinc = k == 0 ? 2 * fc : sinf(2 * (float)M_PI * fc * k) / ((float)M_PI * k);
        float window = 0.54f - 0.46f * cosf(2 * (float)M_PI * i / (RESAMPLE_TAPS - 1)); /* Hamming */
        s_taps[i] = sinc * window;
        sum += s_taps[i];
    }
    for (int i = 0; i < RESAMPLE_TAPS; i++) {
        s_taps[i] /= sum;
    }
    s_ready = 1;
}

void resample_init(resample_t *r)
{
    if (!s_ready) {
        make_taps();
    }
    memset(r, 0, sizeof(*r));
}

static void push(resample_t *r, float x)
{
    r->hist[r->pos] = x;
    r->pos = (r->pos + 1) % RESAMPLE_TAPS;
}

static float filter(const resample_t *r)
{
    float acc = 0;
    int p = r->pos; /* oldest sample */
    for (int i = 0; i < RESAMPLE_TAPS; i++) {
        acc += s_taps[i] * r->hist[p];
        p = p + 1 == RESAMPLE_TAPS ? 0 : p + 1;
    }
    return acc;
}

static int16_t clamp16(float v)
{
    return (int16_t)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : lrintf(v)));
}

void resample_down2(resample_t *r, const int16_t *in, int n_in, int16_t *out)
{
    for (int i = 0; i < n_in; i += 2) {
        push(r, in[i]);
        push(r, in[i + 1]);
        out[i / 2] = clamp16(filter(r));
    }
}

void resample_up2(resample_t *r, const int16_t *in, int n_in, int16_t *out)
{
    for (int i = 0; i < n_in; i++) {
        /* Zero stuffing halves the energy: the filter gain of 2 restores it. */
        push(r, 2.0f * in[i]);
        out[2 * i] = clamp16(filter(r));
        push(r, 0);
        out[2 * i + 1] = clamp16(filter(r));
    }
}
