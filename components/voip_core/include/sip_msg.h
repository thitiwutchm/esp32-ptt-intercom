/*
 * SIP message parsing (RFC 3261) for a small user agent: start line,
 * headers (compact forms understood), body. Pointers refer into the
 * caller's buffer, nothing is copied.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIP_MAX_HEADERS 48

typedef struct {
    const char *s;
    size_t len;
} sip_str_t;

typedef struct {
    sip_str_t name;
    sip_str_t value;
} sip_hdr_t;

typedef struct {
    bool is_request;
    sip_str_t method; /* request */
    sip_str_t uri;    /* request */
    int status;       /* response */
    sip_hdr_t hdrs[SIP_MAX_HEADERS];
    int nhdrs;
    sip_str_t body;
} sip_msg_t;

bool sip_parse(const char *buf, size_t len, sip_msg_t *m);

/* index-th header with this name (case-insensitive, compact form accepted). */
bool sip_get(const sip_msg_t *m, const char *name, int index, sip_str_t *value);

/* CSeq number and method. */
bool sip_cseq(const sip_msg_t *m, unsigned *num, sip_str_t *method);

bool sip_str_eq(sip_str_t a, const char *b);      /* case-sensitive */
bool sip_str_ieq(sip_str_t a, const char *b);     /* case-insensitive */
bool sip_str_icontains(sip_str_t a, const char *needle);

/* Copy into a NUL-terminated buffer (truncating). */
void sip_str_copy(sip_str_t a, char *out, size_t cap);

/* ";name=value" parameter of a header value, outside <...>. Quotes are stripped. */
bool sip_param(sip_str_t value, const char *name, char *out, size_t cap);

/* The URI of a name-addr ("Bob" <sip:200@pbx>;tag=x  ->  sip:200@pbx). */
bool sip_uri(sip_str_t value, sip_str_t *uri);

/* The name-addr without header parameters ("Bob" <sip:200@pbx>), used to rebuild From/To. */
sip_str_t sip_nameaddr(sip_str_t value);

/* Display name, or the user part of the URI when there is none. */
void sip_display(sip_str_t value, char *out, size_t cap);

/* Digest challenge field (realm, nonce, qop, opaque, algorithm) of WWW-/Proxy-Authenticate. */
bool sip_auth_field(sip_str_t value, const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
