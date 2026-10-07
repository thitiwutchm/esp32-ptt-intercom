/*
 * Board support: power rails, I2C, display, touch, backlight, battery and
 * button GPIOs. One implementation per board, chosen in menuconfig
 * (PTT Intercom -> Board).
 */
#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_MAX_BUTTONS 3

typedef enum {
    BOARD_BTN_PTT = 0,
    BOARD_BTN_VOL_UP,
    BOARD_BTN_VOL_DOWN,
} board_button_role_t;

typedef struct {
    gpio_num_t gpio;
    board_button_role_t role;
} board_button_t;

typedef struct {
    /* true: ES8311 (speaker) + ES7210 (mics) on one I2S bus, set up over I2C.
     * false: plain I2S amplifier on port 0 and plain I2S microphone on port 1. */
    bool codec;
    gpio_num_t mclk, bclk, ws, dout, din; /* codec bus, or the amplifier when !codec */
    gpio_num_t mic_sck, mic_ws, mic_din;  /* !codec only */
    gpio_num_t pa;                        /* amplifier enable (active high), or GPIO_NUM_NC */
} board_audio_t;

typedef struct {
    const char *name;
    int width;
    int height;
    bool round;      /* round panel: keep UI inside the circle */
    bool swap_bytes; /* RGB565 byte order for LVGL */

    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch; /* NULL when the board has no touch screen */
    gpio_num_t touch_int;         /* touch interrupt line (active low), or GPIO_NUM_NC */
    i2c_master_bus_handle_t i2c;  /* NULL when the board has no I2C devices */
    board_audio_t audio;

    board_button_t buttons[BOARD_MAX_BUTTONS]; /* active low */
    int button_count;
} board_t;

/* Bring up power, buses, display (blank) and touch. */
esp_err_t board_init(board_t *b);

void board_backlight_set(int percent);

/* 0..100, or -1 when unknown. */
int board_battery_percent(void);

const board_t *board_get(void);

#ifdef __cplusplus
}
#endif
