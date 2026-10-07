/*
 * Captive portal DNS: answer every A query with the setup access point's
 * address so a phone that joins it opens the setup page by itself.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build the reply to one DNS query. ip is in network byte order. A queries
 * get one answer, other types an empty answer. Returns the reply length, 0
 * if the packet should be ignored (not a standard query, malformed).
 */
size_t captive_dns_reply(const uint8_t *query, size_t len, uint32_t ip, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
