#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "wifi";

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static wifi_status_cb_t s_cb;
static volatile bool s_connected;
static volatile bool s_ever_connected;
static volatile bool s_ap_on;
static bool s_have_ssid;
static TimerHandle_t s_retry_timer;
static SemaphoreHandle_t s_scan_lock;
static volatile int s_last_reason;
static volatile int s_failures;

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    /* While the setup AP is up and we are not connected, stop retrying: scans need an idle station. */
    if (s_have_ssid && !(s_ap_on && !s_connected)) {
        esp_wifi_connect();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_ssid) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        /* Our own disconnect (opening the setup network) says nothing about the network. */
        if (!(d->reason == WIFI_REASON_ASSOC_LEAVE && !s_connected)) {
            s_last_reason = d->reason;
            if (!s_connected) {
                s_failures++;
            }
        }
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
        s_ever_connected = true;
        s_last_reason = 0;
        s_failures = 0;
        if (s_cb) {
            s_cb(true, e->ip_info.ip.addr);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "phone joined the setup network");
    }
}

esp_err_t wifi_start(const char *hostname, const char *ssid, const char *password, wifi_status_cb_t cb)
{
    s_cb = cb;
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(2000), pdFALSE, NULL, retry_cb);
    s_scan_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials live in our NVS namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    s_have_ssid = ssid && ssid[0];
    if (s_have_ssid) {
        strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH; /* WPA3 routers that only do hash-to-element */
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (s_have_ssid) {
        ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi configured: use phone setup");
    }
    return ESP_OK;
}

bool wifi_get_addresses(uint32_t *ip, uint32_t *broadcast)
{
    if (!s_connected) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    *ip = info.ip.addr;
    *broadcast = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr;
    return true;
}

bool wifi_ever_connected(void)
{
    return s_ever_connected;
}

wifi_err_t wifi_last_error(int *reason, int *failures)
{
    int r = s_connected ? 0 : s_last_reason;
    if (reason) {
        *reason = r;
    }
    if (failures) {
        *failures = s_connected ? 0 : s_failures;
    }
    switch (r) {
    case 0:
        return WIFI_ERR_NONE;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WIFI_ERR_NOT_FOUND;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
        return WIFI_ERR_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_AKMP_INVALID:
    case WIFI_REASON_BAD_CIPHER_OR_AKM:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return WIFI_ERR_SECURITY;
    default:
        return WIFI_ERR_OTHER;
    }
}

esp_err_t wifi_ap_start(const char *ssid, const char *password)
{
    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
    }
    if (!s_connected) {
        esp_wifi_disconnect(); /* stop a pending connect so scans work */
    }
    s_ap_on = true;
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 3;
    ap.ap.channel = 1; /* follows the station's channel once it is connected */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    ESP_LOGI(TAG, "setup network \"%s\" %s", ssid, err == ESP_OK ? "up" : esp_err_to_name(err));
    return err;
}

void wifi_ap_stop(void)
{
    if (!s_ap_on) {
        return;
    }
    s_ap_on = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (s_have_ssid && !s_connected) {
        esp_wifi_connect();
    }
}

uint32_t wifi_ap_ip(void)
{
    esp_netif_ip_info_t info;
    if (s_ap && esp_netif_get_ip_info(s_ap, &info) == ESP_OK) {
        return info.ip.addr;
    }
    return 0;
}

int wifi_scan(wifi_scan_item_t *out, int max)
{
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    int n = 0;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > 40) {
            count = 40;
        }
        wifi_ap_record_t *recs = calloc(count ? count : 1, sizeof(*recs));
        if (recs && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
            /* Records come strongest first; keep the first of each SSID. */
            for (int i = 0; i < count && n < max; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (!ssid[0]) {
                    continue; /* hidden network */
                }
                bool dup = false;
                for (int k = 0; k < n && !dup; k++) {
                    dup = strcmp(out[k].ssid, ssid) == 0;
                }
                if (!dup) {
                    strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
                    out[n].rssi = recs[i].rssi;
                    out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                    n++;
                }
            }
        }
        free(recs);
    } else {
        ESP_LOGW(TAG, "scan failed");
    }
    xSemaphoreGive(s_scan_lock);
    return n;
}
