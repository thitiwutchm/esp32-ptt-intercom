#include "sip_client.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "g711.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "rtp.h"
#include "sdkconfig.h"

#if CONFIG_PTT_SIP_ENABLE

static const char *TAG = "sip";

static sip_client_cb_t s_cb;
static device_config_t s_cfg;
static bool s_enabled;
static SemaphoreHandle_t s_lock; /* recursive: sip_ua callbacks run with it held */
static sip_ua_t s_ua;
static bool s_ua_ready;
static uint32_t s_local_ip;
static int s_sip_sock = -1;
static int s_rtp_sock = -1;
static TaskHandle_t s_task;

/* media, guarded by s_lock */
static bool s_media_on;
static uint32_t s_media_ip;
static uint16_t s_media_port;
static int s_pt;
static uint16_t s_tx_seq;
static uint32_t s_tx_ts;
static uint32_t s_tx_ssrc;
static bool s_tx_first;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void op_send(void *ctx, const char *buf, size_t len, uint32_t ip, uint16_t port)
{
    (void)ctx;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
    sendto(s_sip_sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static void op_reg(void *ctx, sip_reg_state_t st, int status)
{
    (void)ctx;
    ESP_LOGI(TAG, "registration %s (%d)",
             st == SIP_REG_OK ? "ok" : st == SIP_REG_TRYING ? "trying" : st == SIP_REG_FAILED ? "FAILED" : "idle",
             status);
    if (s_cb.reg_changed) {
        s_cb.reg_changed(st);
    }
}

static void op_call(void *ctx, const sip_call_info_t *info)
{
    (void)ctx;
    ESP_LOGI(TAG, "call %s \"%s\" reason %s status %d", sip_call_state_name(info->state), info->peer,
             sip_end_reason_name(info->reason), info->status);
    s_media_on = info->state == SIP_CALL_ACTIVE;
    if (s_media_on) {
        bool new_stream = s_media_ip != info->media_ip || s_media_port != info->media_port || s_pt != info->pt;
        s_media_ip = info->media_ip;
        s_media_port = info->media_port;
        s_pt = info->pt;
        if (new_stream) {
            s_tx_ssrc = esp_random();
            s_tx_seq = (uint16_t)esp_random();
            s_tx_ts = esp_random();
            s_tx_first = true;
        }
    } else {
        s_media_ip = 0;
        s_media_port = 0;
    }
    if (s_cb.call_changed) {
        s_cb.call_changed(info);
    }
}

static bool op_busy(void *ctx)
{
    (void)ctx;
    return s_cb.is_busy && s_cb.is_busy();
}

static uint32_t op_rand(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static int udp_socket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    int tos = 0xB8; /* DSCP EF: voice queue */
    setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    return s;
}

static bool resolve_server(uint32_t *ip)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(s_cfg.sip_server, NULL, &hints, &res) != 0 || !res) {
        return false;
    }
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

/* Called with s_lock held. */
static void setup_ua(void)
{
    uint32_t server;
    if (!resolve_server(&server)) {
        ESP_LOGW(TAG, "cannot resolve \"%s\", retrying", s_cfg.sip_server);
        return;
    }
    sip_ua_cfg_t cfg = {
        .server_ip = server,
        .server_port = s_cfg.sip_port,
        .local_ip = s_local_ip,
        .local_port = CONFIG_PTT_SIP_LOCAL_PORT,
        .rtp_port = CONFIG_PTT_SIP_RTP_PORT,
        .expires = 300,
    };
    strlcpy(cfg.domain, s_cfg.sip_server, sizeof(cfg.domain));
    strlcpy(cfg.user, s_cfg.sip_user, sizeof(cfg.user));
    strlcpy(cfg.password, s_cfg.sip_pass, sizeof(cfg.password));
    strlcpy(cfg.display, s_cfg.sip_display[0] ? s_cfg.sip_display : s_cfg.name, sizeof(cfg.display));
    sip_ua_ops_t ops = {.send = op_send, .reg_changed = op_reg, .call_changed = op_call, .is_busy = op_busy,
                        .random = op_rand};
    sip_ua_init(&s_ua, &cfg, &ops, now_ms());
    s_ua_ready = true;
    ESP_LOGI(TAG, "registering %s@%s", s_cfg.sip_user, s_cfg.sip_server);
    sip_ua_register(&s_ua, now_ms());
}

static void handle_rtp(const uint8_t *buf, size_t len, uint32_t src_ip)
{
    rtp_hdr_t h;
    const uint8_t *pl;
    size_t pl_len;
    if (!rtp_parse(buf, len, &h, &pl, &pl_len)) {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    bool ok = s_media_on && src_ip == s_media_ip && (h.pt == 0 || h.pt == 8);
    xSemaphoreGiveRecursive(s_lock);
    if (ok && s_cb.rtp_frame) {
        s_cb.rtp_frame(h.seq, h.pt, pl, pl_len); /* telephone-event and comfort noise are dropped above */
    }
}

static void task(void *arg)
{
    (void)arg;
    static char buf[2048];
    uint32_t last_setup_try = 0;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s_sip_sock, &fds);
        FD_SET(s_rtp_sock, &fds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        int maxfd = s_sip_sock > s_rtp_sock ? s_sip_sock : s_rtp_sock;
        int n = select(maxfd + 1, &fds, NULL, NULL, &tv);
        if (n > 0 && FD_ISSET(s_rtp_sock, &fds)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int len = recvfrom(s_rtp_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (len > 0) {
                handle_rtp((uint8_t *)buf, (size_t)len, from.sin_addr.s_addr);
            }
        }
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        if (n > 0 && FD_ISSET(s_sip_sock, &fds)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int len = recvfrom(s_sip_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
            if (len > 0 && s_ua_ready) {
                sip_ua_on_datagram(&s_ua, buf, (size_t)len, from.sin_addr.s_addr, ntohs(from.sin_port), now_ms());
            }
        }
        uint32_t now = now_ms();
        if (s_ua_ready) {
            sip_ua_tick(&s_ua, now);
        } else if (s_local_ip && now - last_setup_try > 10000) {
            last_setup_try = now;
            setup_ua();
        }
        xSemaphoreGiveRecursive(s_lock);
    }
}

esp_err_t sip_client_start(const sip_client_cb_t *cb, const device_config_t *cfg)
{
    s_cb = *cb;
    s_cfg = *cfg;
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!cfg->sip_enabled) {
        ESP_LOGI(TAG, "SIP off");
        return ESP_OK;
    }
    s_enabled = true;
    s_sip_sock = udp_socket(CONFIG_PTT_SIP_LOCAL_PORT);
    s_rtp_sock = udp_socket(CONFIG_PTT_SIP_RTP_PORT);
    if (s_sip_sock < 0 || s_rtp_sock < 0) {
        ESP_LOGE(TAG, "cannot open UDP %d / %d", CONFIG_PTT_SIP_LOCAL_PORT, CONFIG_PTT_SIP_RTP_PORT);
        return ESP_FAIL;
    }
    /* sip_ua keeps several 2 KB messages on the stack in nested handlers. */
    /* PSRAM stack: internal RAM is scarce and this task never touches flash. */
    if (xTaskCreatePinnedToCoreWithCaps(task, "sip", 16384, NULL, 17, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the SIP task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sip_client_network_up(uint32_t local_ip)
{
    if (!s_enabled) {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (local_ip != s_local_ip || !s_ua_ready) {
        if (s_ua_ready && s_ua.info.state != SIP_CALL_IDLE) {
            sip_ua_hangup(&s_ua, now_ms());
        }
        s_local_ip = local_ip;
        s_ua_ready = false;
        setup_ua();
    } else {
        sip_ua_register(&s_ua, now_ms()); /* same address: refresh at once */
    }
    xSemaphoreGiveRecursive(s_lock);
}

void sip_client_network_down(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_ua_ready && s_ua.info.state != SIP_CALL_IDLE) {
        sip_ua_hangup(&s_ua, now_ms());
    }
    xSemaphoreGiveRecursive(s_lock);
}

bool sip_client_call(const char *target)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    bool ok = s_ua_ready && sip_ua_call(&s_ua, target, now_ms());
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

bool sip_client_answer(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    bool ok = s_ua_ready && sip_ua_answer(&s_ua, now_ms());
    xSemaphoreGiveRecursive(s_lock);
    return ok;
}

void sip_client_hangup(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_ua_ready) {
        sip_ua_hangup(&s_ua, now_ms());
    }
    xSemaphoreGiveRecursive(s_lock);
}

sip_reg_state_t sip_client_reg_state(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    sip_reg_state_t st = s_ua_ready ? sip_ua_reg_state(&s_ua) : SIP_REG_IDLE;
    xSemaphoreGiveRecursive(s_lock);
    return st;
}

void sip_client_send_audio(const int16_t *pcm8k, int samples)
{
    uint8_t payload[160];
    uint8_t pkt[RTP_HDR_LEN + 160];
    if (samples > 160) {
        samples = 160;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (!s_media_on) {
        xSemaphoreGiveRecursive(s_lock);
        return;
    }
    rtp_hdr_t h = {.pt = (uint8_t)s_pt, .marker = s_tx_first, .seq = s_tx_seq++, .ts = s_tx_ts, .ssrc = s_tx_ssrc};
    s_tx_ts += (uint32_t)samples;
    s_tx_first = false;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(s_media_port), .sin_addr.s_addr = s_media_ip};
    int pt = s_pt;
    xSemaphoreGiveRecursive(s_lock);

    g711_encode(pt, pcm8k, payload, samples);
    size_t n = rtp_build(pkt, sizeof(pkt), &h, payload, (size_t)samples);
    sendto(s_rtp_sock, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
}

#else /* !CONFIG_PTT_SIP_ENABLE */

esp_err_t sip_client_start(const sip_client_cb_t *cb, const device_config_t *cfg)
{
    (void)cb;
    (void)cfg;
    return ESP_OK;
}
void sip_client_network_up(uint32_t local_ip)
{
    (void)local_ip;
}
void sip_client_network_down(void) {}
bool sip_client_call(const char *target)
{
    (void)target;
    return false;
}
bool sip_client_answer(void)
{
    return false;
}
void sip_client_hangup(void) {}
sip_reg_state_t sip_client_reg_state(void)
{
    return SIP_REG_IDLE;
}
void sip_client_send_audio(const int16_t *pcm8k, int samples)
{
    (void)pcm8k;
    (void)samples;
}

#endif
