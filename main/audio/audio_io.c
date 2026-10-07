#include "audio_io.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "audio";

#define DMA_DESC_NUM 6
#define DMA_FRAME_NUM 240

static bool s_codec; /* true: ES8311/ES7210 via esp_codec_dev, false: plain I2S */
static gpio_num_t s_pa = GPIO_NUM_NC;
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_out_dev;
static esp_codec_dev_handle_t s_in_dev;
static int s_volume = 70;
static int32_t s_i2s_buf[AUDIO_FRAME_SAMPLES]; /* plain I2S: 32-bit slots, one task each way */
static int32_t s_i2s_out[AUDIO_FRAME_SAMPLES];

#ifdef CONFIG_PTT_MIC_SHIFT
#define MIC_SHIFT CONFIG_PTT_MIC_SHIFT
#else
#define MIC_SHIFT 12
#endif

#ifdef CONFIG_PTT_MIC_GAIN_DB
#define MIC_GAIN_DB CONFIG_PTT_MIC_GAIN_DB
#else
#define MIC_GAIN_DB 30
#endif

/* ES8311 (speaker, standard I2S) and ES7210 (4-slot TDM mics) share one I2S port. */
static esp_err_t init_codec(const board_t *b)
{
    const board_audio_t *a = &b->audio;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = DMA_DESC_NUM,
        .dma_frame_num = DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
        },
        .gpio_cfg = {
            .mclk = a->mclk,
            .bclk = a->bclk,
            .ws = a->ws,
            .dout = a->dout,
            .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = a->mclk,
            .bclk = a->bclk,
            .ws = a->ws,
            .dout = I2S_GPIO_UNUSED,
            .din = a->din,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx, &tdm_cfg), TAG, "rx tdm");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");

    audio_codec_i2s_cfg_t i2s_if_cfg = {.port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_if_cfg = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = b->i2c};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    i2c_if_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    if (!data_if || !gpio_if || !out_ctrl || !in_ctrl) {
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = out_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, /* driven by audio_io_speaker_enable() */
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&es8311_cfg);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *in_codec = es7210_codec_new(&es7210_cfg);
    if (!out_codec || !in_codec) {
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if};
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = in_codec, .data_if = data_if};
    s_out_dev = esp_codec_dev_new(&out_cfg);
    s_in_dev = esp_codec_dev_new(&in_cfg);
    if (!s_out_dev || !s_in_dev) {
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_out_dev, &out_fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open out");
    esp_codec_dev_set_out_vol(s_out_dev, s_volume);

    /* Four TDM slots arrive; keep MIC1 only so reads return mono samples. */
    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_in_dev, &in_fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open in");
    esp_codec_dev_set_in_channel_gain(s_in_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)MIC_GAIN_DB);
    return ESP_OK;
}

/* Plain I2S amplifier on port 0 and I2S microphone on port 1, 32-bit mono left slot. */
static esp_err_t init_plain_i2s(const board_t *b)
{
    const board_audio_t *a = &b->audio;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = DMA_DESC_NUM,
        .dma_frame_num = DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "spk channel");
    chan_cfg.id = I2S_NUM_1;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "mic channel");

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = a->bclk,
            .ws = a->ws,
            .dout = a->dout,
            .din = I2S_GPIO_UNUSED,
        },
    };
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; /* mic L/R pin tied low */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &cfg), TAG, "spk std");

    cfg.gpio_cfg.bclk = a->mic_sck;
    cfg.gpio_cfg.ws = a->mic_ws;
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = a->mic_din;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &cfg), TAG, "mic std");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "spk enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "mic enable");
    return ESP_OK;
}

esp_err_t audio_io_init(const board_t *b)
{
    s_codec = b->audio.codec;
    s_pa = b->audio.pa;
    if (s_pa != GPIO_NUM_NC) {
        gpio_config_t io = {.pin_bit_mask = 1ULL << s_pa, .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "pa gpio");
        gpio_set_level(s_pa, 0);
    }
    esp_err_t err = s_codec ? init_codec(b) : init_plain_i2s(b);
    ESP_LOGI(TAG, "%s audio %s", s_codec ? "ES8311/ES7210" : "plain I2S", err == ESP_OK ? "ready" : "FAILED");
    return err;
}

int audio_io_read(int16_t *pcm, int samples)
{
    if (samples > AUDIO_FRAME_SAMPLES) {
        samples = AUDIO_FRAME_SAMPLES;
    }
    if (s_codec) {
        if (esp_codec_dev_read(s_in_dev, pcm, samples * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return 0;
        }
        return samples;
    }
    size_t got = 0;
    if (i2s_channel_read(s_rx, s_i2s_buf, samples * sizeof(int32_t), &got, 200) != ESP_OK) {
        return 0;
    }
    int n = got / sizeof(int32_t);
    for (int i = 0; i < n; i++) {
        int32_t v = s_i2s_buf[i] >> MIC_SHIFT;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
    }
    return n;
}

int audio_io_write(const int16_t *pcm, int samples)
{
    if (samples > AUDIO_FRAME_SAMPLES) {
        samples = AUDIO_FRAME_SAMPLES;
    }
    if (s_codec) {
        if (esp_codec_dev_write(s_out_dev, (void *)pcm, samples * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return 0;
        }
        return samples;
    }
    /* Software volume, quadratic so the 0..100 scale sounds even. */
    int64_t factor = (int64_t)s_volume * s_volume * 65536 / 10000;
    for (int i = 0; i < samples; i++) {
        int64_t v = (int64_t)pcm[i] * factor;
        s_i2s_out[i] = (int32_t)(v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : v));
    }
    size_t written = 0;
    if (i2s_channel_write(s_tx, s_i2s_out, samples * sizeof(int32_t), &written, 200) != ESP_OK) {
        return 0;
    }
    return written / sizeof(int32_t);
}

void audio_io_speaker_enable(bool on)
{
    if (s_pa != GPIO_NUM_NC) {
        gpio_set_level(s_pa, on ? 1 : 0);
    }
}

void audio_io_set_volume(int volume)
{
    s_volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    if (s_codec && s_out_dev) {
        esp_codec_dev_set_out_vol(s_out_dev, s_volume);
    }
}
