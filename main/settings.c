#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "ptt_proto.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NS "ptt"

_Static_assert(CFG_NAME_LEN == PTT_NAME_LEN, "device name must fit the HELLO packet");

/* menuconfig values are only the factory defaults; the phone setup page overrides them. */
#if CONFIG_PTT_SIP_ENABLE
#define DEF_SIP_ENABLED true
#define DEF_SIP_SERVER CONFIG_PTT_SIP_SERVER
#define DEF_SIP_PORT CONFIG_PTT_SIP_PORT
#define DEF_SIP_USER CONFIG_PTT_SIP_USER
#define DEF_SIP_PASS CONFIG_PTT_SIP_PASSWORD
#define DEF_SIP_DISPLAY CONFIG_PTT_SIP_DISPLAY
#define DEF_SIP_TARGET CONFIG_PTT_SIP_CALL_TARGET
#if CONFIG_PTT_SIP_AUTO_ANSWER
#define DEF_SIP_AUTO true
#else
#define DEF_SIP_AUTO false
#endif
#else
#define DEF_SIP_ENABLED false
#define DEF_SIP_SERVER ""
#define DEF_SIP_PORT 5060
#define DEF_SIP_USER ""
#define DEF_SIP_PASS ""
#define DEF_SIP_DISPLAY ""
#define DEF_SIP_TARGET ""
#define DEF_SIP_AUTO false
#endif

/* Copy a stored string into out if it fits; otherwise leave out as it is. */
static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    char tmp[128];
    size_t len = sizeof(tmp);
    if (nvs_get_str(h, key, tmp, &len) == ESP_OK && strlen(tmp) < cap) {
        memcpy(out, tmp, strlen(tmp) + 1);
    }
}

void settings_load(settings_t *s, uint32_t device_id)
{
    memset(s, 0, sizeof(*s));
    s->channel = CONFIG_PTT_DEFAULT_CHANNEL;
    s->volume = CONFIG_PTT_DEFAULT_VOLUME;
    snprintf(s->name, sizeof(s->name), "PTT-%04X", (unsigned)(device_id & 0xFFFF));
    snprintf(s->wifi_ssid, sizeof(s->wifi_ssid), "%s", CONFIG_PTT_WIFI_SSID);
    snprintf(s->wifi_pass, sizeof(s->wifi_pass), "%s", CONFIG_PTT_WIFI_PASSWORD);
    s->sip_enabled = DEF_SIP_ENABLED;
    snprintf(s->sip_server, sizeof(s->sip_server), "%s", DEF_SIP_SERVER);
    s->sip_port = DEF_SIP_PORT;
    snprintf(s->sip_user, sizeof(s->sip_user), "%s", DEF_SIP_USER);
    snprintf(s->sip_pass, sizeof(s->sip_pass), "%s", DEF_SIP_PASS);
    snprintf(s->sip_display, sizeof(s->sip_display), "%s", DEF_SIP_DISPLAY);
    snprintf(s->sip_target, sizeof(s->sip_target), "%s", DEF_SIP_TARGET);
    s->sip_auto_answer = DEF_SIP_AUTO;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* first boot */
    }
    uint8_t v;
    uint16_t p;
    if (nvs_get_u8(h, "channel", &v) == ESP_OK && v >= 1 && v <= PTT_MAX_CHANNEL) {
        s->channel = v;
    }
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) {
        s->volume = v;
    }
    char name[sizeof(s->name)] = "";
    get_str(h, "name", name, sizeof(name));
    if (name[0]) {
        memcpy(s->name, name, sizeof(name));
    }
    /* Wi-Fi and SIP are only in NVS once saved from the phone; then they replace menuconfig. */
    if (nvs_get_u8(h, "configured", &v) == ESP_OK && v) {
        get_str(h, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
        get_str(h, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass));
        if (nvs_get_u8(h, "sip_en", &v) == ESP_OK) {
            s->sip_enabled = v;
        }
        get_str(h, "sip_server", s->sip_server, sizeof(s->sip_server));
        if (nvs_get_u16(h, "sip_port", &p) == ESP_OK && p) {
            s->sip_port = p;
        }
        get_str(h, "sip_user", s->sip_user, sizeof(s->sip_user));
        get_str(h, "sip_pass", s->sip_pass, sizeof(s->sip_pass));
        get_str(h, "sip_display", s->sip_display, sizeof(s->sip_display));
        get_str(h, "sip_target", s->sip_target, sizeof(s->sip_target));
        if (nvs_get_u8(h, "sip_auto", &v) == ESP_OK) {
            s->sip_auto_answer = v;
        }
    }
    nvs_close(h);
}

static void save(const settings_t *s, bool all)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed");
        return;
    }
    nvs_set_u8(h, "channel", s->channel);
    nvs_set_u8(h, "volume", s->volume);
    nvs_set_str(h, "name", s->name);
    if (!all) {
        nvs_commit(h);
        nvs_close(h);
        return;
    }
    nvs_set_u8(h, "configured", 1);
    nvs_set_str(h, "wifi_ssid", s->wifi_ssid);
    nvs_set_str(h, "wifi_pass", s->wifi_pass);
    nvs_set_u8(h, "sip_en", s->sip_enabled);
    nvs_set_str(h, "sip_server", s->sip_server);
    nvs_set_u16(h, "sip_port", s->sip_port);
    nvs_set_str(h, "sip_user", s->sip_user);
    nvs_set_str(h, "sip_pass", s->sip_pass);
    nvs_set_str(h, "sip_display", s->sip_display);
    nvs_set_str(h, "sip_target", s->sip_target);
    nvs_set_u8(h, "sip_auto", s->sip_auto_answer);
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs commit failed");
    }
    nvs_close(h);
}

void settings_save(const settings_t *s)
{
    save(s, false);
}

void settings_save_all(const settings_t *s)
{
    save(s, true);
}
