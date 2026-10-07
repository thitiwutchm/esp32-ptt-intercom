/* Wi-Fi station on the SSID from menuconfig, reconnecting forever. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*wifi_status_cb_t)(bool connected, uint32_t ip);

/* Start connecting. cb runs in the event task on every connect / disconnect. */
esp_err_t wifi_start(const char *hostname, wifi_status_cb_t cb);

/* Current IPv4 address and subnet broadcast address (network byte order). False if offline. */
bool wifi_get_addresses(uint32_t *ip, uint32_t *broadcast);
