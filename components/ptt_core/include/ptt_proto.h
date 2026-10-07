/*
 * PTT wire protocol.
 *
 * Every datagram is a fixed 18-byte little-endian header followed by an
 * optional payload. One UDP port carries everything.
 *
 *   off size field
 *     0    2 magic 'P' 'T'
 *     2    1 version (PTT_PROTO_VERSION)
 *     3    1 type (ptt_msg_type_t)
 *     4    4 device_id   sender id (last 4 bytes of its Wi-Fi MAC)
 *     8    4 talk_id     random per PTT press, so frames of an earlier press are ignored
 *    12    2 seq         audio frame counter, wraps
 *    14    1 channel     1..PTT_MAX_CHANNEL
 *    15    1 codec       ptt_codec_t (AUDIO only)
 *    16    2 payload_len
 *
 * This header is pure C with no ESP-IDF dependency so it can be unit tested
 * on a PC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTT_PROTO_VERSION 1
#define PTT_HDR_LEN 18
#define PTT_MAX_PAYLOAD 1024
#define PTT_MAX_PACKET (PTT_HDR_LEN + PTT_MAX_PAYLOAD)
#define PTT_MAX_CHANNEL 16
#define PTT_NAME_LEN 16 /* bytes on the wire, NUL padded; not necessarily NUL terminated */

typedef enum {
    PTT_MSG_HELLO = 1,      /* broadcast presence, payload ptt_hello_t */
    PTT_MSG_TALK_START = 2, /* sender takes the floor on its channel */
    PTT_MSG_AUDIO = 3,      /* one 20 ms voice frame */
    PTT_MSG_TALK_END = 4,   /* sender released the floor */
} ptt_msg_type_t;

typedef enum {
    PTT_CODEC_PCM16 = 0,     /* 320 samples int16 LE = 640 bytes */
    PTT_CODEC_IMA_ADPCM = 1, /* 4-byte state + 160 bytes = 164 bytes */
} ptt_codec_t;

typedef enum {
    PTT_PEER_IDLE = 0,
    PTT_PEER_TALKING = 1,
    PTT_PEER_LISTENING = 2,
} ptt_peer_state_t;

typedef struct {
    uint8_t type;
    uint32_t device_id;
    uint32_t talk_id;
    uint16_t seq;
    uint8_t channel;
    uint8_t codec;
    uint16_t payload_len;
} ptt_hdr_t;

typedef struct {
    char name[PTT_NAME_LEN + 1]; /* always NUL terminated after parsing */
    uint8_t battery;             /* 0..100, 255 = unknown */
    uint8_t state;               /* ptt_peer_state_t */
} ptt_hello_t;

#define PTT_HELLO_LEN (PTT_NAME_LEN + 2)

/* Serialise header + payload into out. Returns total length, or 0 if it does not fit. */
size_t ptt_proto_build(uint8_t *out, size_t out_cap, const ptt_hdr_t *hdr, const void *payload);

/* Parse a received datagram. On success fills hdr and points *payload into buf. */
bool ptt_proto_parse(const uint8_t *buf, size_t len, ptt_hdr_t *hdr, const uint8_t **payload);

size_t ptt_hello_encode(uint8_t *out, size_t out_cap, const ptt_hello_t *hello);
bool ptt_hello_decode(const uint8_t *buf, size_t len, ptt_hello_t *hello);

#ifdef __cplusplus
}
#endif
