#include "presence_time.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "presence_time";
static bool s_started;

#define PLAUSIBLE 1700000000u /* 2023-11-14: anything earlier means the clock is unset */

void presence_time_start(void)
{
    /* Local time for the log page: Thailand is UTC+7 with no DST (POSIX
     * inverts the sign, so +7 is written "-7"). */
    setenv("TZ", "ICT-7", 1);
    tzset();

    if (s_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    cfg.server_from_dhcp = true; /* prefer the router's NTP server if it offers one */
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

uint32_t presence_now_unix(void)
{
    time_t now = time(NULL);
    return now > (time_t)PLAUSIBLE ? (uint32_t)now : 0;
}

bool presence_time_valid(void)
{
    return presence_now_unix() != 0;
}
