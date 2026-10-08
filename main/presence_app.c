#include "presence_app.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "presence.h"
#include "settings.h"
#include "setup_portal.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "presence_app";

#define TICK_MS 50
#define NOTICE_MS 2500
#define DIM_AFTER_MS 30000
#define BACKLIGHT_ON 100
#define BACKLIGHT_DIM 15

/* App events share the UI event queue, with values past the ui_event_t range. */
#define APP_EV_BOOT_CLICK 100
#define APP_EV_BOOT_VERY_LONG 104
#define APP_EV_SETUP_SAVED 105
#define APP_EV_SETUP_CANCEL 106

#define SETUP_AUTO_AFTER_MS 90000     /* never connected this long after boot: open setup */
#define SETUP_AUTO_BAD_TRIES 3        /* ...or sooner after this many wrong-password failures */
#define SETUP_TIMEOUT_MS (15 * 60000) /* close an unused setup network */
#define REBOOT_DELAY_MS 1500

typedef struct {
    SemaphoreHandle_t lock;
    settings_t cfg;
    uint32_t my_id;
    bool wifi_up;
    int battery;

    bool setup_active;
    char setup_ssid[33];
    char setup_pass[17];
    uint32_t setup_until_ms;
    bool setup_auto_done;
    uint32_t setup_retry_ms;
    uint32_t reboot_at_ms; /* 0 = none */

    char notice[32];
    bool notice_warn;
    uint32_t notice_until_ms;
    uint32_t last_activity_ms;
    bool dimmed;
} app_t;

static app_t s;
static device_config_t s_saved_cfg; /* from the setup page, applied by the app task */
static QueueHandle_t s_events;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void post_app_event(int ev)
{
    ui_event_t e = (ui_event_t)ev;
    xQueueSend(s_events, &e, 0);
}

/* UI (touch) events arrive with the ui_event_t signature; same queue. */
static void post_ui_event(ui_event_t ev)
{
    xQueueSend(s_events, &ev, 0);
}

/* ------------------------------------------------------------------ helpers (call with lock held) */

static void set_notice_locked(const char *text, bool warn, uint32_t now)
{
    strlcpy(s.notice, text, sizeof(s.notice));
    s.notice_warn = warn;
    s.notice_until_ms = now + NOTICE_MS;
}

/* Short English reason why Wi-Fi does not connect (the screen font has no Thai), or "" while trying. */
static void wifi_error_text(char *out, size_t cap)
{
    int reason, failures;
    wifi_err_t e = wifi_last_error(&reason, &failures);
    out[0] = '\0';
    if (failures < 2) {
        return;
    }
    switch (e) {
    case WIFI_ERR_NONE:
        break;
    case WIFI_ERR_NOT_FOUND:
        snprintf(out, cap, "Wi-Fi not found (2.4 GHz?)");
        break;
    case WIFI_ERR_PASSWORD:
        snprintf(out, cap, "Wrong Wi-Fi password?");
        break;
    case WIFI_ERR_SECURITY:
        snprintf(out, cap, "Wi-Fi security not supported");
        break;
    case WIFI_ERR_OTHER:
        snprintf(out, cap, "Wi-Fi failed (reason %d)", reason);
        break;
    }
}

static void build_view_locked(ui_view_t *v, uint32_t now)
{
    memset(v, 0, sizeof(*v));
    v->sip = -1; /* no SIP in this build: hide the call indicator */
    v->channel = s.cfg.channel;
    v->battery = s.battery;
    strlcpy(v->name, s.cfg.name, sizeof(v->name));
    strlcpy(v->wifi_ssid, s.cfg.wifi_ssid, sizeof(v->wifi_ssid));
    wifi_error_text(v->wifi_err, sizeof(v->wifi_err));

    if (s.setup_active) {
        v->mode = UI_MODE_SETUP;
        strlcpy(v->setup_ssid, s.setup_ssid, sizeof(v->setup_ssid));
        strlcpy(v->setup_pass, s.setup_pass, sizeof(v->setup_pass));
        uint32_t ip, bcast;
        if (wifi_get_addresses(&ip, &bcast)) {
            snprintf(v->setup_lan_ip, sizeof(v->setup_lan_ip), "%u.%u.%u.%u", (unsigned)(ip & 0xFF),
                     (unsigned)((ip >> 8) & 0xFF), (unsigned)((ip >> 16) & 0xFF), (unsigned)(ip >> 24));
        }
    } else if (!s.wifi_up) {
        v->mode = UI_MODE_WIFI;
        v->wifi_unset = s.cfg.wifi_ssid[0] == '\0';
    } else {
        v->mode = UI_MODE_PRESENCE;
        v->online = presence_present_count();
    }

    uint32_t my_ip, my_bcast;
    if (wifi_get_addresses(&my_ip, &my_bcast)) {
        snprintf(v->ip, sizeof(v->ip), "%u.%u.%u.%u", (unsigned)(my_ip & 0xFF), (unsigned)((my_ip >> 8) & 0xFF),
                 (unsigned)((my_ip >> 16) & 0xFF), (unsigned)(my_ip >> 24));
    }
    if (s.notice[0] && (int32_t)(s.notice_until_ms - now) > 0) {
        strlcpy(v->notice, s.notice, sizeof(v->notice));
        v->notice_warn = s.notice_warn;
    } else {
        s.notice[0] = '\0';
    }
}

static void update_ui(void)
{
    ui_view_t v;
    uint32_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    build_view_locked(&v, now);
    xSemaphoreGive(s.lock);
    ui_update(&v);
}

/* ------------------------------------------------------------------ callbacks */

static void on_button(board_button_role_t role, button_event_t ev)
{
    if (role != BOARD_BTN_PTT) {
        return; /* volume buttons unused in this build */
    }
    if (ev == BUTTON_VERY_LONG) {
        post_app_event(APP_EV_BOOT_VERY_LONG);
    } else if (ev == BUTTON_CLICK) {
        post_app_event(APP_EV_BOOT_CLICK);
    }
}

static void on_wifi(bool connected, uint32_t ip)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.wifi_up = connected;
    xSemaphoreGive(s.lock);
    if (connected) {
        presence_network_up(ip);
    } else {
        presence_network_down();
    }
    update_ui();
}

static void on_presence_notice(const char *text, bool warn)
{
    uint32_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    set_notice_locked(text, warn, now);
    s.last_activity_ms = now;
    xSemaphoreGive(s.lock);
    update_ui();
}

static void on_setup_saved(const device_config_t *cfg)
{
    s_saved_cfg = *cfg;
    post_app_event(APP_EV_SETUP_SAVED);
}

static void on_setup_cancelled(void)
{
    post_app_event(APP_EV_SETUP_CANCEL);
}

/* ------------------------------------------------------------------ phone setup (app task, no lock) */

static void setup_enter(uint32_t now)
{
    if (setup_portal_active()) {
        return;
    }
    presence_web_pause(); /* the setup portal needs port 80 */
    char ssid[33], pass[17];
    snprintf(ssid, sizeof(ssid), "CUBE-%04X-Setup", (unsigned)(s.my_id & 0xFFFF));
    snprintf(pass, sizeof(pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    xSemaphoreTake(s.lock, portMAX_DELAY);
    device_config_t cfg = s.cfg;
    xSemaphoreGive(s.lock);
    const setup_portal_cb_t cb = {.saved = on_setup_saved, .cancelled = on_setup_cancelled};
    esp_err_t err = setup_portal_start(&cfg, ssid, pass, &cb);
    if (err != ESP_OK) {
        char text[32];
        snprintf(text, sizeof(text), "Setup error %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", text);
        xSemaphoreTake(s.lock, portMAX_DELAY);
        set_notice_locked(text, true, now);
        s.notice_until_ms = now + 5000;
        s.setup_auto_done = false;
        s.setup_retry_ms = now + 5000;
        xSemaphoreGive(s.lock);
        presence_web_resume();
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.setup_active = true;
    strlcpy(s.setup_ssid, ssid, sizeof(s.setup_ssid));
    strlcpy(s.setup_pass, pass, sizeof(s.setup_pass));
    s.setup_until_ms = now + SETUP_TIMEOUT_MS;
    s.last_activity_ms = now;
    xSemaphoreGive(s.lock);
}

static void setup_leave(void)
{
    setup_portal_stop();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.setup_active = false;
    xSemaphoreGive(s.lock);
    presence_web_resume();
}

/* ------------------------------------------------------------------ input (app task) */

static void handle_input(ui_event_t ev, uint32_t now)
{
    enum { ACT_NONE, ACT_SETUP_ENTER, ACT_SETUP_LEAVE, ACT_ENROLL } action = ACT_NONE;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.last_activity_ms = now;
    /* In setup mode BOOT (or the screen X) leaves it, if there is a network to go back to. */
    if (s.setup_active && ((int)ev == APP_EV_BOOT_CLICK || (int)ev == UI_EV_SETUP_EXIT ||
                           (int)ev == APP_EV_SETUP_CANCEL)) {
        if (s.cfg.wifi_ssid[0]) {
            action = ACT_SETUP_LEAVE;
        } else {
            set_notice_locked("Set up Wi-Fi first", true, now);
        }
        ev = (ui_event_t)-1;
    }
    switch ((int)ev) {
    case APP_EV_BOOT_VERY_LONG:
        action = ACT_SETUP_ENTER;
        break;
    case APP_EV_BOOT_CLICK:
        action = ACT_ENROLL; /* open the pairing window */
        break;
    case APP_EV_SETUP_SAVED:
        settings_save_all(&s_saved_cfg);
        set_notice_locked("Saved - restarting", false, now);
        s.notice_until_ms = now + REBOOT_DELAY_MS + 1000;
        s.reboot_at_ms = now + REBOOT_DELAY_MS;
        break;
    }
    xSemaphoreGive(s.lock);

    switch (action) {
    case ACT_SETUP_ENTER:
        setup_enter(now);
        break;
    case ACT_SETUP_LEAVE:
        setup_leave();
        break;
    case ACT_ENROLL:
        presence_enroll_begin(); /* its own on-screen notice follows */
        break;
    case ACT_NONE:
        break;
    }
    update_ui();
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_tick = 0;

    for (;;) {
        ui_event_t ev;
        uint32_t now = now_ms();
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            handle_input(ev, now_ms());
            continue;
        }

        /* Once a second: refresh the screen (present count, battery, notice expiry)
         * and drive setup auto-open / timeout and the pending reboot. */
        if (now - last_tick < 1000) {
            continue;
        }
        last_tick = now;

        xSemaphoreTake(s.lock, portMAX_DELAY);
        s.battery = -1; /* CUBE has no battery gauge; keep it hidden */
        bool setup_on = s.setup_active;
        bool have_wifi = s.cfg.wifi_ssid[0] != '\0';
        int failures;
        wifi_err_t werr = wifi_last_error(NULL, &failures);
        bool hopeless = (werr == WIFI_ERR_PASSWORD || werr == WIFI_ERR_SECURITY) && failures >= SETUP_AUTO_BAD_TRIES;
        bool want_setup = !setup_on && !s.setup_auto_done && (int32_t)(now - s.setup_retry_ms) >= 0 &&
                          (!have_wifi || (!wifi_ever_connected() && (hopeless || now > SETUP_AUTO_AFTER_MS)));
        bool setup_expired = setup_on && have_wifi && (int32_t)(now - s.setup_until_ms) > 0;
        uint32_t reboot_at = s.reboot_at_ms;
        bool active = setup_on || (now - s.last_activity_ms) < DIM_AFTER_MS;
        if (want_setup) {
            s.setup_auto_done = true;
        }
        xSemaphoreGive(s.lock);

        if (reboot_at && (int32_t)(now - reboot_at) >= 0) {
            update_ui();
            ESP_LOGI(TAG, "restarting with the new settings");
            esp_restart();
        }
        if (want_setup) {
            ESP_LOGW(TAG, "%s", have_wifi ? "Wi-Fi not reachable: opening phone setup" : "no Wi-Fi yet: opening phone setup");
            setup_enter(now);
        } else if (setup_expired) {
            setup_leave();
        }
        if (active == s.dimmed) {
            s.dimmed = !active;
            board_backlight_set(active ? BACKLIGHT_ON : BACKLIGHT_DIM);
        }
        update_ui();
    }
}

/* ------------------------------------------------------------------ start */

esp_err_t presence_app_start(const board_t *b)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s.lock = xSemaphoreCreateMutex();
    s.my_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    s.battery = -1;
    settings_load(&s.cfg, s.my_id);
    s.last_activity_ms = now_ms();
    s_events = xQueueCreate(16, sizeof(ui_event_t));
    ESP_LOGI(TAG, "BLE-only device %08lx \"%s\"", (unsigned long)s.my_id, s.cfg.name);

    ESP_ERROR_CHECK(ui_init(b, post_ui_event));
    board_backlight_set(BACKLIGHT_ON);
    update_ui();

    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%04X", (unsigned)(s.my_id & 0xFFFF));
    const presence_cb_t pcb = {.notice = on_presence_notice};
    if (presence_start(suffix, &pcb) != ESP_OK) {
        ESP_LOGW(TAG, "presence logger unavailable");
    }

    char hostname[sizeof(s.cfg.name)];
    for (size_t i = 0; i < sizeof(hostname); i++) {
        char c = s.cfg.name[i];
        hostname[i] = (c == '\0' || isalnum((unsigned char)c)) ? c : '-';
    }
    ESP_ERROR_CHECK(wifi_start(hostname, s.cfg.wifi_ssid, s.cfg.wifi_pass, on_wifi));

    xTaskCreatePinnedToCore(app_task, "presence_app", 4096, NULL, 5, NULL, 0);
    buttons_start(b, on_button);
    return ESP_OK;
}
