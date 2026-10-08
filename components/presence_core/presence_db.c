#include "presence_db.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "flog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "irk_resolver.h"
#include "nvs.h"
#include "presence_time.h"

static const char *TAG = "presence_db";

#define NS "presence"
#define SECTOR 4096u
#define DEPART_TIMEOUT_MS 120000u /* gone this long (no sighting) = departed */

/* Persisted phone. Written and read only by this firmware, so a plain struct
 * is fine; the blob is rewritten whole on any change. */
typedef struct {
    uint16_t id;
    uint8_t used;
    uint8_t name_auto; /* name is still a fallback: a better adv name may replace it */
    uint8_t irk[16];
    uint8_t id_addr[6];
    uint8_t pad[2];
    char name[PRESENCE_NAME_LEN];
    uint32_t enrolled_at;
} peer_rec_t;

/* Log record payload (flog adds a 4-byte sequence in front, making 16 bytes). */
typedef struct __attribute__((packed)) {
    uint32_t ts;
    uint16_t peer_id;
    uint8_t event;
    int8_t rssi;
    uint8_t flags; /* bit0: time was approximate (clock not yet set) */
    uint8_t rsvd[3];
} log_payload_t;
_Static_assert(sizeof(log_payload_t) == 12, "log payload must be 12 bytes");

#define FLAG_APPROX 0x01

static peer_rec_t s_peers[PRESENCE_MAX_PEERS];
static bool s_present[PRESENCE_MAX_PEERS];
static uint32_t s_last_seen_ms[PRESENCE_MAX_PEERS];
static int8_t s_last_rssi[PRESENCE_MAX_PEERS];
static uint16_t s_nextid = 1;

static SemaphoreHandle_t s_lock;
static flog_t s_log;
static const esp_partition_t *s_part;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ flash backend for flog */

static bool part_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, off, dst, len) == ESP_OK;
}
static bool part_write(void *ctx, uint32_t off, const void *src, uint32_t len)
{
    (void)ctx;
    return esp_partition_write(s_part, off, src, len) == ESP_OK;
}
static bool part_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    return esp_partition_erase_range(s_part, off, len) == ESP_OK;
}

/* ------------------------------------------------------------------ NVS (phones) */

static void peers_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed");
        return;
    }
    nvs_set_blob(h, "peers", s_peers, sizeof(s_peers));
    nvs_set_u16(h, "nextid", s_nextid);
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs commit failed");
    }
    nvs_close(h);
}

static void peers_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* first boot */
    }
    size_t len = sizeof(s_peers);
    if (nvs_get_blob(h, "peers", s_peers, &len) != ESP_OK || len != sizeof(s_peers)) {
        memset(s_peers, 0, sizeof(s_peers));
    }
    nvs_get_u16(h, "nextid", &s_nextid);
    if (s_nextid == 0) {
        s_nextid = 1;
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------ log */

static void log_event(uint16_t peer_id, presence_event_t ev, int8_t rssi)
{
    uint32_t ts = presence_now_unix();
    log_payload_t p = {
        .ts = ts,
        .peer_id = peer_id,
        .event = (uint8_t)ev,
        .rssi = rssi,
        .flags = ts ? 0 : FLAG_APPROX,
    };
    if (!s_log.ready || !flog_append(&s_log, &p)) {
        ESP_LOGW(TAG, "log append failed");
    }
}

/* ------------------------------------------------------------------ init */

void presence_db_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    peers_load();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "presence");
    if (!s_part) {
        ESP_LOGE(TAG, "no 'presence' partition: log disabled");
        return;
    }
    s_log = (flog_t){
        .read = part_read,
        .write = part_write,
        .erase = part_erase,
        .size = s_part->size,
        .sector = SECTOR,
        .rec_size = 16,
    };
    if (!flog_init(&s_log)) {
        ESP_LOGE(TAG, "log init failed (partition %u bytes)", (unsigned)s_part->size);
        s_log.ready = false;
    } else {
        ESP_LOGI(TAG, "log ready: %u records, next seq %u", (unsigned)s_log.count, (unsigned)s_log.next_seq);
    }

    int n = 0;
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (s_peers[i].used) {
            n++;
        }
    }
    ESP_LOGI(TAG, "%d phone(s) enrolled", n);
}

/* ------------------------------------------------------------------ enrolment (lock held by helpers) */

static int find_by_irk(const uint8_t irk[16])
{
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (s_peers[i].used && memcmp(s_peers[i].irk, irk, 16) == 0) {
            return i;
        }
    }
    return -1;
}

static void fallback_name(char *out, const uint8_t id_addr[6])
{
    /* Two most significant identity-address bytes; the user renames it later. */
    snprintf(out, PRESENCE_NAME_LEN, "Phone %02X:%02X", id_addr[5], id_addr[4]);
}

int presence_db_enroll(const uint8_t irk[16], const uint8_t id_addr[6], const char *name)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = find_by_irk(irk);
    if (i < 0) {
        for (int k = 0; k < PRESENCE_MAX_PEERS; k++) {
            if (!s_peers[k].used) {
                i = k;
                break;
            }
        }
    }
    if (i < 0) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "enrol: table full");
        return -1;
    }
    peer_rec_t *p = &s_peers[i];
    bool fresh = !p->used;
    if (fresh) {
        memset(p, 0, sizeof(*p));
        p->id = s_nextid++;
        p->used = 1;
    }
    memcpy(p->irk, irk, 16);
    memcpy(p->id_addr, id_addr, 6);
    p->enrolled_at = presence_now_unix();
    if (name && name[0]) {
        strlcpy(p->name, name, sizeof(p->name));
        p->name_auto = 0;
    } else if (fresh || p->name[0] == '\0') {
        fallback_name(p->name, id_addr);
        p->name_auto = 1;
    }
    uint16_t id = p->id;
    s_present[i] = true;
    s_last_seen_ms[i] = now_ms();
    peers_save();
    log_event(id, PRESENCE_EV_ENROLL, 0);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "enrolled id %u \"%s\"", id, p->name);
    return id;
}

bool presence_db_rename(uint16_t id, const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (s_peers[i].used && s_peers[i].id == id) {
            strlcpy(s_peers[i].name, name, sizeof(s_peers[i].name));
            s_peers[i].name_auto = 0;
            peers_save();
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool presence_db_forget(uint16_t id)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (s_peers[i].used && s_peers[i].id == id) {
            memset(&s_peers[i], 0, sizeof(s_peers[i]));
            s_present[i] = false;
            peers_save();
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok; /* log rows keep the id; they show as a forgotten phone */
}

void presence_db_clear_log(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_log.ready) {
        flog_clear(&s_log);
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ detection */

void presence_db_on_seen(const uint8_t addr[6], int8_t rssi, const char *adv_name)
{
    bool is_rpa = irk_addr_is_rpa(addr);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (!s_peers[i].used) {
            continue;
        }
        bool match = is_rpa ? irk_resolve(s_peers[i].irk, addr) : (memcmp(addr, s_peers[i].id_addr, 6) == 0);
        if (!match) {
            continue;
        }
        s_last_seen_ms[i] = now_ms();
        s_last_rssi[i] = rssi;
        if (!s_present[i]) {
            s_present[i] = true;
            log_event(s_peers[i].id, PRESENCE_EV_ARRIVE, rssi);
            ESP_LOGI(TAG, "arrive id %u \"%s\" (%d dBm)", s_peers[i].id, s_peers[i].name, rssi);
        }
        /* Adopt a real advertised name over the address fallback. */
        if (adv_name && adv_name[0] && s_peers[i].name_auto && strcmp(adv_name, s_peers[i].name) != 0) {
            strlcpy(s_peers[i].name, adv_name, sizeof(s_peers[i].name));
            peers_save();
        }
        break; /* one phone per address */
    }
    xSemaphoreGive(s_lock);
}

void presence_db_sweep(void)
{
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (s_peers[i].used && s_present[i] && (now - s_last_seen_ms[i]) > DEPART_TIMEOUT_MS) {
            s_present[i] = false;
            log_event(s_peers[i].id, PRESENCE_EV_DEPART, s_last_rssi[i]);
            ESP_LOGI(TAG, "depart id %u \"%s\"", s_peers[i].id, s_peers[i].name);
        }
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ snapshots for the web UI */

int presence_db_peers(presence_peer_view_t *out, int max)
{
    int n = 0;
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PRESENCE_MAX_PEERS && n < max; i++) {
        if (!s_peers[i].used) {
            continue;
        }
        presence_peer_view_t *v = &out[n++];
        v->id = s_peers[i].id;
        strlcpy(v->name, s_peers[i].name, sizeof(v->name));
        memcpy(v->irk, s_peers[i].irk, 16);
        v->enrolled_at = s_peers[i].enrolled_at;
        v->present = s_present[i];
        v->last_rssi = s_last_rssi[i];
        v->ago_ms = s_last_seen_ms[i] ? (now - s_last_seen_ms[i]) : 0;
    }
    xSemaphoreGive(s_lock);
    return n;
}

typedef struct {
    presence_row_t *out;
    int max;
    int n;
} collect_t;

/* Called under s_lock by flog_foreach_newest, so s_peers is safe to read. */
static void collect_cb(void *user, const void *payload, uint32_t seq)
{
    collect_t *c = user;
    if (c->n >= c->max) {
        return;
    }
    const log_payload_t *p = payload;
    presence_row_t *r = &c->out[c->n++];
    memset(r, 0, sizeof(*r));
    r->seq = seq;
    r->ts = p->ts;
    r->peer_id = p->peer_id;
    r->event = p->event;
    r->rssi = p->rssi;
    r->approx_time = (p->flags & FLAG_APPROX) != 0;
    for (int i = 0; i < PRESENCE_MAX_PEERS; i++) {
        if (s_peers[i].used && s_peers[i].id == p->peer_id) {
            r->known = true;
            strlcpy(r->name, s_peers[i].name, sizeof(r->name));
            memcpy(r->irk, s_peers[i].irk, 16);
            return;
        }
    }
    snprintf(r->name, sizeof(r->name), "(forgotten #%u)", p->peer_id);
}

int presence_db_collect(presence_row_t *out, int max)
{
    collect_t c = {.out = out, .max = max, .n = 0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    flog_foreach_newest(&s_log, max, collect_cb, &c);
    xSemaphoreGive(s_lock);
    return c.n;
}
