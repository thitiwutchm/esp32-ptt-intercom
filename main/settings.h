/* Settings kept in NVS across reboots. */
#pragma once

#include <stdint.h>

#include "ptt_proto.h"

typedef struct {
    uint8_t channel; /* 1..PTT_MAX_CHANNEL */
    uint8_t volume;  /* 0..100 */
    char name[PTT_NAME_LEN + 1];
} settings_t;

/* Load, falling back to menuconfig defaults and "PTT-xxxx" from the MAC. */
void settings_load(settings_t *s, uint32_t device_id);
void settings_save(const settings_t *s);
