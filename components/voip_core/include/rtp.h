/* Minimal RTP (RFC 3550) header handling. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTP_HDR_LEN 12

typedef struct {
    uint8_t pt;
    bool marker;
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
} rtp_hdr_t;

/* Write header + payload. Returns total length or 0 if it does not fit. */
size_t rtp_build(uint8_t *out, size_t cap, const rtp_hdr_t *h, const uint8_t *payload, size_t len);

/* Parse; skips CSRCs, extension and padding. */
bool rtp_parse(const uint8_t *buf, size_t len, rtp_hdr_t *h, const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif
