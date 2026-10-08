/*
 * flog: a fixed-record append log on a flat, sector-erased flash area, used
 * as a ring. Writing fills the area record by record; crossing into a sector
 * erases it first, which drops its (oldest) records. So the newest records
 * always survive and flash wears evenly (each sector is erased once per full
 * lap). The backend (read/write/erase over a byte range) is injected, so the
 * same code runs on an esp_partition on the device and on a RAM buffer in the
 * host tests.
 *
 * Each record is rec_size bytes: a 4-byte sequence number that flog owns,
 * followed by rec_size-4 bytes of caller payload. Erased flash reads as
 * 0xFF, so a sequence of 0xFFFFFFFF marks an empty slot; flog never writes
 * that value. On init flog scans the area to find the newest record (highest
 * sequence) and continues from there, so the log survives reboots.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Backend over a flat space of `size` bytes. off/len for erase are
     * sector-aligned. Return false on failure. ctx is passed back unchanged. */
    bool (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    bool (*write)(void *ctx, uint32_t off, const void *src, uint32_t len);
    bool (*erase)(void *ctx, uint32_t off, uint32_t len);
    void *ctx;

    uint32_t size;     /* total bytes; must be a whole number of sectors */
    uint32_t sector;   /* erase unit, e.g. 4096 */
    uint32_t rec_size; /* record size including the 4-byte sequence; divides sector */

    /* State, set by flog_init. */
    uint32_t count;    /* records that fit = size / rec_size */
    uint32_t head;     /* index of the next record to write (0..count-1) */
    uint32_t next_seq; /* sequence to stamp on the next append */
    bool ready;
} flog_t;

/*
 * Scan the area and get ready to append. The backend fields and size/sector/
 * rec_size must be set first. Returns false only if the geometry is invalid
 * or the backend fails; an empty or garbage area is fine (treated as empty).
 */
bool flog_init(flog_t *f);

/* Append one record. payload is rec_size-4 bytes. Returns false on I/O error. */
bool flog_append(flog_t *f, const void *payload);

/* Drop everything (erase the whole area) and restart sequence numbers. */
bool flog_clear(flog_t *f);

/*
 * Visit stored records newest first, at most `max` of them (<=0 = all). cb
 * gets the payload (rec_size-4 bytes) and the record's sequence number.
 */
typedef void (*flog_iter_cb)(void *user, const void *payload, uint32_t seq);
void flog_foreach_newest(flog_t *f, int max, flog_iter_cb cb, void *user);

#ifdef __cplusplus
}
#endif
