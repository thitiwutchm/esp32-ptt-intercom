/*
 * The presence store: enrolled phones (IRK + name, in NVS) and the event log
 * (arrivals/departures, in a flash ring via flog). Internal to presence_core;
 * the BLE scanner feeds it sightings and the web handlers read snapshots.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRESENCE_MAX_PEERS 16
#define PRESENCE_NAME_LEN 32

typedef enum {
    PRESENCE_EV_ENROLL = 0, /* phone paired and added */
    PRESENCE_EV_ARRIVE = 1, /* phone came into range */
    PRESENCE_EV_DEPART = 2, /* phone not seen for a while */
} presence_event_t;

/* One log row for the web page, with the phone's name and IRK resolved. */
typedef struct {
    uint32_t seq;
    uint32_t ts;   /* unix seconds, 0 if the clock was not set when logged */
    uint16_t peer_id;
    uint8_t event; /* presence_event_t */
    int8_t rssi;
    bool approx_time;
    bool known; /* false if the phone was later forgotten */
    char name[PRESENCE_NAME_LEN];
    uint8_t irk[16];
} presence_row_t;

/* One enrolled phone for the web page. */
typedef struct {
    uint16_t id;
    char name[PRESENCE_NAME_LEN];
    uint8_t irk[16];
    uint32_t enrolled_at;
    bool present;
    int8_t last_rssi;
    uint32_t ago_ms; /* since last seen, 0 if never */
} presence_peer_view_t;

void presence_db_init(void);

/* Add or refresh a bonded phone from its IRK and identity address. name may be
 * NULL/empty (a fallback is used). Returns the phone's id, or -1 on failure. */
int presence_db_enroll(const uint8_t irk[16], const uint8_t id_addr[6], const char *name);
bool presence_db_rename(uint16_t id, const char *name);
bool presence_db_forget(uint16_t id);
void presence_db_clear_log(void);

/* Every advertisement the scanner sees (addr little-endian, adv_name may be
 * NULL). Resolves it against enrolled phones and logs arrivals. */
void presence_db_on_seen(const uint8_t addr[6], int8_t rssi, const char *adv_name);

/* Call about once a second: logs a departure for phones gone quiet too long. */
void presence_db_sweep(void);

/* Snapshots for the web UI. Return the number written (newest first for the log). */
int presence_db_peers(presence_peer_view_t *out, int max);
int presence_db_collect(presence_row_t *out, int max);

#ifdef __cplusplus
}
#endif
