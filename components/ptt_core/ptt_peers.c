#include "ptt_peers.h"

#include <string.h>

void ptt_peers_init(ptt_peers_t *t)
{
    memset(t, 0, sizeof(*t));
}

static ptt_peer_t *find_or_alloc(ptt_peers_t *t, uint32_t device_id)
{
    ptt_peer_t *free_slot = NULL;
    for (int i = 0; i < PTT_MAX_PEERS; i++) {
        ptt_peer_t *p = &t->peers[i];
        if (p->used && p->device_id == device_id) {
            return p;
        }
        if (!p->used && !free_slot) {
            free_slot = p;
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->device_id = device_id;
        free_slot->battery = 255;
        /* Placeholder name until the first HELLO arrives. */
        memcpy(free_slot->name, "?", 2);
    }
    return free_slot;
}

bool ptt_peers_update(ptt_peers_t *t, uint32_t device_id, uint32_t ip, uint8_t channel,
                      const ptt_hello_t *hello, uint32_t now_ms)
{
    ptt_peer_t *p = find_or_alloc(t, device_id);
    if (!p) {
        return false;
    }
    p->ip = ip;
    p->channel = channel;
    p->last_seen_ms = now_ms;
    if (hello) {
        memcpy(p->name, hello->name, sizeof(p->name));
        p->name[PTT_NAME_LEN] = '\0';
        p->battery = hello->battery;
        p->state = hello->state;
    }
    return true;
}

void ptt_peers_touch(ptt_peers_t *t, uint32_t device_id, uint32_t ip, uint8_t channel, uint32_t now_ms)
{
    ptt_peers_update(t, device_id, ip, channel, NULL, now_ms);
}

int ptt_peers_expire(ptt_peers_t *t, uint32_t now_ms)
{
    int removed = 0;
    for (int i = 0; i < PTT_MAX_PEERS; i++) {
        ptt_peer_t *p = &t->peers[i];
        if (p->used && (uint32_t)(now_ms - p->last_seen_ms) > PTT_PEER_TIMEOUT_MS) {
            p->used = false;
            removed++;
        }
    }
    return removed;
}

const ptt_peer_t *ptt_peers_find(const ptt_peers_t *t, uint32_t device_id)
{
    for (int i = 0; i < PTT_MAX_PEERS; i++) {
        if (t->peers[i].used && t->peers[i].device_id == device_id) {
            return &t->peers[i];
        }
    }
    return NULL;
}

int ptt_peers_count(const ptt_peers_t *t, uint8_t channel)
{
    int n = 0;
    for (int i = 0; i < PTT_MAX_PEERS; i++) {
        if (t->peers[i].used && (channel == 0 || t->peers[i].channel == channel)) {
            n++;
        }
    }
    return n;
}

int ptt_peers_ips(const ptt_peers_t *t, uint8_t channel, uint32_t *ips, int n)
{
    int k = 0;
    for (int i = 0; i < PTT_MAX_PEERS && k < n; i++) {
        if (t->peers[i].used && t->peers[i].channel == channel) {
            ips[k++] = t->peers[i].ip;
        }
    }
    return k;
}
