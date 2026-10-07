#include "setup_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "captive_dns.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi.h"

static const char *TAG = "setup";

extern const char setup_page_start[] asm("_binary_setup_page_html_start");
extern const char setup_page_end[] asm("_binary_setup_page_html_end");

static httpd_handle_t s_http;
static setup_portal_cb_t s_cb;
static device_config_t s_cfg; /* what the page shows and edits */
static SemaphoreHandle_t s_lock;
static volatile bool s_active;
static volatile bool s_dns_run;
static TaskHandle_t s_dns_task;

#define MAX_BODY 2048

/* ------------------------------------------------------------------ captive DNS */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS port 53 unavailable");
        if (sock >= 0) {
            close(sock);
        }
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t q[512], r[600];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        size_t rn = captive_dns_reply(q, (size_t)n, wifi_ap_ip(), r, sizeof(r));
        if (rn) {
            sendto(sock, r, rn, 0, (struct sockaddr *)&from, fl);
        }
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ HTTP */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_page_start, setup_page_end - setup_page_start);
}

static esp_err_t config_get(httpd_req_t *req)
{
    char json[1024];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = config_to_json(&s_cfg, json, sizeof(json));
    xSemaphoreGive(s_lock);
    return n ? send_json(req, "200 OK", json) : httpd_resp_send_500(req);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    static wifi_scan_item_t items[20];
    int n = wifi_scan(items, 20);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < n; i++) {
        char ssid[200], line[260];
        json_escape(items[i].ssid, ssid, sizeof(ssid));
        snprintf(line, sizeof(line), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", i ? "," : "", ssid, items[i].rssi,
                 items[i].secure ? "true" : "false");
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (req->content_len >= MAX_BODY) {
        return send_json(req, "413 Payload Too Large", "{\"ok\":false,\"field\":\"\",\"error\":\"too large\"}");
    }
    char *body = calloc(1, MAX_BODY);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }

    char field[24] = "", err[192] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    device_config_t next = s_cfg;
    bool ok = config_apply_form(&next, body, field, sizeof(field), err, sizeof(err));
    if (ok) {
        s_cfg = next;
    }
    xSemaphoreGive(s_lock);
    free(body);

    if (!ok) {
        char json[600], f[48], e[400];
        json_escape(field, f, sizeof(f));
        json_escape(err, e, sizeof(e));
        snprintf(json, sizeof(json), "{\"ok\":false,\"field\":\"%s\",\"error\":\"%s\"}", f, e);
        return send_json(req, "400 Bad Request", json);
    }
    ESP_LOGI(TAG, "saved: Wi-Fi \"%s\", SIP %s %s@%s", next.wifi_ssid, next.sip_enabled ? "on" : "off",
             next.sip_user, next.sip_server);
    esp_err_t r = send_json(req, "200 OK", "{\"ok\":true}");
    if (s_cb.saved) {
        s_cb.saved(&next);
    }
    return r;
}

static esp_err_t cancel_post(httpd_req_t *req)
{
    esp_err_t r = send_json(req, "200 OK", "{\"ok\":true}");
    if (s_cb.cancelled) {
        s_cb.cancelled();
    }
    return r;
}

/*
 * Anything else (phones probe /generate_204, /hotspot-detect.html, ...):
 * redirect to the page, which makes the phone show its "sign in" sheet.
 */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    char ip[16];
    uint32_t a = wifi_ap_ip();
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF),
             (unsigned)((a >> 16) & 0xFF), (unsigned)(a >> 24));
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char location[96];
    /* Requests to one of our own addresses just go to "/"; foreign hosts go to the AP address. */
    uint32_t lan_ip, bcast;
    char lan[16] = "";
    if (wifi_get_addresses(&lan_ip, &bcast)) {
        snprintf(lan, sizeof(lan), "%u.%u.%u.%u", (unsigned)(lan_ip & 0xFF), (unsigned)((lan_ip >> 8) & 0xFF),
                 (unsigned)((lan_ip >> 16) & 0xFF), (unsigned)(lan_ip >> 24));
    }
    if (strcmp(host, ip) == 0 || (lan[0] && strcmp(host, lan) == 0)) {
        snprintf(location, sizeof(location), "/");
    } else {
        snprintf(location, sizeof(location), "http://%s/", ip);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t setup_portal_start(const device_config_t *current, const char *ap_ssid, const char *ap_pass,
                             const setup_portal_cb_t *cb)
{
    if (s_active) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_cb = *cb;
    s_cfg = *current;
    ESP_LOGI(TAG, "starting: %u bytes internal RAM free (largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_err_t err = wifi_ap_start(ap_ssid, ap_pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "access point failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;
    cfg.core_id = 0;
    err = httpd_start(&s_http, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server failed: %s", esp_err_to_name(err));
        wifi_ap_stop();
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/cancel", .method = HTTP_POST, .handler = cancel_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_http, &uris[i]);
    }
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, redirect_404);

    s_dns_run = true;
    if (xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 4, &s_dns_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "no DNS task: open http://192.168.4.1 by hand");
    }
    s_active = true;
    ESP_LOGI(TAG, "setup mode: join \"%s\" and open http://192.168.4.1", ap_ssid);
    return ESP_OK;
}

void setup_portal_stop(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    s_dns_run = false;
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    for (int i = 0; i < 20 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50)); /* DNS task exits within its 300 ms receive timeout */
    }
    wifi_ap_stop();
    ESP_LOGI(TAG, "setup mode off");
}

bool setup_portal_active(void)
{
    return s_active;
}
