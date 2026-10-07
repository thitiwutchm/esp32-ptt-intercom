#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "ui";

#define COLOR_BG 0x0E1116
#define COLOR_RING_IDLE 0x37404C
#define COLOR_RING_WIFI 0x5A5F66
#define COLOR_BLUE 0x2D6CDF
#define COLOR_RED 0xE53935
#define COLOR_GREEN 0x2E9E4F
#define COLOR_ORANGE 0xF08C00
#define COLOR_TEXT 0xF2F4F7
#define COLOR_MUTED 0x9AA4B2

static ui_event_cb_t s_cb;
static bool s_touch;
static lv_obj_t *s_ring;
static lv_obj_t *s_channel;
static lv_obj_t *s_ptt;
static lv_obj_t *s_ptt_label;
static lv_obj_t *s_status;
static lv_obj_t *s_info;
static lv_obj_t *s_call_btn;
static lv_obj_t *s_call_btn_label;
static ui_mode_t s_mode; /* last shown, read by the touch handlers (LVGL task) */
static lv_obj_t *s_setup;  /* full-screen overlay for phone setup */
static lv_obj_t *s_setup_qr;
static lv_obj_t *s_setup_text;
static char s_setup_shown[96];

/*
 * Touch input. The CST816 sleeps when nobody touches it and then NACKs I2C,
 * and esp_lvgl_port aborts on any read error, so read it here instead: only
 * after its interrupt line fired or while a finger is down, and treat a
 * failed read as "released".
 */
#define TOUCH_IRQ_WINDOW_US 100000
static esp_lcd_touch_handle_t s_tp;
static gpio_num_t s_tp_int = GPIO_NUM_NC;
static volatile int64_t s_tp_irq_us;
static bool s_tp_down;

static void IRAM_ATTR touch_isr(void *arg)
{
    (void)arg;
    s_tp_irq_us = esp_timer_get_time();
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    bool irq = s_tp_int == GPIO_NUM_NC || esp_timer_get_time() - s_tp_irq_us < TOUCH_IRQ_WINDOW_US;
    if (!s_tp_down && !irq) {
        return;
    }
    esp_lcd_touch_point_data_t pt[1];
    uint8_t cnt = 0;
    s_tp_down = esp_lcd_touch_read_data(s_tp) == ESP_OK && esp_lcd_touch_get_data(s_tp, pt, &cnt, 1) == ESP_OK &&
                cnt > 0;
    if (s_tp_down) {
        data->point.x = pt[0].x;
        data->point.y = pt[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

static void add_touch(const board_t *b, lv_display_t *disp)
{
    s_tp = b->touch;
    s_tp_int = b->touch_int;
    if (s_tp_int != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_tp_int,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io);
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "isr service: %s, polling touch", esp_err_to_name(err));
            s_tp_int = GPIO_NUM_NC;
        } else {
            gpio_isr_handler_add(s_tp_int, touch_isr, NULL);
        }
    }
    /* A sleeping controller would log an error on every poll otherwise. */
    esp_log_level_set("CST816S", ESP_LOG_NONE);

    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, disp);
    lvgl_port_unlock();
}

static bool in_call_mode(void)
{
    return s_mode == UI_MODE_CALL_IN || s_mode == UI_MODE_CALL_OUT || s_mode == UI_MODE_CALL;
}

/* Centre button: hold to talk on the walkie-talkie; during a phone call a tap answers or hangs up. */
static void ptt_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (in_call_mode()) {
        if (code == LV_EVENT_CLICKED) {
            s_cb(UI_EV_CALL);
        }
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        s_cb(UI_EV_PTT_DOWN);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_cb(UI_EV_PTT_UP);
    }
}

static void click_event(lv_event_t *e)
{
    s_cb((ui_event_t)(intptr_t)lv_event_get_user_data(e));
}

/* Phone button: call when idle, decline while ringing, hang up otherwise. */
static void call_btn_event(lv_event_t *e)
{
    (void)e;
    s_cb(s_mode == UI_MODE_CALL_IN ? UI_EV_REJECT : UI_EV_CALL);
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *symbol, ui_event_t ev, int size)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_RING_IDLE), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* Bigger hit area than the drawing: fingers are larger than 40 px. */
    lv_obj_set_ext_click_area(btn, size / 3);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, click_event, LV_EVENT_CLICKED, (void *)(intptr_t)ev);
    return btn;
}

static void build_setup(const board_t *b)
{
    const int w = b->width;
    const int h = b->height;
    const bool big = w >= 300;
    s_setup = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_setup);
    lv_obj_set_size(s_setup, w, h);
    lv_obj_set_style_bg_color(s_setup, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_setup, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_setup);
    lv_obj_set_style_text_font(title, big ? &lv_font_montserrat_20 : &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Phone setup: scan to join");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, b->round ? h * 11 / 100 : 6);

    int qr = big ? 150 : 118;
    s_setup_qr = lv_qrcode_create(s_setup);
    lv_qrcode_set_size(s_setup_qr, qr);
    lv_qrcode_set_dark_color(s_setup_qr, lv_color_black());
    lv_qrcode_set_light_color(s_setup_qr, lv_color_white());
    /* A white border keeps the code readable on the dark background. */
    lv_obj_set_style_border_color(s_setup_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_setup_qr, 5, 0);
    lv_obj_align(s_setup_qr, LV_ALIGN_TOP_MID, 0, b->round ? h * 19 / 100 : 30);

    s_setup_text = lv_label_create(s_setup);
    lv_obj_set_style_text_font(s_setup_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_setup_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_setup_text, w * 80 / 100);
    lv_label_set_long_mode(s_setup_text, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(s_setup_text, s_setup_qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    if (s_touch) {
        lv_obj_t *x = small_button(s_setup, LV_SYMBOL_CLOSE, UI_EV_SETUP_EXIT, big ? 40 : 30);
        lv_obj_align(x, LV_ALIGN_BOTTOM_MID, 0, b->round ? -h * 5 / 100 : -4);
    }
    lv_obj_add_flag(s_setup, LV_OBJ_FLAG_HIDDEN);
}

static void build(const board_t *b)
{
    const int w = b->width;
    const int h = b->height;
    const bool big = w >= 300;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* State ring around the edge of the panel. */
    s_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, w, h);
    lv_obj_center(s_ring);
    lv_obj_set_style_radius(s_ring, b->round ? LV_RADIUS_CIRCLE : 18, 0);
    lv_obj_set_style_border_width(s_ring, big ? 12 : 8, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(COLOR_RING_WIFI), 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    /* Channel row near the top: [<]  CH 1  [>] centred as a flex row. */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, w / 14, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, b->round ? h * 10 / 100 : h * 4 / 100);
    int sz = big ? 44 : 32;
    if (s_touch) {
        small_button(row, LV_SYMBOL_LEFT, UI_EV_CH_DOWN, sz);
    }
    s_channel = lv_label_create(row);
    lv_obj_set_style_text_font(s_channel, big ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
    lv_label_set_text(s_channel, "CH -");
    if (s_touch) {
        small_button(row, LV_SYMBOL_RIGHT, UI_EV_CH_UP, sz);
    }

    /* Big round talk button. */
    int d = w * 46 / 100;
    s_ptt = lv_button_create(scr);
    lv_obj_set_size(s_ptt, d, d);
    lv_obj_align(s_ptt, LV_ALIGN_CENTER, 0, -h / 30);
    lv_obj_set_style_radius(s_ptt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ptt, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_shadow_width(s_ptt, 0, 0);
    s_ptt_label = lv_label_create(s_ptt);
    lv_obj_set_style_text_font(s_ptt_label, big ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
    lv_label_set_text(s_ptt_label, "PTT");
    lv_obj_center(s_ptt_label);
    if (s_touch) {
        lv_obj_add_event_cb(s_ptt, ptt_event, LV_EVENT_ALL, NULL);
    } else {
        lv_obj_clear_flag(s_ptt, LV_OBJ_FLAG_CLICKABLE);
    }

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, big ? &lv_font_montserrat_20 : &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status, w * 70 / 100);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, d / 2 + h / 14);

    s_info = lv_label_create(scr);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, b->round ? -h * 13 / 100 : -h * 5 / 100);

    if (s_touch) {
        sz = big ? 40 : 30;
        lv_obj_t *minus = small_button(scr, LV_SYMBOL_MINUS, UI_EV_VOL_DOWN, sz);
        lv_obj_align(minus, LV_ALIGN_BOTTOM_MID, -w * 22 / 100, b->round ? -h * 19 / 100 : -h * 12 / 100);
        lv_obj_t *plus = small_button(scr, LV_SYMBOL_PLUS, UI_EV_VOL_UP, sz);
        lv_obj_align(plus, LV_ALIGN_BOTTOM_MID, w * 22 / 100, b->round ? -h * 19 / 100 : -h * 12 / 100);

        s_call_btn = lv_button_create(scr);
        lv_obj_set_size(s_call_btn, sz + 8, sz + 8);
        lv_obj_set_style_radius(s_call_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(s_call_btn, 0, 0);
        lv_obj_set_ext_click_area(s_call_btn, sz / 3);
        lv_obj_align(s_call_btn, LV_ALIGN_BOTTOM_MID, 0, b->round ? -h * 18 / 100 : -h * 11 / 100);
        s_call_btn_label = lv_label_create(s_call_btn);
        lv_label_set_text(s_call_btn_label, LV_SYMBOL_CALL);
        lv_obj_center(s_call_btn_label);
        lv_obj_add_event_cb(s_call_btn, call_btn_event, LV_EVENT_CLICKED, NULL);
        lv_obj_add_flag(s_call_btn, LV_OBJ_FLAG_HIDDEN);
        /* Info line moves below the button row. */
        lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, b->round ? -h * 8 / 100 : -h * 3 / 100);
    }
}

esp_err_t ui_init(const board_t *b, ui_event_cb_t cb)
{
    s_cb = cb;
    s_touch = b->touch != NULL;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = b->panel_io,
        .panel_handle = b->panel,
        .buffer_size = b->width * 40,
        .double_buffer = true,
        .hres = b->width,
        .vres = b->height,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = b->swap_bytes,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add display");

    if (s_touch) {
        add_touch(b, disp);
    }

    lvgl_port_lock(0);
    build(b);
    build_setup(b);
    lvgl_port_unlock();
    return ESP_OK;
}

void ui_update(const ui_view_t *v)
{
    uint32_t ring = COLOR_RING_IDLE;
    uint32_t button = COLOR_BLUE;
    uint32_t call_btn = COLOR_GREEN;
    const char *center = "PTT";
    const char *status;
    char buf[64];

    switch (v->mode) {
    case UI_MODE_WIFI:
        ring = COLOR_RING_WIFI;
        button = COLOR_RING_IDLE;
        status = v->wifi_unset ? "No Wi-Fi: hold BOOT 8 s" : "Connecting Wi-Fi...";
        break;
    case UI_MODE_TX:
        ring = button = COLOR_RED;
        status = v->online ? "TALKING" : "TALKING - no one online";
        break;
    case UI_MODE_RX:
        ring = button = COLOR_GREEN;
        center = LV_SYMBOL_VOLUME_MAX;
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX " %s", v->talker);
        status = buf;
        break;
    case UI_MODE_CALL_IN:
        ring = COLOR_BLUE;
        button = COLOR_GREEN;
        call_btn = COLOR_RED;
        center = LV_SYMBOL_CALL;
        snprintf(buf, sizeof(buf), "%s calling", v->call_peer);
        status = buf;
        break;
    case UI_MODE_CALL_OUT:
        ring = COLOR_BLUE;
        button = call_btn = COLOR_RED;
        center = LV_SYMBOL_CLOSE;
        snprintf(buf, sizeof(buf), "%s %s...", v->call_ringing ? "Ringing" : "Calling", v->call_peer);
        status = buf;
        break;
    case UI_MODE_CALL:
        ring = COLOR_GREEN;
        button = call_btn = COLOR_RED;
        center = LV_SYMBOL_CLOSE;
        snprintf(buf, sizeof(buf), "%s  %d:%02d", v->call_peer, v->call_secs / 60, v->call_secs % 60);
        status = buf;
        break;
    case UI_MODE_IDLE:
    default:
        if (s_touch) {
            status = "Hold to talk";
        } else if (v->sip >= 0) {
            status = "BOOT: tap = call, hold = talk";
        } else {
            status = "Hold BOOT to talk";
        }
        break;
    }
    bool warn = v->notice[0] && v->notice_warn;
    if (v->notice[0]) {
        status = v->notice;
    }
    if (warn) {
        ring = COLOR_ORANGE;
    }

    char info[80];
    int n = snprintf(info, sizeof(info), "%s  |  %d online", v->name, v->online);
    if (v->battery >= 0) {
        n += snprintf(info + n, sizeof(info) - n, "  |  %d%%", v->battery);
    }
    if (v->sip >= 0) {
        snprintf(info + n, sizeof(info) - n, "  |  " LV_SYMBOL_CALL "%s", v->sip ? "" : " !");
    }

    if (!lvgl_port_lock(100)) {
        return;
    }
    s_mode = v->mode;
    if (v->mode == UI_MODE_SETUP) {
        /* Standard Wi-Fi QR: phone cameras offer to join the network. */
        char qr[96];
        snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", v->setup_ssid, v->setup_pass);
        if (strcmp(qr, s_setup_shown) != 0) {
            lv_qrcode_update(s_setup_qr, qr, strlen(qr));
            strlcpy(s_setup_shown, qr, sizeof(s_setup_shown));
        }
        char text[200];
        int n = snprintf(text, sizeof(text), "Wi-Fi %s\nPassword %s\nthen open http://192.168.4.1", v->setup_ssid,
                         v->setup_pass);
        if (v->setup_lan_ip[0]) {
            snprintf(text + n, sizeof(text) - n, "\nor http://%s", v->setup_lan_ip);
        }
        if (v->notice[0]) {
            snprintf(text, sizeof(text), "%s", v->notice);
        }
        lv_label_set_text(s_setup_text, text);
        lv_obj_clear_flag(s_setup, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
        return;
    }
    lv_obj_add_flag(s_setup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(ring), 0);
    lv_obj_set_style_bg_color(s_ptt, lv_color_hex(button), 0);
    lv_label_set_text(s_ptt_label, center);
    lv_label_set_text_fmt(s_channel, "CH %d", v->channel);
    lv_label_set_text(s_status, status);
    lv_obj_set_style_text_color(s_status, lv_color_hex(warn ? COLOR_ORANGE : COLOR_TEXT), 0);
    lv_label_set_text(s_info, info);
    if (s_call_btn) {
        if (v->sip < 0 || v->mode == UI_MODE_WIFI || v->mode == UI_MODE_TX || v->mode == UI_MODE_RX) {
            lv_obj_add_flag(s_call_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_call_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_call_btn, lv_color_hex(call_btn), 0);
            lv_label_set_text(s_call_btn_label, v->mode == UI_MODE_CALL_IN ? LV_SYMBOL_CLOSE : LV_SYMBOL_CALL);
        }
    }
    lvgl_port_unlock();
}
