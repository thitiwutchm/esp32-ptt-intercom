#include "captive_dns.h"

#include <string.h>

#define DNS_HDR 12

size_t captive_dns_reply(const uint8_t *q, size_t len, uint32_t ip, uint8_t *out, size_t cap)
{
    if (len < DNS_HDR + 5 || (q[2] & 0x80) || ((q[2] >> 3) & 0x0F) != 0) {
        return 0; /* too short, a response, or not a standard query */
    }
    if (q[4] != 0 || q[5] != 1) {
        return 0; /* exactly one question */
    }
    size_t p = DNS_HDR;
    while (p < len && q[p] != 0) {
        if ((q[p] & 0xC0) != 0 || q[p] > 63) {
            return 0; /* compression is not allowed in a question */
        }
        p += 1 + q[p];
    }
    if (p + 5 > len) {
        return 0;
    }
    p++; /* root label */
    uint16_t qtype = (uint16_t)((q[p] << 8) | q[p + 1]);
    uint16_t qclass = (uint16_t)((q[p + 2] << 8) | q[p + 3]);
    size_t qend = p + 4;
    int answer = qtype == 1 && qclass == 1;
    size_t need = qend + (answer ? 16 : 0);
    if (need > cap) {
        return 0;
    }
    memcpy(out, q, qend);
    out[2] = 0x84 | (q[2] & 0x01); /* response, authoritative, copy RD */
    out[3] = 0x00;                 /* RA=0, no error */
    out[6] = 0;
    out[7] = (uint8_t)answer; /* ANCOUNT */
    memset(out + 8, 0, 4);    /* NSCOUNT, ARCOUNT */
    if (answer) {
        uint8_t *a = out + qend;
        a[0] = 0xC0; /* name: pointer to the question */
        a[1] = DNS_HDR;
        a[2] = 0;
        a[3] = 1; /* A */
        a[4] = 0;
        a[5] = 1; /* IN */
        a[6] = 0;
        a[7] = 0;
        a[8] = 0;
        a[9] = 60; /* TTL 60 s */
        a[10] = 0;
        a[11] = 4;
        memcpy(a + 12, &ip, 4);
    }
    return need;
}
