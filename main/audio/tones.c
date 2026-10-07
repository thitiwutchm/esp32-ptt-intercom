#include "tones.h"

#include <math.h>

#include "audio_io.h"

#define AMPLITUDE 6000

typedef struct {
    int freq_hz; /* 0 = silence */
    int frames;
} tone_step_t;

static const tone_step_t kRxStart[] = {{1000, 3}};
static const tone_step_t kRoger[] = {{1400, 2}, {0, 1}, {1000, 3}};
static const tone_step_t kRing[] = {{880, 20}, {0, 10}, {880, 20}, {0, 110}};
static const tone_step_t kRingback[] = {{425, 50}, {0, 200}};
static const tone_step_t kHangup[] = {{800, 6}, {0, 3}, {600, 8}};

#define STEPS(t) (*count = sizeof(t) / sizeof((t)[0]), (t))

static const tone_step_t *steps(tone_t tone, int *count)
{
    switch (tone) {
    case TONE_ROGER: return STEPS(kRoger);
    case TONE_RING: return STEPS(kRing);
    case TONE_RINGBACK: return STEPS(kRingback);
    case TONE_HANGUP: return STEPS(kHangup);
    case TONE_RX_START:
    default: return STEPS(kRxStart);
    }
}

int tone_frames(tone_t tone)
{
    int n, total = 0;
    const tone_step_t *s = steps(tone, &n);
    for (int i = 0; i < n; i++) {
        total += s[i].frames;
    }
    return total;
}

void tone_frame(tone_t tone, int frame_index, int16_t *pcm)
{
    int n;
    const tone_step_t *s = steps(tone, &n);
    int freq = 0;
    int start = 0; /* first frame of the current step, for click-free fades */
    int len = 0;
    for (int i = 0, f = 0; i < n; f += s[i].frames, i++) {
        if (frame_index < f + s[i].frames) {
            freq = s[i].freq_hz;
            start = f;
            len = s[i].frames;
            break;
        }
    }
    const int total = len * AUDIO_FRAME_SAMPLES;
    const int fade = AUDIO_SAMPLE_RATE / 400; /* 2.5 ms */
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        if (!freq) {
            pcm[i] = 0;
            continue;
        }
        int t = (frame_index - start) * AUDIO_FRAME_SAMPLES + i;
        float env = 1.0f;
        if (t < fade) {
            env = (float)t / fade;
        } else if (total - t < fade) {
            env = (float)(total - t) / fade;
        }
        pcm[i] = (int16_t)(AMPLITUDE * env * sinf(2.0f * (float)M_PI * freq * t / AUDIO_SAMPLE_RATE));
    }
}
