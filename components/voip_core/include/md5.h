/* MD5 (RFC 1321), only for SIP digest authentication. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[4];
    uint64_t bytes;
    uint8_t buf[64];
} md5_ctx_t;

void md5_init(md5_ctx_t *c);
void md5_update(md5_ctx_t *c, const void *data, size_t len);
void md5_final(md5_ctx_t *c, uint8_t out[16]);

/* Lowercase hex digest of the concatenation of the given NUL-terminated strings. */
void md5_hex(char out[33], int count, ...);

#ifdef __cplusplus
}
#endif
