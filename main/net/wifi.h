/*
 * Wi-Fi: station on the configured network (reconnecting forever), plus an
 * optional access point for phone setup (AP + STA at the same time).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*wifi_status_cb_t)(bool connected, uint32_t ip);

/* Start Wi-Fi. With an empty ssid the station stays idle (setup mode will configure it). */
esp_err_t wifi_start(const char *hostname, const char *ssid, const char *password, wifi_status_cb_t cb);

/* Current IPv4 address and subnet broadcast address (network byte order). False if offline. */
bool wifi_get_addresses(uint32_t *ip, uint32_t *broadcast);

/* Open the setup access point (WPA2). The station keeps its connection if it has one. */
esp_err_t wifi_ap_start(const char *ssid, const char *password);
void wifi_ap_stop(void);
uint32_t wifi_ap_ip(void); /* network byte order */

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_item_t;

/* Blocking scan (about 2 s), strongest first, one entry per SSID. Returns the count. */
int wifi_scan(wifi_scan_item_t *out, int max);

/* True once the station has had an address since boot. */
bool wifi_ever_connected(void);

typedef enum {
    WIFI_ERR_NONE = 0,  /* connected, or no attempt has failed yet */
    WIFI_ERR_NOT_FOUND, /* network not seen (wrong name, out of range, 5 GHz only) */
    WIFI_ERR_PASSWORD,  /* handshake failed: almost always a wrong password */
    WIFI_ERR_SECURITY,  /* network found but its security mode is not supported */
    WIFI_ERR_OTHER,
} wifi_err_t;

/* Why the station is not connected. reason (optional) gets the last disconnect code,
 * failures (optional) the attempts that failed in a row. */
wifi_err_t wifi_last_error(int *reason, int *failures);
