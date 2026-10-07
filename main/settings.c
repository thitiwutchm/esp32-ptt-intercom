#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NS "ptt"

void settings_load(settings_t *s, uint32_t device_id)
{
    s->channel = CONFIG_PTT_DEFAULT_CHANNEL;
    s->volume = CONFIG_PTT_DEFAULT_VOLUME;
    snprintf(s->name, sizeof(s->name), "PTT-%04X", (unsigned)(device_id & 0xFFFF));

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* first boot */
    }
    uint8_t v;
    if (nvs_get_u8(h, "channel", &v) == ESP_OK && v >= 1 && v <= PTT_MAX_CHANNEL) {
        s->channel = v;
    }
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) {
        s->volume = v;
    }
    size_t len = sizeof(s->name);
    char name[sizeof(s->name)];
    if (nvs_get_str(h, "name", name, &len) == ESP_OK && name[0]) {
        memcpy(s->name, name, sizeof(s->name));
    }
    nvs_close(h);
}

void settings_save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed");
        return;
    }
    nvs_set_u8(h, "channel", s->channel);
    nvs_set_u8(h, "volume", s->volume);
    nvs_set_str(h, "name", s->name);
    nvs_commit(h);
    nvs_close(h);
}
