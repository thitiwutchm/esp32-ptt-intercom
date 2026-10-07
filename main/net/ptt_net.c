#include "ptt_net.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi.h"

static const char *TAG = "net";

static int s_sock = -1;
static uint16_t s_port;
static ptt_net_rx_cb_t s_cb;

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[PTT_MAX_PACKET];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ptt_hdr_t hdr;
        const uint8_t *payload;
        if (ptt_proto_parse(buf, (size_t)n, &hdr, &payload)) {
            s_cb(&hdr, payload, from.sin_addr.s_addr);
        }
    }
}

esp_err_t ptt_net_start(uint16_t port, ptt_net_rx_cb_t cb)
{
    s_port = port;
    s_cb = cb;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        return ESP_FAIL;
    }
    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    /* DSCP EF (voice): Wi-Fi maps it to the WMM voice queue. */
    int tos = 0xB8;
    setsockopt(s_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %u failed", port);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    xTaskCreatePinnedToCore(rx_task, "net_rx", 4096, NULL, 18, NULL, 0);
    ESP_LOGI(TAG, "listening on UDP %u", port);
    return ESP_OK;
}

void ptt_net_send(uint32_t ip, const uint8_t *buf, size_t len)
{
    if (s_sock < 0) {
        return;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr.s_addr = ip,
    };
    /* Errors (e.g. ENOMEM while Wi-Fi is busy) just drop the frame, like a lost packet. */
    sendto(s_sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
}

void ptt_net_broadcast(const uint8_t *buf, size_t len)
{
    uint32_t ip, bcast;
    if (wifi_get_addresses(&ip, &bcast)) {
        ptt_net_send(bcast, buf, len);
    }
}
