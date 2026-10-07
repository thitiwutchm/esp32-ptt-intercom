#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static wifi_status_cb_t s_cb;
static volatile bool s_connected;
static TimerHandle_t s_retry_timer;

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        bool was = s_connected;
        s_connected = false;
        if (was && s_cb) {
            s_cb(false, 0);
        }
        xTimerStart(s_retry_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        /* Power save delays incoming frames by up to a beacon interval: off for voice. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_connected = true;
        if (s_cb) {
            s_cb(true, e->ip_info.ip.addr);
        }
    }
}

esp_err_t wifi_start(const char *hostname, wifi_status_cb_t cb)
{
    s_cb = cb;
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(2000), pdFALSE, NULL, retry_cb);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_PTT_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_PTT_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(CONFIG_PTT_WIFI_PASSWORD) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_PTT_WIFI_SSID);
    return ESP_OK;
}

bool wifi_get_addresses(uint32_t *ip, uint32_t *broadcast)
{
    if (!s_connected) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    *ip = info.ip.addr;
    *broadcast = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr;
    return true;
}
