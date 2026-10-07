/*
 * Table of devices seen on the LAN, filled from HELLO broadcasts.
 * Not thread safe: the caller serialises access.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ptt_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PTT_MAX_PEERS 16
#define PTT_PEER_TIMEOUT_MS 6000

typedef struct {
    bool used;
    uint32_t device_id;
    uint32_t ip; /* IPv4 address in network byte order, as lwIP stores it */
    char name[PTT_NAME_LEN + 1];
    uint8_t channel;
    uint8_t battery;
    uint8_t state;
    uint32_t last_seen_ms;
} ptt_peer_t;

typedef struct {
    ptt_peer_t peers[PTT_MAX_PEERS];
} ptt_peers_t;

void ptt_peers_init(ptt_peers_t *t);

/* Insert or refresh a peer. Returns false when the table is full. */
bool ptt_peers_update(ptt_peers_t *t, uint32_t device_id, uint32_t ip, uint8_t channel,
                      const ptt_hello_t *hello, uint32_t now_ms);

/* Refresh only address/last_seen for a peer we hear audio from before its HELLO. */
void ptt_peers_touch(ptt_peers_t *t, uint32_t device_id, uint32_t ip, uint8_t channel, uint32_t now_ms);

/* Drop peers silent for longer than PTT_PEER_TIMEOUT_MS. Returns how many were removed. */
int ptt_peers_expire(ptt_peers_t *t, uint32_t now_ms);

const ptt_peer_t *ptt_peers_find(const ptt_peers_t *t, uint32_t device_id);

int ptt_peers_count(const ptt_peers_t *t, uint8_t channel); /* channel 0 = all */

/* Copy the IPs of every peer on `channel` into ips (max n). Returns the count. */
int ptt_peers_ips(const ptt_peers_t *t, uint8_t channel, uint32_t *ips, int n);

#ifdef __cplusplus
}
#endif
