/* Wall-clock time for the log, from SNTP once Wi-Fi is up. Internal to
 * presence_core. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start SNTP (idempotent) and set the Thailand time zone for local formatting. */
void presence_time_start(void);

/* Unix seconds now, or 0 if the clock has not been set yet. */
uint32_t presence_now_unix(void);

/* True once SNTP has set a plausible clock. */
bool presence_time_valid(void);

#ifdef __cplusplus
}
#endif
