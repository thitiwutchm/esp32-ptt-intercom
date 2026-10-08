/* NimBLE side of presence: scan for enrolled phones, and during an enrol
 * window accept one phone bonding so we learn its IRK. Internal to
 * presence_core. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_notice_cb_t)(const char *text, bool warn);

/* Bring up the controller + NimBLE host and start scanning. suffix is the
 * short id shown in the advertised name "CUBE-<suffix>". */
esp_err_t ble_presence_start(const char *suffix, ble_notice_cb_t notice);

/* Advertise connectable for the configured window so a phone can pair. */
void ble_presence_enroll_begin(void);
bool ble_presence_enroll_active(void);

/* The advertised name, "CUBE-<suffix>" (empty before ble_presence_start). */
const char *ble_presence_name(void);

#ifdef __cplusplus
}
#endif
