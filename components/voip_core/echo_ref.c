#include "echo_ref.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void echo_ref_init(echo_ref_t *r, int16_t *buf, uint32_t size_pow2)
{
    r->buf = buf;
    r->size = size_pow2;
    r->written = 0;
    memset(buf, 0, size_pow2 * sizeof(int16_t));
}

void echo_ref_push(echo_ref_t *r, const int16_t *pcm, int n)
{
    uint32_t w = r->written;
    for (int i = 0; i < n; i++) {
        r->buf[(w + (uint32_t)i) & (r->size - 1)] = pcm[i];
    }
    r->written = w + (uint32_t)n;
}

void echo_ref_read(const echo_ref_t *r, uint32_t index, int16_t *out, int n)
{
    uint32_t w = r->written;
    for (int i = 0; i < n; i++) {
        uint32_t k = index + (uint32_t)i;
        uint32_t age = w - k; /* 1 = newest sample */
        out[i] = (age >= 1 && age <= r->size) ? r->buf[k & (r->size - 1)] : 0;
    }
}

#define DECIM 4

static float corr_at(const float *x, const float *y, int n, int d, const float *x_energy_prefix)
{
    float acc = 0;
    for (int i = 0; i < n; i++) {
        acc += y[i] * x[i + d];
    }
    float ex = x_energy_prefix[d + n] - x_energy_prefix[d];
    return ex > 0 ? acc / sqrtf(ex) : 0;
}

int delay_find(const int16_t *x, int x_len, const int16_t *y, int y_len, float *confidence)
{
    *confidence = 0;
    if (y_len < DECIM * 16 || x_len < y_len) {
        return -1;
    }
    /* Coarse search on 4x decimated signals, then refine at full rate. */
    int xn = x_len / DECIM, yn = y_len / DECIM;
    float *xd = malloc(sizeof(float) * (size_t)xn);
    float *yd = malloc(sizeof(float) * (size_t)yn);
    float *xp = malloc(sizeof(float) * (size_t)(xn + 1));
    if (!xd || !yd || !xp) {
        free(xd);
        free(yd);
        free(xp);
        return -1;
    }
    for (int i = 0; i < xn; i++) {
        float s = 0;
        for (int k = 0; k < DECIM; k++) {
            s += x[i * DECIM + k];
        }
        xd[i] = s;
    }
    float ey = 0;
    for (int i = 0; i < yn; i++) {
        float s = 0;
        for (int k = 0; k < DECIM; k++) {
            s += y[i * DECIM + k];
        }
        yd[i] = s;
        ey += s * s;
    }
    xp[0] = 0;
    for (int i = 0; i < xn; i++) {
        xp[i + 1] = xp[i] + xd[i] * xd[i];
    }
    int best = 0;
    float best_c = -INFINITY;
    for (int d = 0; d + yn <= xn; d++) {
        float c = fabsf(corr_at(xd, yd, yn, d, xp));
        if (c > best_c) {
            best_c = c;
            best = d;
        }
    }
    free(xd);
    free(yd);
    free(xp);
    if (ey <= 0) {
        return -1;
    }

    /* Refine around the coarse peak with full-rate samples. */
    int lo = best * DECIM - DECIM * 2, hi = best * DECIM + DECIM * 2;
    if (lo < 0) {
        lo = 0;
    }
    if (hi > x_len - y_len) {
        hi = x_len - y_len;
    }
    double eyf = 0;
    for (int i = 0; i < y_len; i++) {
        eyf += (double)y[i] * y[i];
    }
    int best_d = lo;
    double best_n = -1;
    for (int d = lo; d <= hi; d++) {
        double acc = 0, ex = 0;
        for (int i = 0; i < y_len; i++) {
            acc += (double)y[i] * x[i + d];
            ex += (double)x[i + d] * x[i + d];
        }
        double n = (ex > 0 && eyf > 0) ? fabs(acc) / sqrt(ex * eyf) : 0;
        if (n > best_n) {
            best_n = n;
            best_d = d;
        }
    }
    *confidence = (float)best_n;
    return best_d;
}
