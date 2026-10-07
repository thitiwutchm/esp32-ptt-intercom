/*
 * Phone setup: a WPA2 access point, a captive-portal DNS and a web page to
 * set Wi-Fi and SIP. The page is also served on the normal LAN address while
 * setup mode is on.
 */
#pragma once

#include <stdbool.h>

#include "device_config.h"
#include "esp_err.h"

typedef struct {
    /* HTTP task: the user saved valid settings (already validated, not yet stored). */
    void (*saved)(const device_config_t *cfg);
    /* HTTP task: the user left without saving. */
    void (*cancelled)(void);
} setup_portal_cb_t;

/* Start with the current settings; ap_ssid / ap_pass name the setup network. */
esp_err_t setup_portal_start(const device_config_t *current, const char *ap_ssid, const char *ap_pass,
                             const setup_portal_cb_t *cb);
void setup_portal_stop(void);
bool setup_portal_active(void);
