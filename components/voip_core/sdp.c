#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ip_parse(const char *s, size_t len, uint32_t *ip)
{
    unsigned part[4];
    char tmp[16];
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    char extra;
    if (sscanf(tmp, "%u.%u.%u.%u%c", &part[0], &part[1], &part[2], &part[3], &extra) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (part[i] > 255) {
            return false;
        }
    }
    /* Network byte order in memory regardless of host endianness. */
    uint8_t b[4] = {(uint8_t)part[0], (uint8_t)part[1], (uint8_t)part[2], (uint8_t)part[3]};
    memcpy(ip, b, 4);
    return true;
}

void ip_format(uint32_t ip, char out[16])
{
    uint8_t b[4];
    memcpy(b, &ip, 4);
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

size_t sdp_build(char *out, size_t cap, uint32_t local_ip, uint16_t rtp_port, uint32_t session_id, int pt)
{
    char ip[16];
    ip_format(local_ip, ip);
    int n;
    if (pt < 0) {
        n = snprintf(out, cap,
                     "v=0\r\n"
                     "o=- %lu %lu IN IP4 %s\r\n"
                     "s=esp32-ptt-intercom\r\n"
                     "c=IN IP4 %s\r\n"
                     "t=0 0\r\n"
                     "m=audio %u RTP/AVP 0 8\r\n"
                     "a=rtpmap:0 PCMU/8000\r\n"
                     "a=rtpmap:8 PCMA/8000\r\n"
                     "a=ptime:20\r\n"
                     "a=sendrecv\r\n",
                     (unsigned long)session_id, (unsigned long)session_id, ip, ip, rtp_port);
    } else {
        n = snprintf(out, cap,
                     "v=0\r\n"
                     "o=- %lu %lu IN IP4 %s\r\n"
                     "s=esp32-ptt-intercom\r\n"
                     "c=IN IP4 %s\r\n"
                     "t=0 0\r\n"
                     "m=audio %u RTP/AVP %d\r\n"
                     "a=rtpmap:%d %s/8000\r\n"
                     "a=ptime:20\r\n"
                     "a=sendrecv\r\n",
                     (unsigned long)session_id, (unsigned long)session_id, ip, ip, rtp_port, pt, pt,
                     pt == SDP_PT_PCMA ? "PCMA" : "PCMU");
    }
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

bool sdp_parse(const char *body, size_t len, sdp_media_t *m)
{
    memset(m, 0, sizeof(*m));
    uint32_t session_ip = 0, media_ip = 0;
    bool in_audio = false, seen_audio = false;
    const char *p = body, *end = body + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) {
            eol = end;
        }
        size_t l = (size_t)(eol - p);
        if (l && p[l - 1] == '\r') {
            l--;
        }
        if (l > 2 && p[1] == '=') {
            if (p[0] == 'm') {
                in_audio = l > 8 && memcmp(p, "m=audio ", 8) == 0 && !seen_audio;
                if (in_audio) {
                    seen_audio = true;
                    char line[160];
                    size_t n = l < sizeof(line) - 1 ? l : sizeof(line) - 1;
                    memcpy(line, p, n);
                    line[n] = '\0';
                    char *s = line + 8;
                    m->port = (uint16_t)strtoul(s, &s, 10);
                    /* skip protocol */
                    while (*s == ' ') s++;
                    while (*s && *s != ' ') s++;
                    while (*s && m->npts < 8) {
                        char *e;
                        long pt = strtol(s, &e, 10);
                        if (e == s) {
                            break;
                        }
                        m->pts[m->npts++] = (int)pt;
                        s = e;
                    }
                }
            } else if (p[0] == 'c' && l > 9 && memcmp(p, "c=IN IP4 ", 9) == 0) {
                const char *a = p + 9;
                size_t al = l - 9;
                const char *slash = memchr(a, '/', al);
                if (slash) {
                    al = (size_t)(slash - a);
                }
                uint32_t ip;
                if (ip_parse(a, al, &ip)) {
                    if (in_audio) {
                        media_ip = ip;
                    } else if (!seen_audio) {
                        session_ip = ip;
                    }
                }
            }
        }
        p = eol + 1;
    }
    m->ip = media_ip ? media_ip : session_ip;
    return seen_audio && m->ip && m->port;
}

int sdp_choose_pt(const sdp_media_t *m)
{
    for (int i = 0; i < m->npts; i++) {
        if (m->pts[i] == SDP_PT_PCMU || m->pts[i] == SDP_PT_PCMA) {
            return m->pts[i];
        }
    }
    return -1;
}
