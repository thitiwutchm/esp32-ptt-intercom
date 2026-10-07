/*
 * Xingzhi CUBE 1.54" TFT Wi-Fi: ESP32-S3, 240x240 ST7789 (SPI), plain I2S
 * microphone and I2S amplifier, three buttons (BOOT, VOL+, VOL-), battery
 * read through ADC2 with a charge-detect pin.
 *
 * Pins follow xiaozhi-esp32 main/boards/nologo/xingzhi-cube-1.54tft-wifi
 * (MIT License).
 */
#include "sdkconfig.h"

#if CONFIG_PTT_BOARD_CUBE

#include "board.h"
#include "board_common.h"

#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "cube";

#define PIN_MIC_WS GPIO_NUM_4
#define PIN_MIC_SCK GPIO_NUM_5
#define PIN_MIC_DIN GPIO_NUM_6
#define PIN_SPK_DOUT GPIO_NUM_7
#define PIN_SPK_BCLK GPIO_NUM_15
#define PIN_SPK_LRCK GPIO_NUM_16

#define PIN_BOOT GPIO_NUM_0
#define PIN_VOL_UP GPIO_NUM_40
#define PIN_VOL_DOWN GPIO_NUM_39

#define LCD_HOST SPI3_HOST
#define LCD_W 240
#define LCD_H 240
#define PIN_LCD_MOSI GPIO_NUM_10
#define PIN_LCD_SCLK GPIO_NUM_9
#define PIN_LCD_DC GPIO_NUM_8
#define PIN_LCD_CS GPIO_NUM_14
#define PIN_LCD_RST GPIO_NUM_18
#define PIN_LCD_BL GPIO_NUM_13

#define PIN_POWER_HOLD GPIO_NUM_21 /* keeps the battery switch on; low = power off */
#define PIN_CHARGING GPIO_NUM_38
#define BATTERY_ADC_UNIT ADC_UNIT_2
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_6 /* GPIO17 */

static board_t *s_board;
static adc_oneshot_unit_handle_t s_adc;

static esp_err_t init_display(board_t *b)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &b->panel_io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(b->panel_io, &panel_cfg, &b->panel), TAG, "st7789");
    esp_lcd_panel_reset(b->panel);
    esp_lcd_panel_init(b->panel);
    esp_lcd_panel_invert_color(b->panel, true);
    esp_lcd_panel_disp_on_off(b->panel, true);
    return ESP_OK;
}

esp_err_t board_init(board_t *b)
{
    s_board = b;
    *b = (board_t){
        .name = "CUBE",
        .width = LCD_W,
        .height = LCD_H,
        .round = false,
        .swap_bytes = true,
        .touch_int = GPIO_NUM_NC,
        .buttons = {
            {.gpio = PIN_BOOT, .role = BOARD_BTN_PTT},
            {.gpio = PIN_VOL_UP, .role = BOARD_BTN_VOL_UP},
            {.gpio = PIN_VOL_DOWN, .role = BOARD_BTN_VOL_DOWN},
        },
        .button_count = 3,
        .audio = {
            .codec = false,
            .mclk = GPIO_NUM_NC,
            .bclk = PIN_SPK_BCLK,
            .ws = PIN_SPK_LRCK,
            .dout = PIN_SPK_DOUT,
            .din = GPIO_NUM_NC,
            .mic_sck = PIN_MIC_SCK,
            .mic_ws = PIN_MIC_WS,
            .mic_din = PIN_MIC_DIN,
            .pa = GPIO_NUM_NC,
        },
    };

    /* Hold the power switch on (the board powers off when this pin goes low). */
    rtc_gpio_init(PIN_POWER_HOLD);
    rtc_gpio_set_direction(PIN_POWER_HOLD, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(PIN_POWER_HOLD, 1);

    gpio_config_t chg = {.pin_bit_mask = 1ULL << PIN_CHARGING, .mode = GPIO_MODE_INPUT};
    gpio_config(&chg);

    board_backlight_init(PIN_LCD_BL);
    ESP_RETURN_ON_ERROR(init_display(b), TAG, "display");

    adc_oneshot_unit_init_cfg_t unit = {.unit_id = BATTERY_ADC_UNIT, .ulp_mode = ADC_ULP_MODE_DISABLE};
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK ||
        adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unavailable");
        s_adc = NULL;
    }
    return ESP_OK;
}

int board_battery_percent(void)
{
    if (!s_adc) {
        return -1;
    }
    /* Raw ADC calibration points from the xiaozhi CUBE power manager. */
    static const struct {
        int adc;
        int level;
    } table[] = {{1970, 0}, {2062, 20}, {2154, 40}, {2246, 60}, {2338, 80}, {2430, 100}};
    static int avg = -1;

    int raw;
    /* ADC2 shares hardware with Wi-Fi and can be busy: keep the last value. */
    if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
        avg = avg < 0 ? raw : (avg * 3 + raw) / 4;
    }
    if (avg < 0) {
        return -1;
    }
    const int n = sizeof(table) / sizeof(table[0]);
    if (avg <= table[0].adc) {
        return 0;
    }
    if (avg >= table[n - 1].adc) {
        return 100;
    }
    for (int i = 0; i < n - 1; i++) {
        if (avg < table[i + 1].adc) {
            return table[i].level + (avg - table[i].adc) * (table[i + 1].level - table[i].level) /
                                        (table[i + 1].adc - table[i].adc);
        }
    }
    return 100;
}

const board_t *board_get(void)
{
    return s_board;
}

#endif /* CONFIG_PTT_BOARD_CUBE */
