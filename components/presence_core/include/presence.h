/*
 * BLE presence logger for the CUBE. Phones that bond with the device once are
 * remembered by their Identity Resolving Key, so the device recognises them
 * afterwards even though they rotate their Bluetooth address for privacy. Each
 * arrival and departure is logged with the time, and the log is viewable over
 * http://<device-ip>/presence.
 *
 * A phone is only ever recognised after it deliberately pairs with the device
 * (the IRK is exchanged during bonding and nowhere else), so this works only
 * for phones whose owner opts in. Compiled out unless CONFIG_PTT_PRESENCE_ENABLE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*notice)(const char *text, bool warn); /* short on-screen hint, may be NULL */
} presence_cb_t;

/* Bring up the store and BLE scanner. suffix is a short id for the advertised
 * name "CUBE-<suffix>". Returns ESP_OK (or a no-op OK when disabled). */
esp_err_t presence_start(const char *suffix, const presence_cb_t *cb);

/* The station got / lost an address: start SNTP and the log web server, or
 * stop the server. */
void presence_network_up(uint32_t ip);
void presence_network_down(void);

/* Hand port 80 to the phone setup portal, then take it back. */
void presence_web_pause(void);
void presence_web_resume(void);

/* Open a pairing window so one phone can enrol. */
void presence_enroll_begin(void);
bool presence_enroll_active(void);

#ifdef __cplusplus
}
#endif
