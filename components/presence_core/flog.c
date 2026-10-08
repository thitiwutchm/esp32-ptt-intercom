#include "flog.h"

#include <string.h>

#define EMPTY_SEQ 0xFFFFFFFFu

static uint32_t rd_seq(const uint8_t *rec)
{
    /* Little-endian, matching how we write it; the device is little-endian too. */
    return (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) | ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);
}

static bool read_rec(flog_t *f, uint32_t idx, uint8_t *buf)
{
    return f->read(f->ctx, idx * f->rec_size, buf, f->rec_size);
}

bool flog_init(flog_t *f)
{
    if (!f || !f->read || !f->write || !f->erase || !f->rec_size || !f->sector || !f->size) {
        return false;
    }
    if (f->rec_size < 4 || f->sector % f->rec_size != 0 || f->size % f->sector != 0) {
        return false; /* geometry must line records up with sectors */
    }
    f->count = f->size / f->rec_size;
    f->head = 0;
    f->next_seq = 1;
    f->ready = false;

    /* Find the newest record: highest sequence among the non-empty slots. The
     * slot after it is where writing continues. Sequence numbers only grow, so
     * a wrap (older, smaller numbers physically after newer ones) sorts right. */
    uint8_t rec[64];
    uint8_t *buf = rec;
    uint8_t *heap = NULL;
    if (f->rec_size > sizeof(rec)) {
        return false; /* rec_size is tiny in practice; keep the stack buffer */
    }
    (void)heap;

    bool any = false;
    uint32_t best_seq = 0, best_idx = 0;
    for (uint32_t i = 0; i < f->count; i++) {
        if (!read_rec(f, i, buf)) {
            return false;
        }
        uint32_t seq = rd_seq(buf);
        if (seq == EMPTY_SEQ) {
            continue;
        }
        if (!any || seq > best_seq) {
            any = true;
            best_seq = seq;
            best_idx = i;
        }
    }
    if (any) {
        f->head = (best_idx + 1) % f->count;
        f->next_seq = best_seq + 1;
        if (f->next_seq == EMPTY_SEQ) {
            f->next_seq = 1; /* never stamp the empty marker */
        }
    }
    f->ready = true;
    return true;
}

bool flog_append(flog_t *f, const void *payload)
{
    if (!f || !f->ready) {
        return false;
    }
    uint32_t per_sector = f->sector / f->rec_size;
    /* First record of a sector: erase it, dropping the oldest lap's records. */
    if (f->head % per_sector == 0) {
        uint32_t off = (f->head / per_sector) * f->sector;
        if (!f->erase(f->ctx, off, f->sector)) {
            return false;
        }
    }
    uint8_t rec[64];
    uint32_t seq = f->next_seq;
    rec[0] = (uint8_t)seq;
    rec[1] = (uint8_t)(seq >> 8);
    rec[2] = (uint8_t)(seq >> 16);
    rec[3] = (uint8_t)(seq >> 24);
    memcpy(rec + 4, payload, f->rec_size - 4);
    if (!f->write(f->ctx, f->head * f->rec_size, rec, f->rec_size)) {
        return false;
    }
    f->head = (f->head + 1) % f->count;
    f->next_seq = seq + 1;
    if (f->next_seq == EMPTY_SEQ) {
        f->next_seq = 1;
    }
    return true;
}

bool flog_clear(flog_t *f)
{
    if (!f) {
        return false;
    }
    if (!f->erase(f->ctx, 0, f->size)) {
        return false;
    }
    f->head = 0;
    f->next_seq = 1;
    f->ready = true;
    return true;
}

void flog_foreach_newest(flog_t *f, int max, flog_iter_cb cb, void *user)
{
    if (!f || !f->ready || !cb) {
        return;
    }
    uint8_t rec[64];
    int seen = 0;
    for (uint32_t n = 0; n < f->count; n++) {
        if (max > 0 && seen >= max) {
            break;
        }
        uint32_t idx = (f->head + f->count - 1 - n) % f->count; /* newest backwards */
        if (!read_rec(f, idx, rec)) {
            break;
        }
        uint32_t seq = rd_seq(rec);
        if (seq == EMPTY_SEQ) {
            break; /* reached the erased gap: everything older is gone */
        }
        cb(user, rec + 4, seq);
        seen++;
    }
}
