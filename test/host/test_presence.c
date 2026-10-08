/* Host unit tests for components/presence_core pure logic: the IRK/RPA
 * resolver (irk_resolver.c) and the flash ring log (flog.c). Run: make -C test/host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flog.h"
#include "irk_resolver.h"

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

/* ---------------------------------------------------------------- IRK resolver */

/* Reverse a big-endian hex blob into the little-endian byte order NimBLE uses. */
static void be_to_le(const uint8_t *be, uint8_t *le, int n)
{
    for (int i = 0; i < n; i++) {
        le[i] = be[n - 1 - i];
    }
}

static void test_irk(void)
{
    /* Bluetooth Core spec sample data for ah (Vol 3, Part H, "ah"):
     *   IRK   = 0xec0234a357c8ad05341010a60a397d9b  (big-endian)
     *   prand = 0x708194
     *   ah    = 0x0dfbaa
     * So the resolvable address, big-endian, is prand||hash = 70 81 94 0d fb aa. */
    const uint8_t irk_be[16] = {0xec, 0x02, 0x34, 0xa3, 0x57, 0xc8, 0xad, 0x05,
                                0x34, 0x10, 0x10, 0xa6, 0x0a, 0x39, 0x7d, 0x9b};
    uint8_t irk[16];
    be_to_le(irk_be, irk, 16);

    /* ah() takes prand low byte first: 0x708194 -> {0x94,0x81,0x70}. */
    uint8_t prand[3] = {0x94, 0x81, 0x70};
    uint8_t h[3];
    irk_ah(irk, prand, h);
    CHECK(h[0] == 0xaa && h[1] == 0xfb && h[2] == 0x0d); /* 0x0dfbaa, low byte first */

    /* Full address, little-endian val[]: hash in val[0..2], prand in val[3..5]. */
    uint8_t addr[6] = {0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70};
    CHECK(irk_addr_is_rpa(addr));
    CHECK(irk_resolve(irk, addr));

    /* A one-bit change in the address no longer resolves. */
    uint8_t bad = addr[0] ^ 0x01;
    uint8_t addr2[6];
    memcpy(addr2, addr, 6);
    addr2[0] = bad;
    CHECK(!irk_resolve(irk, addr2));

    /* A different IRK does not resolve this address. */
    uint8_t other[16];
    memcpy(other, irk, 16);
    other[0] ^= 0xff;
    CHECK(!irk_resolve(other, addr));

    /* Non-resolvable (static, top bits 0b11) and public-style addresses are rejected. */
    uint8_t stat[6] = {1, 2, 3, 4, 5, 0xC0};
    CHECK(!irk_addr_is_rpa(stat));
    CHECK(!irk_resolve(irk, stat));
    uint8_t pub[6] = {1, 2, 3, 4, 5, 0x00};
    CHECK(!irk_addr_is_rpa(pub));
}

/* ---------------------------------------------------------------- flash ring log */

/* RAM-backed flash: 0xFF when erased, writes must land on erased bytes. */
#define FSIZE 4096u
#define SECT 512u /* small sectors so the test wraps quickly */
#define RSIZE 16u

typedef struct {
    uint8_t mem[FSIZE];
    int writes, erases;
} fake_t;

static bool fk_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    fake_t *fk = ctx;
    if (off + len > FSIZE) {
        return false;
    }
    memcpy(dst, fk->mem + off, len);
    return true;
}
static bool fk_write(void *ctx, uint32_t off, const void *src, uint32_t len)
{
    fake_t *fk = ctx;
    if (off + len > FSIZE) {
        return false;
    }
    const uint8_t *s = src;
    for (uint32_t i = 0; i < len; i++) {
        /* Real NOR flash only clears bits; writing needs an erased byte. */
        if (fk->mem[off + i] != 0xFF && fk->mem[off + i] != s[i]) {
            return false;
        }
        fk->mem[off + i] = s[i];
    }
    fk->writes++;
    return true;
}
static bool fk_erase(void *ctx, uint32_t off, uint32_t len)
{
    fake_t *fk = ctx;
    if (off + len > FSIZE || off % SECT || len % SECT) {
        return false;
    }
    memset(fk->mem + off, 0xFF, len);
    fk->erases++;
    return true;
}

static void mk(flog_t *f, fake_t *fk)
{
    memset(f, 0, sizeof(*f));
    f->read = fk_read;
    f->write = fk_write;
    f->erase = fk_erase;
    f->ctx = fk;
    f->size = FSIZE;
    f->sector = SECT;
    f->rec_size = RSIZE;
}

typedef struct {
    uint32_t vals[FSIZE / RSIZE];
    uint32_t seqs[FSIZE / RSIZE];
    int n;
} collect_t;

static void collect_cb(void *user, const void *payload, uint32_t seq)
{
    collect_t *c = user;
    memcpy(&c->vals[c->n], payload, 4); /* first 4 payload bytes hold our counter */
    c->seqs[c->n] = seq;
    c->n++;
}

static void append_u32(flog_t *f, uint32_t v)
{
    uint8_t payload[RSIZE - 4];
    memset(payload, 0, sizeof(payload));
    memcpy(payload, &v, 4);
    CHECK(flog_append(f, payload));
}

static void test_flog(void)
{
    fake_t fk;
    memset(fk.mem, 0xFF, FSIZE); /* virgin flash */
    fk.writes = fk.erases = 0;
    flog_t f;
    mk(&f, &fk);
    CHECK(flog_init(&f));
    CHECK(f.count == FSIZE / RSIZE); /* 256 records */

    /* Empty log: nothing to visit. */
    collect_t c = {0};
    flog_foreach_newest(&f, 0, collect_cb, &c);
    CHECK(c.n == 0);

    /* A few records come back newest first. */
    for (uint32_t i = 1; i <= 5; i++) {
        append_u32(&f, i);
    }
    c.n = 0;
    flog_foreach_newest(&f, 0, collect_cb, &c);
    CHECK(c.n == 5);
    CHECK(c.vals[0] == 5 && c.vals[4] == 1);
    CHECK(c.seqs[0] == 5 && c.seqs[4] == 1);

    /* max limits the count but still newest first. */
    c.n = 0;
    flog_foreach_newest(&f, 2, collect_cb, &c);
    CHECK(c.n == 2 && c.vals[0] == 5 && c.vals[1] == 4);

    /* Reopen the same flash: head and sequence are recovered from the records. */
    flog_t f2;
    mk(&f2, &fk);
    CHECK(flog_init(&f2));
    CHECK(f2.next_seq == 6);
    append_u32(&f2, 6);
    c.n = 0;
    flog_foreach_newest(&f2, 0, collect_cb, &c);
    CHECK(c.n == 6 && c.vals[0] == 6);

    /* Overflow the ring: write well past capacity, oldest records fall off but
     * the newest ones and their order survive, and sequence keeps climbing. */
    fake_t fk2;
    memset(fk2.mem, 0xFF, FSIZE);
    fk2.writes = fk2.erases = 0;
    flog_t g;
    mk(&g, &fk2);
    CHECK(flog_init(&g));
    uint32_t total = FSIZE / RSIZE * 3 + 7; /* three laps and a bit */
    for (uint32_t i = 1; i <= total; i++) {
        append_u32(&g, i);
    }
    c.n = 0;
    flog_foreach_newest(&g, 0, collect_cb, &c);
    CHECK(c.n > 0);
    CHECK(c.vals[0] == total);             /* newest is the last written */
    CHECK(c.seqs[0] == total);             /* sequence never resets */
    for (int i = 1; i < c.n; i++) {
        CHECK(c.vals[i] == c.vals[i - 1] - 1); /* contiguous, descending */
    }
    CHECK(fk2.erases >= 3);                 /* erased at least once per lap */

    /* Recover after the overflow too. */
    flog_t g2;
    mk(&g2, &fk2);
    CHECK(flog_init(&g2));
    CHECK(g2.next_seq == total + 1);

    /* Clear wipes everything. */
    CHECK(flog_clear(&g2));
    c.n = 0;
    flog_foreach_newest(&g2, 0, collect_cb, &c);
    CHECK(c.n == 0);
    CHECK(g2.next_seq == 1);
}

int main(void)
{
    test_irk();
    test_flog();
    printf("test_presence: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
