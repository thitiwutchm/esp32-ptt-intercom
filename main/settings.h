/* Settings kept in NVS across reboots: device_config_t from the phone setup page. */
#pragma once

#include <stdint.h>

#include "device_config.h"

typedef device_config_t settings_t;

/* Load, falling back to menuconfig defaults and "PTT-xxxx" from the MAC. */
void settings_load(settings_t *s, uint32_t device_id);
/* Channel, volume and name only (changed on the device). */
void settings_save(const settings_t *s);

/* Everything, from the phone setup page: Wi-Fi and SIP stop following menuconfig. */
void settings_save_all(const settings_t *s);
