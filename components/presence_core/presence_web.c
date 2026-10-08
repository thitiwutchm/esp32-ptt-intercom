#include "sdkconfig.h"

#if CONFIG_PTT_PRESENCE_ENABLE

#include "presence_web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_presence.h"
#include "device_config.h" /* json_escape, form_get from setup_core */
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "presence_db.h"
#include "presence_time.h"

static const char *TAG = "presence_web";

extern const char presence_page_start[] asm("_binary_presence_page_html_start");
extern const char presence_page_end[] asm("_binary_presence_page_html_end");

static httpd_handle_t s_http;

#define MAX_LOG 500
#define BODY_MAX 256

/* ------------------------------------------------------------------ small helpers */

/* IRK as big-endian hex (how bonding tools print it); stored little-endian. */
static void irk_hex(const uint8_t irk[16], char out[33])
{
    for (int i = 0; i < 16; i++) {
        sprintf(out + i * 2, "%02x", irk[15 - i]);
    }
}

static void format_when(uint32_t ts, char *out, size_t cap)
{
    if (ts == 0) {
        strlcpy(out, "(ก่อนตั้งเวลา)", cap);
        return;
    }
    time_t t = ts;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%d/%m/%Y %H:%M:%S", &tm);
}

/* Buffered chunked writer so we never hold the whole response in RAM. */
typedef struct {
    httpd_req_t *req;
    char buf[1024];
    size_t len;
    bool failed;
} chunker_t;

static void ck_flush(chunker_t *c)
{
    if (c->len && !c->failed) {
        if (httpd_resp_send_chunk(c->req, c->buf, c->len) != ESP_OK) {
            c->failed = true;
        }
        c->len = 0;
    }
}

static void ck_write(chunker_t *c, const char *s, size_t n)
{
    while (n && !c->failed) {
        size_t room = sizeof(c->buf) - c->len;
        if (room == 0) {
            ck_flush(c);
            continue;
        }
        size_t k = n < room ? n : room;
        memcpy(c->buf + c->len, s, k);
        c->len += k;
        s += k;
        n -= k;
    }
}

static void ck_puts(chunker_t *c, const char *s)
{
    ck_write(c, s, strlen(s));
}

__attribute__((format(printf, 2, 3))) static void ck_printf(chunker_t *c, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ck_write(c, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

/* Append a JSON string value (with quotes) using setup_core's escaper. */
static void ck_json_str(chunker_t *c, const char *s)
{
    char esc[256];
    if (!json_escape(s ? s : "", esc, sizeof(esc))) {
        esc[0] = '\0';
    }
    ck_printf(c, "\"%s\"", esc);
}

static esp_err_t ck_done(chunker_t *c)
{
    ck_flush(c);
    httpd_resp_send_chunk(c->req, NULL, 0);
    return c->failed ? ESP_FAIL : ESP_OK;
}

/* ------------------------------------------------------------------ handlers */

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, presence_page_start, presence_page_end - presence_page_start);
}

static esp_err_t json_get(httpd_req_t *req)
{
    presence_peer_view_t *peers = calloc(PRESENCE_MAX_PEERS, sizeof(*peers));
    presence_row_t *rows = heap_caps_malloc(MAX_LOG * sizeof(*rows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows) {
        rows = malloc(MAX_LOG * sizeof(*rows));
    }
    if (!peers || !rows) {
        free(peers);
        free(rows);
        return httpd_resp_send_500(req);
    }
    int np = presence_db_peers(peers, PRESENCE_MAX_PEERS);
    int nr = presence_db_collect(rows, MAX_LOG);

    char now[32], hex[33];
    format_when(presence_now_unix(), now, sizeof(now));

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    chunker_t c = {.req = req};

    ck_puts(&c, "{\"now\":");
    ck_json_str(&c, now);
    ck_printf(&c, ",\"time_valid\":%s,\"enrolling\":%s,\"name\":", presence_time_valid() ? "true" : "false",
              ble_presence_enroll_active() ? "true" : "false");
    ck_json_str(&c, ble_presence_name());
    ck_puts(&c, ",\"peers\":[");
    for (int i = 0; i < np; i++) {
        irk_hex(peers[i].irk, hex);
        ck_printf(&c, "%s{\"id\":%u,\"present\":%s,\"rssi\":%d,\"ago\":%u,\"name\":", i ? "," : "", peers[i].id,
                  peers[i].present ? "true" : "false", peers[i].rssi, (unsigned)(peers[i].ago_ms / 1000));
        ck_json_str(&c, peers[i].name);
        ck_printf(&c, ",\"irk\":\"%s\"}", hex);
    }
    ck_puts(&c, "],\"log\":[");
    for (int i = 0; i < nr; i++) {
        char when[32];
        format_when(rows[i].ts, when, sizeof(when));
        irk_hex(rows[i].irk, hex);
        ck_printf(&c, "%s{\"seq\":%u,\"ts\":%u,\"approx\":%s,\"event\":%u,\"id\":%u,\"rssi\":%d,\"when\":", i ? "," : "",
                  (unsigned)rows[i].seq, (unsigned)rows[i].ts, rows[i].approx_time ? "true" : "false", rows[i].event,
                  rows[i].peer_id, rows[i].rssi);
        ck_json_str(&c, when);
        ck_puts(&c, ",\"name\":");
        ck_json_str(&c, rows[i].name);
        ck_printf(&c, ",\"irk\":\"%s\"}", rows[i].known ? hex : "");
    }
    ck_puts(&c, "]}");
    esp_err_t r = ck_done(&c);
    free(peers);
    free(rows);
    return r;
}

static const char *ev_csv(uint8_t e)
{
    switch (e) {
    case PRESENCE_EV_ENROLL:
        return "enroll";
    case PRESENCE_EV_ARRIVE:
        return "arrive";
    case PRESENCE_EV_DEPART:
        return "depart";
    default:
        return "?";
    }
}

static esp_err_t csv_get(httpd_req_t *req)
{
    presence_row_t *rows = heap_caps_malloc(MAX_LOG * sizeof(*rows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows) {
        rows = malloc(MAX_LOG * sizeof(*rows));
    }
    if (!rows) {
        return httpd_resp_send_500(req);
    }
    int nr = presence_db_collect(rows, MAX_LOG);

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"presence.csv\"");
    chunker_t c = {.req = req};
    ck_puts(&c, "seq,unix_ts,when,event,phone_id,name,irk,rssi_dbm\n");
    for (int i = 0; i < nr; i++) {
        char when[32], hex[33];
        format_when(rows[i].ts, when, sizeof(when));
        irk_hex(rows[i].irk, hex);
        /* Names can hold commas/quotes: wrap and double any quote (RFC 4180). */
        ck_printf(&c, "%u,%u,%s,%s,%u,\"", (unsigned)rows[i].seq, (unsigned)rows[i].ts, when, ev_csv(rows[i].event),
                  rows[i].peer_id);
        for (const char *p = rows[i].name; *p; p++) {
            if (*p == '"') {
                ck_puts(&c, "\"\"");
            } else {
                ck_write(&c, p, 1);
            }
        }
        ck_printf(&c, "\",%s,%d\n", rows[i].known ? hex : "", rows[i].rssi);
    }
    free(rows);
    return ck_done(&c);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t ok(httpd_req_t *req, bool good)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, good ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t enroll_post(httpd_req_t *req)
{
    ble_presence_enroll_begin();
    return ok(req, true);
}

static esp_err_t rename_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ok(req, false);
    }
    char ids[8] = "", name[PRESENCE_NAME_LEN] = "";
    if (!form_get(body, "id", ids, sizeof(ids)) || !form_get(body, "name", name, sizeof(name))) {
        return ok(req, false);
    }
    return ok(req, presence_db_rename((uint16_t)atoi(ids), name));
}

static esp_err_t forget_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return ok(req, false);
    }
    char ids[8] = "";
    if (!form_get(body, "id", ids, sizeof(ids))) {
        return ok(req, false);
    }
    return ok(req, presence_db_forget((uint16_t)atoi(ids)));
}

static esp_err_t clear_post(httpd_req_t *req)
{
    presence_db_clear_log();
    return ok(req, true);
}

/* ------------------------------------------------------------------ lifecycle */

void presence_web_start(void)
{
    if (s_http) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 5120;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.core_id = 0;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "web server start failed");
        s_http = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/presence", .method = HTTP_GET, .handler = page_get},
        {.uri = "/presence.json", .method = HTTP_GET, .handler = json_get},
        {.uri = "/presence.csv", .method = HTTP_GET, .handler = csv_get},
        {.uri = "/api/presence/enroll", .method = HTTP_POST, .handler = enroll_post},
        {.uri = "/api/presence/rename", .method = HTTP_POST, .handler = rename_post},
        {.uri = "/api/presence/forget", .method = HTTP_POST, .handler = forget_post},
        {.uri = "/api/presence/clear", .method = HTTP_POST, .handler = clear_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_http, &uris[i]);
    }
    ESP_LOGI(TAG, "log web server on port 80");
}

void presence_web_stop(void)
{
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
}

#endif /* CONFIG_PTT_PRESENCE_ENABLE */
