#include "presence.h"

#include "sdkconfig.h"

#if CONFIG_PTT_PRESENCE_ENABLE

#include "ble_presence.h"
#include "presence_db.h"
#include "presence_time.h"
#include "presence_web.h"

static bool s_net_up;  /* the station has an address */
static bool s_paused;  /* phone setup holds port 80: keep our server down */

/* Run the web server only when the station is up and setup is not using the port. */
static void web_refresh(void)
{
    if (s_net_up && !s_paused) {
        presence_web_start();
    } else {
        presence_web_stop();
    }
}

esp_err_t presence_start(const char *suffix, const presence_cb_t *cb)
{
    presence_db_init();
    return ble_presence_start(suffix, cb ? cb->notice : NULL);
}

void presence_network_up(uint32_t ip)
{
    (void)ip;
    s_net_up = true;
    presence_time_start();
    web_refresh();
}

void presence_network_down(void)
{
    s_net_up = false;
    web_refresh();
}

void presence_web_pause(void)
{
    s_paused = true;
    web_refresh();
}

void presence_web_resume(void)
{
    s_paused = false;
    web_refresh();
}

void presence_enroll_begin(void)
{
    ble_presence_enroll_begin();
}

bool presence_enroll_active(void)
{
    return ble_presence_enroll_active();
}

#else /* feature off: no-op stubs so the app links on boards without it */

esp_err_t presence_start(const char *suffix, const presence_cb_t *cb)
{
    (void)suffix;
    (void)cb;
    return ESP_OK;
}
void presence_network_up(uint32_t ip) { (void)ip; }
void presence_network_down(void) {}
void presence_web_pause(void) {}
void presence_web_resume(void) {}
void presence_enroll_begin(void) {}
bool presence_enroll_active(void) { return false; }

#endif
