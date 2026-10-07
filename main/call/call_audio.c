#include "call_audio.h"

#include <math.h>
#include <string.h>

#include "audio_io.h"
#include "echo_ref.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "g711.h"
#include "ptt_jitter.h"
#include "resample.h"
#include "sdkconfig.h"
#include "sip_client.h"

#if CONFIG_PTT_SIP_ENABLE

#include "esp_aec.h"

static const char *TAG = "call_audio";

#define FRAME AUDIO_FRAME_SAMPLES /* 320 @ 16 kHz */
#define FRAME_8K (FRAME / 2)
#define REF_RING (1 << 14)        /* ~1 s of played audio */
#define AEC_LEAD 80               /* feed the reference 5 ms ahead of its echo (ESP-SR wants 0-10 ms) */

/* Chirp calibration: 300 ms sweep, then 600 ms of microphone to search in. */
#define CHIRP_FRAMES 15
#define CAL_MIC (FRAME * 30)            /* 600 ms analysed */
#define CAL_EXTRA (FRAME * 5)           /* let the reference catch up before searching */
#define CAL_BEFORE 4000                 /* search from 250 ms before the mic window ... */
#define CAL_SPAN (CAL_MIC + CAL_BEFORE + 1600) /* ... to 100 ms after */
#define CAL_MIN_CONFIDENCE 0.35f

#define DUCK_GAIN 0.125f /* -18 dB */
#define DUCK_HOLD_MS 200
#define FAR_ACTIVE_RMS 300

typedef enum { ECHO_AEC, ECHO_DUCK, ECHO_NONE } echo_mode_t;

typedef enum {
    CAL_NONE = 0,    /* hardware reference, or not AEC */
    CAL_WANT_CHIRP,  /* speaker side plays the chirp next */
    CAL_RECORDING,   /* mic side collects CAL_MIC + CAL_EXTRA samples */
    CAL_COMPUTING,   /* calibration task searches */
    CAL_DONE,        /* s_lag valid */
    CAL_FAILED,      /* duck for the rest of the call */
} cal_state_t;

static bool s_hw_ref;
static echo_mode_t s_mode;
static SemaphoreHandle_t s_jb_lock;
static ptt_jitter_t s_jb;
static resample_t s_up, s_down;

static aec_handle_t *s_aec;
static int s_chunk;
static int16_t *s_aec_mic, *s_aec_ref, *s_aec_out; /* one AEC chunk each, 16-byte aligned */
#define AEC_MAX_CHUNK 1024
static int16_t s_in_mic[FRAME + AEC_MAX_CHUNK], s_in_ref[FRAME + AEC_MAX_CHUNK];
static int s_in_fill;
static int16_t s_out[FRAME + AEC_MAX_CHUNK];
static int s_out_fill;

/* software reference */
static echo_ref_t s_ref;
static int16_t *s_ref_buf;
static uint32_t s_mic_count; /* microphone samples consumed this call */
static volatile cal_state_t s_cal;
static volatile int32_t s_lag; /* echo of played sample p appears at mic sample p + s_lag */
static int16_t *s_cal_mic, *s_cal_ref;
static int s_cal_fill;
static uint32_t s_cal_mic_start, s_cal_ref_start;
static TaskHandle_t s_cal_task;
static int s_chirp_frame;

/* ducking */
static volatile uint32_t s_far_until_ms;

/* Set by the speaker side once everything is reset for a new call; the mic side waits for it. */
static volatile bool s_running;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ------------------------------------------------------------------ calibration */

static void chirp(int frame, int16_t *out)
{
    /* 400 -> 3400 Hz linear sweep, faded in and out. */
    const float f0 = 400, f1 = 3400, dur = CHIRP_FRAMES * FRAME / (float)AUDIO_SAMPLE_RATE;
    for (int i = 0; i < FRAME; i++) {
        float t = (frame * FRAME + i) / (float)AUDIO_SAMPLE_RATE;
        float phase = 2 * (float)M_PI * (f0 * t + (f1 - f0) * t * t / (2 * dur));
        float env = fminf(1.0f, fminf(t / 0.01f, (dur - t) / 0.01f));
        out[i] = (int16_t)(7000 * env * sinf(phase));
    }
}

static void cal_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_cal != CAL_COMPUTING) {
            continue;
        }
        echo_ref_read(&s_ref, s_cal_ref_start, s_cal_ref, CAL_SPAN);
        float conf = 0;
        int d = delay_find(s_cal_ref, CAL_SPAN, s_cal_mic, CAL_MIC, &conf);
        if (d >= 0 && conf >= CAL_MIN_CONFIDENCE) {
            s_lag = (int32_t)(s_cal_mic_start - (s_cal_ref_start + (uint32_t)d));
            s_cal = CAL_DONE;
            ESP_LOGI(TAG, "echo path %ld samples (%.1f ms), confidence %.2f", (long)s_lag,
                     s_lag * 1000.0f / AUDIO_SAMPLE_RATE, conf);
        } else {
            s_cal = CAL_FAILED;
            ESP_LOGW(TAG, "echo alignment failed (confidence %.2f): ducking instead", conf);
        }
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t call_audio_init(bool hw_ref)
{
    s_hw_ref = hw_ref;
#if CONFIG_PTT_ECHO_AEC
    s_mode = ECHO_AEC;
#elif CONFIG_PTT_ECHO_DUCK
    s_mode = ECHO_DUCK;
#else
    s_mode = ECHO_NONE;
#endif
    s_jb_lock = xSemaphoreCreateMutex();
    ptt_jb_init(&s_jb, 3);

    if (s_mode == ECHO_AEC) {
        s_aec = aec_create(AUDIO_SAMPLE_RATE, 4, 1, AEC_MODE_VOIP_HIGH_PERF);
        if (!s_aec) {
            ESP_LOGE(TAG, "AEC create failed: ducking instead");
            s_mode = ECHO_DUCK;
        } else {
            s_chunk = aec_get_chunksize(s_aec);
            size_t bytes = s_chunk * sizeof(int16_t);
            s_aec_mic = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);
            s_aec_ref = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);
            s_aec_out = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);
            if (!s_aec_mic || !s_aec_ref || !s_aec_out || s_chunk <= 0 || s_chunk > AEC_MAX_CHUNK) {
                ESP_LOGE(TAG, "AEC buffers (chunk %d) failed: ducking instead", s_chunk);
                s_mode = ECHO_DUCK;
            } else {
                ESP_LOGI(TAG, "AEC ready, chunk %d samples, %s reference", s_chunk, hw_ref ? "hardware" : "software");
            }
        }
    }
    if (s_mode == ECHO_AEC && !hw_ref) {
        s_ref_buf = heap_caps_malloc(REF_RING * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_cal_mic = heap_caps_malloc((CAL_MIC + CAL_EXTRA) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_cal_ref = heap_caps_malloc(CAL_SPAN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_ref_buf || !s_cal_mic || !s_cal_ref) {
            ESP_LOGE(TAG, "reference buffers failed: ducking instead");
            s_mode = ECHO_DUCK;
        } else {
            echo_ref_init(&s_ref, s_ref_buf, REF_RING);
            xTaskCreatePinnedToCore(cal_task, "echo_cal", 4096, NULL, 3, &s_cal_task, 0);
        }
    }
    return ESP_OK;
}

void call_audio_begin(void)
{
    xSemaphoreTake(s_jb_lock, portMAX_DELAY);
    ptt_jb_reset(&s_jb);
    xSemaphoreGive(s_jb_lock);
    resample_init(&s_up);
    resample_init(&s_down);
    s_in_fill = 0;
    s_out_fill = 0;
    s_mic_count = 0;
    s_far_until_ms = 0;
    s_chirp_frame = 0;
    s_cal_fill = 0;
    if (s_mode == ECHO_AEC && !s_hw_ref) {
        echo_ref_init(&s_ref, s_ref_buf, REF_RING);
        s_cal = CAL_WANT_CHIRP;
    } else {
        s_cal = CAL_NONE;
    }
    __sync_synchronize();
    s_running = true;
}

void call_audio_end(void)
{
    s_running = false;
    s_cal = CAL_NONE;
}

void call_audio_rtp_in(uint16_t seq, int pt, const uint8_t *payload, size_t len)
{
    if (len > FRAME_8K) {
        len = FRAME_8K; /* 20 ms at 8 kHz; longer ptime is cut */
    }
    xSemaphoreTake(s_jb_lock, portMAX_DELAY);
    ptt_jb_put(&s_jb, seq, (uint8_t)pt, payload, (uint16_t)len);
    xSemaphoreGive(s_jb_lock);
}

static int rms(const int16_t *x, int n)
{
    int64_t acc = 0;
    for (int i = 0; i < n; i++) {
        acc += (int32_t)x[i] * x[i];
    }
    return (int)sqrtf((float)(acc / n));
}

void call_audio_speaker_frame(int16_t *out)
{
    if (s_cal == CAL_WANT_CHIRP || (s_chirp_frame > 0 && s_chirp_frame < CHIRP_FRAMES)) {
        if (s_chirp_frame == 0) {
            s_cal_ref_start = 0; /* set by the mic side when it starts recording */
            s_cal = CAL_RECORDING;
        }
        chirp(s_chirp_frame++, out);
    } else {
        uint8_t frame[PTT_JB_FRAME_MAX];
        uint16_t len = 0;
        uint8_t pt = 0;
        int16_t pcm8k[FRAME_8K];
        xSemaphoreTake(s_jb_lock, portMAX_DELAY);
        ptt_jb_result_t r = ptt_jb_get(&s_jb, frame, &len, &pt);
        xSemaphoreGive(s_jb_lock);
        if (r == PTT_JB_FRAME) {
            int n = len > FRAME_8K ? FRAME_8K : len;
            g711_decode(pt, frame, pcm8k, n);
            for (int i = n; i < FRAME_8K; i++) {
                pcm8k[i] = 0;
            }
        } else {
            memset(pcm8k, 0, sizeof(pcm8k)); /* lost or buffering: silence keeps the timing */
        }
        resample_up2(&s_up, pcm8k, FRAME_8K, out);
    }
    if (rms(out, FRAME) > FAR_ACTIVE_RMS) {
        s_far_until_ms = now_ms() + DUCK_HOLD_MS;
    }
    if (s_ref_buf) {
        echo_ref_push(&s_ref, out, FRAME);
    }
}

static void send_16k(const int16_t *pcm)
{
    int16_t pcm8k[FRAME_8K];
    resample_down2(&s_down, pcm, FRAME, pcm8k);
    sip_client_send_audio(pcm8k, FRAME_8K);
}

/* AEC works in its own chunk size: collect, process, re-frame into 20 ms. */
static void aec_frame(const int16_t *mic, const int16_t *ref, int16_t *out_frame, bool *have_out)
{
    memcpy(s_in_mic + s_in_fill, mic, FRAME * sizeof(int16_t));
    memcpy(s_in_ref + s_in_fill, ref, FRAME * sizeof(int16_t));
    s_in_fill += FRAME;
    while (s_in_fill >= s_chunk) {
        memcpy(s_aec_mic, s_in_mic, s_chunk * sizeof(int16_t));
        memcpy(s_aec_ref, s_in_ref, s_chunk * sizeof(int16_t));
        aec_process(s_aec, s_aec_mic, s_aec_ref, s_aec_out);
        memcpy(s_out + s_out_fill, s_aec_out, s_chunk * sizeof(int16_t));
        s_out_fill += s_chunk;
        s_in_fill -= s_chunk;
        memmove(s_in_mic, s_in_mic + s_chunk, s_in_fill * sizeof(int16_t));
        memmove(s_in_ref, s_in_ref + s_chunk, s_in_fill * sizeof(int16_t));
    }
    *have_out = s_out_fill >= FRAME;
    if (*have_out) {
        memcpy(out_frame, s_out, FRAME * sizeof(int16_t));
        s_out_fill -= FRAME;
        memmove(s_out, s_out + FRAME, s_out_fill * sizeof(int16_t));
    }
}

void call_audio_mic(const int16_t *mic, const int16_t *hw_ref)
{
    static int16_t buf[FRAME], ref[FRAME];
    if (!s_running) {
        return; /* call just connected: the speaker side is still setting up */
    }
    uint32_t m0 = s_mic_count;
    s_mic_count += FRAME;

    cal_state_t cal = s_cal;
    if (cal == CAL_RECORDING) {
        if (s_cal_fill == 0) {
            s_cal_mic_start = m0;
            s_cal_ref_start = s_ref.written - CAL_BEFORE;
        }
        int n = CAL_MIC + CAL_EXTRA - s_cal_fill;
        n = n > FRAME ? FRAME : n;
        memcpy(s_cal_mic + s_cal_fill, mic, n * sizeof(int16_t));
        s_cal_fill += n;
        if (s_cal_fill >= CAL_MIC + CAL_EXTRA) {
            s_cal = CAL_COMPUTING;
            xTaskNotifyGive(s_cal_task);
        }
    }
    if (cal == CAL_WANT_CHIRP || cal == CAL_RECORDING || cal == CAL_COMPUTING) {
        memset(buf, 0, sizeof(buf)); /* the far end hears silence while we measure */
        send_16k(buf);
        return;
    }

    bool use_aec = s_mode == ECHO_AEC && (s_hw_ref || cal == CAL_DONE);
    if (use_aec) {
        if (s_hw_ref) {
            memcpy(ref, hw_ref, sizeof(ref));
        } else {
            echo_ref_read(&s_ref, m0 - (uint32_t)s_lag + AEC_LEAD, ref, FRAME);
        }
        bool have;
        aec_frame(mic, ref, buf, &have);
        if (!have) {
            memset(buf, 0, sizeof(buf)); /* first chunk still filling */
        }
    } else {
        memcpy(buf, mic, sizeof(buf));
        bool duck = s_mode == ECHO_DUCK || cal == CAL_FAILED;
        if (duck && (int32_t)(s_far_until_ms - now_ms()) > 0) {
            for (int i = 0; i < FRAME; i++) {
                buf[i] = (int16_t)(buf[i] * DUCK_GAIN);
            }
        }
    }
    send_16k(buf);
}

#else /* !CONFIG_PTT_SIP_ENABLE */

esp_err_t call_audio_init(bool hw_ref)
{
    (void)hw_ref;
    return ESP_OK;
}
void call_audio_begin(void) {}
void call_audio_end(void) {}
void call_audio_speaker_frame(int16_t *out)
{
    memset(out, 0, AUDIO_FRAME_SAMPLES * sizeof(int16_t));
}
void call_audio_mic(const int16_t *mic, const int16_t *hw_ref)
{
    (void)mic;
    (void)hw_ref;
}
void call_audio_rtp_in(uint16_t seq, int pt, const uint8_t *payload, size_t len)
{
    (void)seq;
    (void)pt;
    (void)payload;
    (void)len;
}

#endif
