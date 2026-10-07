/*
 * EchoEar / ESP-VoCat: ESP32-S3, 1.85" round 360x360 ST77916 (QSPI) with
 * CST816S touch, ES8311 speaker codec + ES7210 dual-mic ADC, NS4150B amp,
 * BQ27220 fuel gauge.
 *
 * Pins and the V1.0 / V1.2 PCB detection follow xiaozhi-esp32
 * main/boards/espressif/esp-vocat (MIT License).
 */
#include "sdkconfig.h"

#if CONFIG_PTT_BOARD_ECHOEAR

#include "board.h"
#include "board_common.h"
#include "echoear_lcd_init.h"

#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "echoear";

#define PIN_I2C_SDA GPIO_NUM_2
#define PIN_I2C_SCL GPIO_NUM_1
#define PIN_CODEC_POWER GPIO_NUM_48 /* V1.2: gates the codec supply rail */
#define PIN_POWER_CTRL GPIO_NUM_9

#define PIN_I2S_MCLK GPIO_NUM_42
#define PIN_I2S_WS GPIO_NUM_39
#define PIN_I2S_BCLK GPIO_NUM_40
#define PIN_I2S_DOUT GPIO_NUM_41
#define PIN_I2S_DIN_V10 GPIO_NUM_15
#define PIN_I2S_DIN_V12 GPIO_NUM_3
#define PIN_PA_V10 GPIO_NUM_4
#define PIN_PA_V12 GPIO_NUM_15

#define LCD_HOST SPI2_HOST
#define LCD_W 360
#define LCD_H 360
#define PIN_LCD_PCLK GPIO_NUM_18
#define PIN_LCD_CS GPIO_NUM_14
#define PIN_LCD_D0 GPIO_NUM_46
#define PIN_LCD_D1 GPIO_NUM_13
#define PIN_LCD_D2 GPIO_NUM_11
#define PIN_LCD_D3 GPIO_NUM_12
#define PIN_LCD_RST_V10 GPIO_NUM_3
#define PIN_LCD_RST_V12 GPIO_NUM_47
#define PIN_LCD_BL GPIO_NUM_44

#define PIN_TP_INT GPIO_NUM_10

#define ES8311_ADDR_7BIT 0x18
#define CST816_ADDR_7BIT 0x15
#define BQ27220_ADDR_7BIT 0x55
#define BQ27220_REG_SOC 0x2C

static board_t *s_board;
static i2c_master_dev_handle_t s_gauge;

/*
 * The PCB version is cached in RTC memory that survives every reset except a
 * power cycle: after a soft reset the V1.2 codec rail keeps residual charge
 * and the probe below would misdetect the board.
 */
#define PCB_VERSION_MAGIC 0x31434256u
static RTC_NOINIT_ATTR uint32_t s_pcb_magic;
static RTC_NOINIT_ATTR uint8_t s_pcb_version;

/* Returns 0 for V1.0, 1 for V1.2. */
static int detect_pcb_version(i2c_master_bus_handle_t bus)
{
    esp_reset_reason_t reason = esp_reset_reason();
    /* Set the latch before switching to output: a glitch to 0 cuts the V1.2 codec rail. */
    gpio_set_level(PIN_CODEC_POWER, reason != ESP_RST_POWERON ? 1 : 0);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_CODEC_POWER,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    if (reason != ESP_RST_POWERON && s_pcb_magic == PCB_VERSION_MAGIC && s_pcb_version <= 1) {
        if (s_pcb_version == 1) {
            gpio_set_level(PIN_CODEC_POWER, 1);
            for (int i = 0; i < 10 && i2c_master_probe(bus, CST816_ADDR_7BIT, 50) != ESP_OK; i++) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        ESP_LOGI(TAG, "PCB V1.%d (cached)", s_pcb_version ? 2 : 0);
        return s_pcb_version;
    }

    gpio_set_level(PIN_CODEC_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (i2c_master_probe(bus, ES8311_ADDR_7BIT, 100) == ESP_OK) {
        /* Codec answers with GPIO48 low: its supply is not gated, so V1.0. */
        ESP_LOGI(TAG, "PCB V1.0");
        s_pcb_version = 0;
    } else {
        gpio_set_level(PIN_CODEC_POWER, 1);
        bool alive = false;
        for (int i = 0; i < 10 && !alive; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
            alive = i2c_master_probe(bus, ES8311_ADDR_7BIT, 100) == ESP_OK;
        }
        if (!alive) {
            /* Leave GPIO48 high: harmless on V1.0, keeps the codec powered on V1.2. */
            ESP_LOGE(TAG, "PCB version detection failed, assuming V1.0 pins");
            return 0;
        }
        ESP_LOGI(TAG, "PCB V1.2");
        s_pcb_version = 1;
    }
    s_pcb_magic = PCB_VERSION_MAGIC;
    return s_pcb_version;
}

static esp_err_t init_display(board_t *b, int pcb)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_PCLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_W * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &b->panel_io),
                        TAG, "panel io");

    st77916_vendor_config_t vendor = {
        .init_cmds = echoear_lcd_init_cmds,
        .init_cmds_size = sizeof(echoear_lcd_init_cmds) / sizeof(echoear_lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = pcb ? PIN_LCD_RST_V12 : PIN_LCD_RST_V10,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .flags.reset_active_high = pcb ? 1 : 0,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(b->panel_io, &panel_cfg, &b->panel), TAG, "st77916");
    esp_lcd_panel_reset(b->panel);
    esp_lcd_panel_init(b->panel);
    esp_lcd_panel_disp_on_off(b->panel, true);
    return ESP_OK;
}

static esp_err_t init_touch(board_t *b)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(b->i2c, &tp_io_cfg, &tp_io), TAG, "touch io");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC, /* the UI watches PIN_TP_INT itself, see ui.c */
    };
    b->touch_int = PIN_TP_INT;
    return esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &b->touch);
}

esp_err_t board_init(board_t *b)
{
    s_board = b;
    *b = (board_t){
        .name = "EchoEar",
        .width = LCD_W,
        .height = LCD_H,
        .round = true,
        .swap_bytes = true,
        .touch_int = GPIO_NUM_NC,
        .buttons = {{.gpio = GPIO_NUM_0, .role = BOARD_BTN_PTT}},
        .button_count = 1,
    };

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &b->i2c), TAG, "i2c");

    int pcb = detect_pcb_version(b->i2c);

    gpio_config_t pwr = {.pin_bit_mask = 1ULL << PIN_POWER_CTRL, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&pwr));
    gpio_set_level(PIN_POWER_CTRL, 0);

    b->audio = (board_audio_t){
        .codec = true,
        .mclk = PIN_I2S_MCLK,
        .bclk = PIN_I2S_BCLK,
        .ws = PIN_I2S_WS,
        .dout = PIN_I2S_DOUT,
        .din = pcb ? PIN_I2S_DIN_V12 : PIN_I2S_DIN_V10,
        .mic_sck = GPIO_NUM_NC,
        .mic_ws = GPIO_NUM_NC,
        .mic_din = GPIO_NUM_NC,
        .pa = pcb ? PIN_PA_V12 : PIN_PA_V10,
    };

    board_backlight_init(PIN_LCD_BL);
    ESP_RETURN_ON_ERROR(init_display(b, pcb), TAG, "display");

    if (init_touch(b) != ESP_OK) {
        ESP_LOGW(TAG, "touch not available, use the BOOT button to talk");
        b->touch = NULL;
        b->touch_int = GPIO_NUM_NC;
    }

    i2c_device_config_t gauge = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ27220_ADDR_7BIT,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_probe(b->i2c, BQ27220_ADDR_7BIT, 50) != ESP_OK ||
        i2c_master_bus_add_device(b->i2c, &gauge, &s_gauge) != ESP_OK) {
        ESP_LOGW(TAG, "no fuel gauge");
        s_gauge = NULL;
    }
    return ESP_OK;
}

int board_battery_percent(void)
{
    if (!s_gauge) {
        return -1;
    }
    uint8_t reg = BQ27220_REG_SOC;
    uint8_t data[2];
    if (i2c_master_transmit_receive(s_gauge, &reg, 1, data, 2, 50) != ESP_OK) {
        return -1;
    }
    int soc = data[0] | (data[1] << 8);
    return soc > 100 ? 100 : soc;
}

const board_t *board_get(void)
{
    return s_board;
}

#endif /* CONFIG_PTT_BOARD_ECHOEAR */
