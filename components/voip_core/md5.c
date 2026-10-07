#include "md5.h"

#include <stdarg.h>
#include <string.h>

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void block(md5_ctx_t *c, const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) | ((uint32_t)p[i * 4 + 2] << 16) |
               ((uint32_t)p[i * 4 + 3] << 24);
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + ROTL(a + f + K[i] + m[g], R[i]);
        a = tmp;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
}

void md5_init(md5_ctx_t *c)
{
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->bytes = 0;
}

void md5_update(md5_ctx_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t used = c->bytes % 64;
    c->bytes += len;
    while (len) {
        size_t n = 64 - used < len ? 64 - used : len;
        memcpy(c->buf + used, p, n);
        used += n;
        p += n;
        len -= n;
        if (used == 64) {
            block(c, c->buf);
            used = 0;
        }
    }
}

void md5_final(md5_ctx_t *c, uint8_t out[16])
{
    uint64_t bits = c->bytes * 8;
    uint8_t pad = 0x80;
    md5_update(c, &pad, 1);
    pad = 0;
    while (c->bytes % 64 != 56) {
        md5_update(c, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (8 * i));
    }
    md5_update(c, len, 8);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out[i * 4 + j] = (uint8_t)(c->state[i] >> (8 * j));
        }
    }
}

void md5_hex(char out[33], int count, ...)
{
    md5_ctx_t c;
    md5_init(&c);
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++) {
        const char *s = va_arg(ap, const char *);
        md5_update(&c, s, strlen(s));
    }
    va_end(ap);
    uint8_t d[16];
    md5_final(&c, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 15];
    }
    out[32] = '\0';
}
