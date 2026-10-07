#include "sip_msg.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    char compact;
    const char *full;
} kCompact[] = {
    {'v', "Via"}, {'f', "From"}, {'t', "To"}, {'i', "Call-ID"}, {'m', "Contact"},
    {'l', "Content-Length"}, {'c', "Content-Type"}, {'k', "Supported"},
};

static int ci_ncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d || !a[i]) {
            return d;
        }
    }
    return 0;
}

static sip_str_t trim(const char *s, size_t len)
{
    while (len && (*s == ' ' || *s == '\t')) {
        s++;
        len--;
    }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
        len--;
    }
    return (sip_str_t){s, len};
}

bool sip_str_eq(sip_str_t a, const char *b)
{
    size_t n = strlen(b);
    return a.len == n && memcmp(a.s, b, n) == 0;
}

bool sip_str_ieq(sip_str_t a, const char *b)
{
    size_t n = strlen(b);
    if (a.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a.s[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

bool sip_str_icontains(sip_str_t a, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= a.len; i++) {
        if (sip_str_ieq((sip_str_t){a.s + i, n}, needle)) {
            return true;
        }
    }
    return false;
}

void sip_str_copy(sip_str_t a, char *out, size_t cap)
{
    if (!cap) {
        return;
    }
    size_t n = a.len < cap - 1 ? a.len : cap - 1;
    memcpy(out, a.s, n);
    out[n] = '\0';
}

static bool name_matches(sip_str_t hdr, const char *name)
{
    if (sip_str_ieq(hdr, name)) {
        return true;
    }
    for (size_t i = 0; i < sizeof(kCompact) / sizeof(kCompact[0]); i++) {
        if (strlen(name) == strlen(kCompact[i].full) && ci_ncmp(name, kCompact[i].full, strlen(name)) == 0 && hdr.len == 1 &&
            tolower((unsigned char)hdr.s[0]) == kCompact[i].compact) {
            return true;
        }
    }
    return false;
}

bool sip_parse(const char *buf, size_t len, sip_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    const char *end = buf + len;
    const char *line = buf;
    const char *eol = memchr(line, '\n', len);
    if (!eol) {
        return false;
    }
    sip_str_t first = trim(line, (size_t)(eol - line));
    if (first.len > 8 && memcmp(first.s, "SIP/2.0 ", 8) == 0) {
        m->status = atoi(first.s + 8);
        if (m->status < 100 || m->status > 699) {
            return false;
        }
    } else {
        const char *sp1 = memchr(first.s, ' ', first.len);
        if (!sp1) {
            return false;
        }
        const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(first.s + first.len - sp1 - 1));
        if (!sp2 || !sip_str_eq(trim(sp2 + 1, (size_t)(first.s + first.len - sp2 - 1)), "SIP/2.0")) {
            return false;
        }
        m->is_request = true;
        m->method = (sip_str_t){first.s, (size_t)(sp1 - first.s)};
        m->uri = (sip_str_t){sp1 + 1, (size_t)(sp2 - sp1 - 1)};
    }

    line = eol + 1;
    while (line < end) {
        eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol) {
            eol = end;
        }
        sip_str_t l = trim(line, (size_t)(eol - line));
        if (l.len == 0) {
            line = eol + 1;
            break; /* end of headers */
        }
        const char *colon = memchr(l.s, ':', l.len);
        if (colon && m->nhdrs < SIP_MAX_HEADERS) {
            m->hdrs[m->nhdrs].name = trim(l.s, (size_t)(colon - l.s));
            m->hdrs[m->nhdrs].value = trim(colon + 1, (size_t)(l.s + l.len - colon - 1));
            m->nhdrs++;
        }
        line = eol + 1;
    }
    if (line > end) {
        line = end;
    }
    size_t body_len = (size_t)(end - line);
    sip_str_t cl;
    if (sip_get(m, "Content-Length", 0, &cl)) {
        size_t declared = (size_t)strtoul(cl.s, NULL, 10);
        if (declared < body_len) {
            body_len = declared;
        }
    }
    m->body = (sip_str_t){line, body_len};
    return true;
}

bool sip_get(const sip_msg_t *m, const char *name, int index, sip_str_t *value)
{
    for (int i = 0; i < m->nhdrs; i++) {
        if (name_matches(m->hdrs[i].name, name) && index-- == 0) {
            *value = m->hdrs[i].value;
            return true;
        }
    }
    return false;
}

bool sip_cseq(const sip_msg_t *m, unsigned *num, sip_str_t *method)
{
    sip_str_t v;
    if (!sip_get(m, "CSeq", 0, &v)) {
        return false;
    }
    char tmp[32];
    sip_str_copy(v, tmp, sizeof(tmp));
    char *sp;
    *num = (unsigned)strtoul(tmp, &sp, 10);
    const char *meth = v.s + (sp - tmp);
    *method = trim(meth, (size_t)(v.s + v.len - meth));
    return method->len > 0;
}

bool sip_param(sip_str_t value, const char *name, char *out, size_t cap)
{
    const char *p = value.s;
    const char *end = value.s + value.len;
    /* Parameters after the closing '>' if there is one. */
    const char *gt = memchr(p, '>', value.len);
    if (gt) {
        p = gt + 1;
    }
    size_t n = strlen(name);
    while (p < end) {
        const char *semi = memchr(p, ';', (size_t)(end - p));
        if (!semi) {
            break;
        }
        p = semi + 1;
        sip_str_t rest = trim(p, (size_t)(end - p));
        if (rest.len >= n && ci_ncmp(rest.s, name, n) == 0) {
            const char *q = rest.s + n;
            if (q < end && *q == '=') {
                q++;
                const char *e = q;
                while (e < end && *e != ';' && *e != ',' && *e != ' ') {
                    e++;
                }
                sip_str_t v = {q, (size_t)(e - q)};
                if (v.len >= 2 && v.s[0] == '"' && v.s[v.len - 1] == '"') {
                    v.s++;
                    v.len -= 2;
                }
                sip_str_copy(v, out, cap);
                return true;
            }
            if (q >= end || *q == ';') {
                if (cap) {
                    out[0] = '\0';
                }
                return true; /* flag parameter */
            }
        }
    }
    return false;
}

bool sip_uri(sip_str_t value, sip_str_t *uri)
{
    const char *lt = memchr(value.s, '<', value.len);
    if (lt) {
        const char *gt = memchr(lt, '>', (size_t)(value.s + value.len - lt));
        if (!gt) {
            return false;
        }
        *uri = (sip_str_t){lt + 1, (size_t)(gt - lt - 1)};
        return true;
    }
    /* addr-spec without brackets: parameters after ';' belong to the header. */
    const char *semi = memchr(value.s, ';', value.len);
    *uri = trim(value.s, semi ? (size_t)(semi - value.s) : value.len);
    return uri->len > 0;
}

sip_str_t sip_nameaddr(sip_str_t value)
{
    const char *gt = memchr(value.s, '>', value.len);
    if (gt) {
        return (sip_str_t){value.s, (size_t)(gt - value.s + 1)};
    }
    const char *semi = memchr(value.s, ';', value.len);
    return trim(value.s, semi ? (size_t)(semi - value.s) : value.len);
}

void sip_display(sip_str_t value, char *out, size_t cap)
{
    const char *q1 = memchr(value.s, '"', value.len);
    const char *lt = memchr(value.s, '<', value.len);
    if (q1 && (!lt || q1 < lt)) {
        const char *q2 = memchr(q1 + 1, '"', (size_t)(value.s + value.len - q1 - 1));
        if (q2 && q2 > q1 + 1) {
            sip_str_copy((sip_str_t){q1 + 1, (size_t)(q2 - q1 - 1)}, out, cap);
            return;
        }
    } else if (lt && lt > value.s) {
        sip_str_t name = trim(value.s, (size_t)(lt - value.s));
        if (name.len) {
            sip_str_copy(name, out, cap);
            return;
        }
    }
    sip_str_t uri;
    if (sip_uri(value, &uri)) {
        const char *colon = memchr(uri.s, ':', uri.len);
        const char *user = colon ? colon + 1 : uri.s;
        const char *at = memchr(user, '@', (size_t)(uri.s + uri.len - user));
        if (at) {
            sip_str_copy((sip_str_t){user, (size_t)(at - user)}, out, cap);
            return;
        }
    }
    sip_str_copy(value, out, cap);
}

bool sip_auth_field(sip_str_t value, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    const char *p = value.s;
    const char *end = value.s + value.len;
    /* Skip the scheme ("Digest"). */
    while (p < end && *p != ' ') {
        p++;
    }
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',')) {
            p++;
        }
        const char *key = p;
        while (p < end && *p != '=') {
            p++;
        }
        sip_str_t k = trim(key, (size_t)(p - key));
        if (p >= end) {
            break;
        }
        p++; /* '=' */
        sip_str_t v;
        if (p < end && *p == '"') {
            const char *q = memchr(p + 1, '"', (size_t)(end - p - 1));
            if (!q) {
                return false;
            }
            v = (sip_str_t){p + 1, (size_t)(q - p - 1)};
            p = q + 1;
        } else {
            const char *s = p;
            while (p < end && *p != ',') {
                p++;
            }
            v = trim(s, (size_t)(p - s));
        }
        if (k.len == n && ci_ncmp(k.s, name, n) == 0) {
            sip_str_copy(v, out, cap);
            return true;
        }
    }
    return false;
}
