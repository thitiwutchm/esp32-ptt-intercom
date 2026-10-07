/* Just enough SDP (RFC 4566) for one G.711 audio stream. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDP_PT_PCMU 0
#define SDP_PT_PCMA 8

typedef struct {
    uint32_t ip; /* network byte order */
    uint16_t port;
    int pts[8]; /* payload types in the order offered */
    int npts;
} sdp_media_t;

/* Offer both PCMU and PCMA (pt = -1), or answer with a single payload type. */
size_t sdp_build(char *out, size_t cap, uint32_t local_ip, uint16_t rtp_port, uint32_t session_id, int pt);

bool sdp_parse(const char *body, size_t len, sdp_media_t *m);

/* First G.711 payload type in the remote list, honouring its order; -1 if none. */
int sdp_choose_pt(const sdp_media_t *m);

/* "a.b.c.d" <-> network byte order. */
bool ip_parse(const char *s, size_t len, uint32_t *ip);
void ip_format(uint32_t ip, char out[16]);

#ifdef __cplusplus
}
#endif
