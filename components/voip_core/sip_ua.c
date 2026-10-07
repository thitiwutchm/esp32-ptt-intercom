#include "sip_ua.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"
#include "sip_msg.h"

#define T1_MS 500
#define T2_MS 4000
#define TIMEOUT_MS (64 * T1_MS)        /* timers B and F: 32 s */
#define INVITE_WAIT_FINAL_MS 180000    /* after a provisional response */
#define RING_TIMEOUT_MS 120000         /* incoming call nobody answers */
#define REG_RETRY_MS 30000
#define USER_AGENT "esp32-ptt-intercom"
#define ALLOW "INVITE, ACK, CANCEL, BYE, OPTIONS, UPDATE, INFO, NOTIFY"

/* ------------------------------------------------------------------ string building */

typedef struct {
    char *p;
    size_t cap;
    size_t len;
    bool overflow;
} sb_t;

static void sb_init(sb_t *b, char *buf, size_t cap)
{
    b->p = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = false;
    if (cap) {
        buf[0] = '\0';
    }
}

static void sb_printf(sb_t *b, const char *fmt, ...)
{
    if (b->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->overflow = true;
        b->p[b->len] = '\0';
        return;
    }
    b->len += (size_t)n;
}

static void hex_id(sip_ua_t *ua, char *out, size_t cap, int words)
{
    size_t off = 0;
    for (int i = 0; i < words && off + 9 <= cap; i++) {
        off += (size_t)snprintf(out + off, cap - off, "%08lx", (unsigned long)ua->ops.random(ua->ops.ctx));
    }
}

static void new_branch(sip_ua_t *ua, char *out, size_t cap)
{
    char id[17];
    hex_id(ua, id, sizeof(id), 2);
    snprintf(out, cap, "z9hG4bK%s", id);
}

static void local_ip_str(const sip_ua_t *ua, char out[16])
{
    ip_format(ua->cfg.local_ip, out);
}

static void send_to(sip_ua_t *ua, const char *buf, size_t len, uint32_t ip, uint16_t port)
{
    if (len) {
        ua->ops.send(ua->ops.ctx, buf, len, ip, port);
    }
}

/* ------------------------------------------------------------------ transactions */

static void tx_start(sip_ua_t *ua, sip_tx_t *t, const char *buf, size_t len, uint32_t ip, uint16_t port,
                     bool invite, bool retransmit)
{
    if (len > sizeof(t->buf)) {
        len = 0;
    }
    memcpy(t->buf, buf, len);
    t->len = len;
    t->ip = ip;
    t->port = port;
    t->invite = invite;
    t->active = len > 0;
    t->interval_ms = retransmit ? T1_MS : 0;
    t->next_ms = ua->now_ms + T1_MS;
    t->deadline_ms = ua->now_ms + TIMEOUT_MS;
    send_to(ua, t->buf, t->len, ip, port);
}

static void tx_stop(sip_tx_t *t)
{
    t->active = false;
}

/* Returns true when the transaction timed out (and was stopped). */
static bool tx_tick(sip_ua_t *ua, sip_tx_t *t)
{
    if (!t->active) {
        return false;
    }
    if ((int32_t)(ua->now_ms - t->deadline_ms) >= 0) {
        t->active = false;
        return true;
    }
    if (t->interval_ms && (int32_t)(ua->now_ms - t->next_ms) >= 0) {
        send_to(ua, t->buf, t->len, t->ip, t->port);
        t->interval_ms *= 2;
        if (!t->invite && t->interval_ms > T2_MS) {
            t->interval_ms = T2_MS;
        }
        t->next_ms = ua->now_ms + t->interval_ms;
    }
    return false;
}

/* ------------------------------------------------------------------ messages */

static size_t build_request(sip_ua_t *ua, char *out, size_t cap, const char *method, const char *ruri,
                            const char *branch, const char *from, const char *to, const char *callid,
                            unsigned cseq, const char *extra, const char *body)
{
    char ip[16];
    local_ip_str(ua, ip);
    sb_t b;
    sb_init(&b, out, cap);
    sb_printf(&b, "%s %s SIP/2.0\r\n", method, ruri);
    sb_printf(&b, "Via: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n", ip, ua->cfg.local_port, branch);
    sb_printf(&b, "Max-Forwards: 70\r\n");
    sb_printf(&b, "From: %s\r\n", from);
    sb_printf(&b, "To: %s\r\n", to);
    sb_printf(&b, "Call-ID: %s\r\n", callid);
    sb_printf(&b, "CSeq: %u %s\r\n", cseq, method);
    if (strcmp(method, "ACK") != 0 && strcmp(method, "CANCEL") != 0) {
        sb_printf(&b, "Contact: <sip:%s@%s:%u>\r\n", ua->cfg.user, ip, ua->cfg.local_port);
        sb_printf(&b, "User-Agent: " USER_AGENT "\r\n");
    }
    if (extra) {
        sb_printf(&b, "%s", extra);
    }
    if (body && *body) {
        sb_printf(&b, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    } else {
        sb_printf(&b, "Content-Length: 0\r\n\r\n");
    }
    return b.overflow ? 0 : b.len;
}

static bool ids_from_msg(const sip_msg_t *m, sip_req_ids_t *r)
{
    sb_t b;
    sb_init(&b, r->vias, sizeof(r->vias));
    sip_str_t v;
    for (int i = 0; sip_get(m, "Via", i, &v); i++) {
        sb_printf(&b, "Via: %.*s\r\n", (int)v.len, v.s);
    }
    if (b.overflow || b.len == 0) {
        return false;
    }
    if (!sip_get(m, "From", 0, &v)) {
        return false;
    }
    sip_str_copy(v, r->from, sizeof(r->from));
    if (!sip_get(m, "To", 0, &v)) {
        return false;
    }
    sip_str_copy(v, r->to, sizeof(r->to));
    if (!sip_get(m, "Call-ID", 0, &v)) {
        return false;
    }
    sip_str_copy(v, r->callid, sizeof(r->callid));
    if (!sip_get(m, "CSeq", 0, &v)) {
        return false;
    }
    sip_str_copy(v, r->cseq, sizeof(r->cseq));
    return true;
}

static const char *reason_phrase(int code)
{
    switch (code) {
    case 100: return "Trying";
    case 180: return "Ringing";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 405: return "Method Not Allowed";
    case 480: return "Temporarily Unavailable";
    case 481: return "Call/Transaction Does Not Exist";
    case 486: return "Busy Here";
    case 487: return "Request Terminated";
    case 488: return "Not Acceptable Here";
    case 501: return "Not Implemented";
    case 603: return "Decline";
    default: return "Unknown";
    }
}

/* to_tag is added to To when it has none (every response but 100). */
static size_t build_response(sip_ua_t *ua, char *out, size_t cap, const sip_req_ids_t *r, int code,
                             const char *to_tag, const char *extra, const char *body)
{
    char ip[16];
    local_ip_str(ua, ip);
    sb_t b;
    sb_init(&b, out, cap);
    sb_printf(&b, "SIP/2.0 %d %s\r\n", code, reason_phrase(code));
    sb_printf(&b, "%s", r->vias);
    sb_printf(&b, "From: %s\r\n", r->from);
    char existing[64];
    sip_str_t to = {r->to, strlen(r->to)};
    if (to_tag && code > 100 && !sip_param(to, "tag", existing, sizeof(existing))) {
        sb_printf(&b, "To: %s;tag=%s\r\n", r->to, to_tag);
    } else {
        sb_printf(&b, "To: %s\r\n", r->to);
    }
    sb_printf(&b, "Call-ID: %s\r\n", r->callid);
    sb_printf(&b, "CSeq: %s\r\n", r->cseq);
    sb_printf(&b, "User-Agent: " USER_AGENT "\r\n");
    if (extra) {
        sb_printf(&b, "%s", extra);
    }
    if (body && *body) {
        sb_printf(&b, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(body), body);
    } else {
        sb_printf(&b, "Content-Length: 0\r\n\r\n");
    }
    return b.overflow ? 0 : b.len;
}

static void respond(sip_ua_t *ua, const sip_msg_t *req, int code, const char *to_tag, const char *extra,
                    uint32_t ip, uint16_t port)
{
    sip_req_ids_t ids;
    char out[SIP_BUF_SIZE];
    if (!ids_from_msg(req, &ids)) {
        return;
    }
    send_to(ua, out, build_response(ua, out, sizeof(out), &ids, code, to_tag, extra, NULL), ip, port);
}

/*
 * Digest authorization header for a 401/407 challenge (RFC 2617, MD5, qop=auth
 * or none). Writes the whole header line including CRLF.
 */
static bool make_auth(sip_ua_t *ua, const sip_msg_t *resp, const char *method, const char *uri, char *out,
                      size_t cap)
{
    bool proxy = resp->status == 407;
    sip_str_t ch;
    if (!sip_get(resp, proxy ? "Proxy-Authenticate" : "WWW-Authenticate", 0, &ch)) {
        return false;
    }
    char realm[96] = "", nonce[128] = "", qop[32] = "", opaque[96] = "", algo[24] = "MD5";
    sip_auth_field(ch, "realm", realm, sizeof(realm));
    if (!sip_auth_field(ch, "nonce", nonce, sizeof(nonce))) {
        return false;
    }
    sip_auth_field(ch, "qop", qop, sizeof(qop));
    bool has_opaque = sip_auth_field(ch, "opaque", opaque, sizeof(opaque));
    sip_auth_field(ch, "algorithm", algo, sizeof(algo));
    if (strcmp(algo, "MD5") != 0 && strcmp(algo, "md5") != 0) {
        return false;
    }
    bool use_qop = strstr(qop, "auth") != NULL;

    char ha1[33], ha2[33], response[33], cnonce[17];
    md5_hex(ha1, 5, ua->cfg.user, ":", realm, ":", ua->cfg.password);
    md5_hex(ha2, 3, method, ":", uri);
    hex_id(ua, cnonce, sizeof(cnonce), 2);
    const char *nc = "00000001";
    if (use_qop) {
        md5_hex(response, 11, ha1, ":", nonce, ":", nc, ":", cnonce, ":", "auth", ":", ha2);
    } else {
        md5_hex(response, 5, ha1, ":", nonce, ":", ha2);
    }

    sb_t b;
    sb_init(&b, out, cap);
    sb_printf(&b, "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
              proxy ? "Proxy-Authorization" : "Authorization", ua->cfg.user, realm, nonce, uri, response);
    if (use_qop) {
        sb_printf(&b, ", qop=auth, nc=%s, cnonce=\"%s\"", nc, cnonce);
    }
    if (has_opaque) {
        sb_printf(&b, ", opaque=\"%s\"", opaque);
    }
    sb_printf(&b, "\r\n");
    return !b.overflow;
}

/* ------------------------------------------------------------------ notifications */

static void set_reg(sip_ua_t *ua, sip_reg_state_t st, int status)
{
    bool changed = ua->reg_state != st;
    ua->reg_state = st;
    if (changed && ua->ops.reg_changed) {
        ua->ops.reg_changed(ua->ops.ctx, st, status);
    }
}

static void notify_call(sip_ua_t *ua)
{
    if (ua->ops.call_changed) {
        ua->ops.call_changed(ua->ops.ctx, &ua->info);
    }
}

static void call_ended(sip_ua_t *ua, sip_end_reason_t reason, int status)
{
    if (ua->info.state == SIP_CALL_IDLE) {
        return;
    }
    strncpy(ua->ended_callid, ua->callid, sizeof(ua->ended_callid) - 1);
    ua->info.state = SIP_CALL_IDLE;
    ua->info.reason = reason;
    ua->info.status = status;
    notify_call(ua);
}

static void set_media(sip_ua_t *ua, const sdp_media_t *m, int pt)
{
    ua->info.media_ip = m->ip;
    ua->info.media_port = m->port;
    ua->info.pt = pt;
    ua->pt = pt;
}

/* ------------------------------------------------------------------ registration */

static void send_register(sip_ua_t *ua, const char *auth)
{
    char ruri[96], from[160], extra[512];
    snprintf(ruri, sizeof(ruri), "sip:%s", ua->cfg.domain);
    snprintf(from, sizeof(from), "<sip:%s@%s>;tag=%s", ua->cfg.user, ua->cfg.domain, ua->reg_tag);
    char to[128];
    snprintf(to, sizeof(to), "<sip:%s@%s>", ua->cfg.user, ua->cfg.domain);
    snprintf(extra, sizeof(extra), "Expires: %d\r\nAllow: " ALLOW "\r\n%s", ua->cfg.expires, auth ? auth : "");
    char branch[40];
    new_branch(ua, branch, sizeof(branch));
    char out[SIP_BUF_SIZE];
    size_t n = build_request(ua, out, sizeof(out), "REGISTER", ruri, branch, from, to, ua->reg_callid,
                             ++ua->reg_cseq, extra, NULL);
    tx_start(ua, &ua->reg_tx, out, n, ua->cfg.server_ip, ua->cfg.server_port, false, true);
    ua->reg_due_valid = false;
    if (ua->reg_state != SIP_REG_OK) {
        set_reg(ua, SIP_REG_TRYING, 0);
    }
}

void sip_ua_register(sip_ua_t *ua, uint32_t now_ms)
{
    ua->now_ms = now_ms;
    ua->reg_auth_tries = 0;
    send_register(ua, NULL);
}

static void reg_retry_later(sip_ua_t *ua, int status)
{
    tx_stop(&ua->reg_tx);
    set_reg(ua, SIP_REG_FAILED, status);
    ua->reg_due_valid = true;
    ua->reg_due_ms = ua->now_ms + REG_RETRY_MS;
}

static void on_register_response(sip_ua_t *ua, const sip_msg_t *m)
{
    if (m->status < 200) {
        return;
    }
    if (m->status == 401 || m->status == 407) {
        char auth[600], ruri[96];
        snprintf(ruri, sizeof(ruri), "sip:%s", ua->cfg.domain);
        if (ua->reg_auth_tries++ < 2 && make_auth(ua, m, "REGISTER", ruri, auth, sizeof(auth))) {
            send_register(ua, auth);
        } else {
            reg_retry_later(ua, m->status);
        }
        return;
    }
    if (m->status >= 300) {
        reg_retry_later(ua, m->status);
        return;
    }
    tx_stop(&ua->reg_tx);
    ua->reg_auth_tries = 0;
    int expires = ua->cfg.expires;
    sip_str_t v;
    char tmp[16];
    if (sip_get(m, "Contact", 0, &v) && sip_param(v, "expires", tmp, sizeof(tmp))) {
        expires = atoi(tmp);
    } else if (sip_get(m, "Expires", 0, &v)) {
        sip_str_copy(v, tmp, sizeof(tmp));
        expires = atoi(tmp);
    }
    if (expires < 25) {
        expires = 25;
    }
    ua->reg_due_valid = true;
    ua->reg_due_ms = ua->now_ms + (uint32_t)expires * 800u; /* refresh at 80 % */
    set_reg(ua, SIP_REG_OK, m->status);
}

/* ------------------------------------------------------------------ outgoing call (UAC) */

static void uac_headers(sip_ua_t *ua, char *from, size_t fcap, char *to, size_t tcap, bool with_remote_tag)
{
    snprintf(from, fcap, "%s;tag=%s", ua->local_uri, ua->local_tag);
    if (with_remote_tag && ua->remote_tag[0]) {
        snprintf(to, tcap, "%s;tag=%s", ua->remote_uri, ua->remote_tag);
    } else {
        snprintf(to, tcap, "%s", ua->remote_uri);
    }
}

static void send_invite(sip_ua_t *ua, const char *auth)
{
    char sdp[600], from[280], to[280], extra[700], out[SIP_BUF_SIZE];
    sdp_build(sdp, sizeof(sdp), ua->cfg.local_ip, ua->cfg.rtp_port, ua->session_id, -1);
    uac_headers(ua, from, sizeof(from), to, sizeof(to), false);
    snprintf(extra, sizeof(extra), "Allow: " ALLOW "\r\n%s", auth ? auth : "");
    new_branch(ua, ua->invite_branch, sizeof(ua->invite_branch));
    size_t n = build_request(ua, out, sizeof(out), "INVITE", ua->request_uri, ua->invite_branch, from, to,
                             ua->callid, ua->invite_cseq, extra, sdp);
    ua->got_provisional = false;
    tx_start(ua, &ua->call_tx, out, n, ua->cfg.server_ip, ua->cfg.server_port, true, true);
}

/* ACK for a non-2xx final response: same branch and To as the response. */
static void ack_error(sip_ua_t *ua, const sip_msg_t *resp)
{
    char from[280], to[280], out[SIP_BUF_SIZE];
    sip_str_t v;
    uac_headers(ua, from, sizeof(from), to, sizeof(to), false);
    if (sip_get(resp, "To", 0, &v)) {
        sip_str_copy(v, to, sizeof(to));
    }
    size_t n = build_request(ua, out, sizeof(out), "ACK", ua->request_uri, ua->invite_branch, from, to, ua->callid,
                             ua->invite_cseq, NULL, NULL);
    send_to(ua, out, n, ua->cfg.server_ip, ua->cfg.server_port);
}

static void send_cancel(sip_ua_t *ua)
{
    char from[280], to[280], out[SIP_BUF_SIZE];
    uac_headers(ua, from, sizeof(from), to, sizeof(to), false);
    size_t n = build_request(ua, out, sizeof(out), "CANCEL", ua->request_uri, ua->invite_branch, from, to,
                             ua->callid, ua->invite_cseq, NULL, NULL);
    tx_start(ua, &ua->aux_tx, out, n, ua->cfg.server_ip, ua->cfg.server_port, false, true);
    ua->cancel_pending = false;
}

/* Where in-dialog requests go: the remote Contact, else the remote URI. */
static const char *dialog_target(sip_ua_t *ua)
{
    if (!ua->remote_target[0]) {
        sip_str_t uri;
        if (sip_uri((sip_str_t){ua->remote_uri, strlen(ua->remote_uri)}, &uri)) {
            sip_str_copy(uri, ua->remote_target, sizeof(ua->remote_target));
        } else {
            strncpy(ua->remote_target, ua->request_uri, sizeof(ua->remote_target) - 1);
        }
    }
    return ua->remote_target;
}

static void send_bye(sip_ua_t *ua)
{
    char from[280], to[280], branch[40], out[SIP_BUF_SIZE];
    snprintf(from, sizeof(from), "%s;tag=%s", ua->local_uri, ua->local_tag);
    snprintf(to, sizeof(to), "%s;tag=%s", ua->remote_uri, ua->remote_tag);
    new_branch(ua, branch, sizeof(branch));
    size_t n = build_request(ua, out, sizeof(out), "BYE", dialog_target(ua), branch, from, to, ua->callid,
                             ++ua->local_cseq, NULL, NULL);
    /* A call we answered: hang up towards where it came from (a softphone calling us
     * directly may not listen on the PBX port). Calls we placed go through the PBX. */
    uint32_t ip = ua->uas ? ua->invite_src_ip : ua->cfg.server_ip;
    uint16_t port = ua->uas ? ua->invite_src_port : ua->cfg.server_port;
    tx_start(ua, &ua->aux_tx, out, n, ip, port, false, true);
}

static void on_invite_response(sip_ua_t *ua, const sip_msg_t *m)
{
    sip_str_t v;
    if (m->status < 200) {
        if (!ua->got_provisional) {
            ua->got_provisional = true;
            ua->call_tx.interval_ms = 0; /* stop retransmitting the INVITE */
            ua->call_tx.deadline_ms = ua->now_ms + INVITE_WAIT_FINAL_MS;
        }
        if (ua->cancel_pending) {
            send_cancel(ua);
        }
        if ((m->status == 180 || m->status == 183) && ua->info.state == SIP_CALL_OUTGOING) {
            ua->info.state = SIP_CALL_RINGBACK;
            notify_call(ua);
        }
        return;
    }

    if (m->status < 300) {
        char tag[64] = "";
        if (sip_get(m, "To", 0, &v)) {
            sip_param(v, "tag", tag, sizeof(tag));
        }
        bool retransmission = ua->ack_len && strcmp(tag, ua->remote_tag) == 0;
        if (retransmission) {
            send_to(ua, ua->ack, ua->ack_len, ua->cfg.server_ip, ua->cfg.server_port);
            return;
        }
        tx_stop(&ua->call_tx);
        strncpy(ua->remote_tag, tag, sizeof(ua->remote_tag) - 1);
        sip_str_t uri;
        if (sip_get(m, "Contact", 0, &v) && sip_uri(v, &uri)) {
            sip_str_copy(uri, ua->remote_target, sizeof(ua->remote_target));
        }
        char from[280], to[280], branch[40];
        uac_headers(ua, from, sizeof(from), to, sizeof(to), true);
        new_branch(ua, branch, sizeof(branch));
        ua->ack_len = build_request(ua, ua->ack, sizeof(ua->ack), "ACK", dialog_target(ua), branch, from, to,
                                    ua->callid, ua->invite_cseq, NULL, NULL);
        send_to(ua, ua->ack, ua->ack_len, ua->cfg.server_ip, ua->cfg.server_port);

        sdp_media_t media;
        int pt = -1;
        if (sdp_parse(m->body.s, m->body.len, &media)) {
            pt = sdp_choose_pt(&media);
        }
        if (ua->info.state == SIP_CALL_IDLE || ua->cancel_pending || pt < 0) {
            /* We hung up meanwhile, or no usable codec: end the dialog at once. */
            ua->cancel_pending = false;
            send_bye(ua);
            call_ended(ua, pt < 0 ? SIP_END_FAILED : SIP_END_LOCAL, pt < 0 ? 488 : 0);
            return;
        }
        set_media(ua, &media, pt);
        ua->info.state = SIP_CALL_ACTIVE;
        notify_call(ua);
        return;
    }

    /* 3xx-6xx */
    ack_error(ua, m);
    tx_stop(&ua->call_tx);
    if ((m->status == 401 || m->status == 407) && ua->invite_auth_tries++ < 2 && ua->info.state != SIP_CALL_IDLE &&
        !ua->cancel_pending) {
        char auth[600];
        if (make_auth(ua, m, "INVITE", ua->request_uri, auth, sizeof(auth))) {
            ua->invite_cseq = ++ua->local_cseq;
            send_invite(ua, auth);
            return;
        }
    }
    ua->cancel_pending = false;
    sip_end_reason_t reason = SIP_END_FAILED;
    switch (m->status) {
    case 486:
    case 600:
        reason = SIP_END_BUSY;
        break;
    case 603:
        reason = SIP_END_REJECTED;
        break;
    case 404:
    case 408:
    case 480:
        reason = SIP_END_UNREACHABLE;
        break;
    case 487:
        reason = SIP_END_LOCAL;
        break;
    }
    call_ended(ua, reason, m->status);
}

bool sip_ua_call(sip_ua_t *ua, const char *target, uint32_t now_ms)
{
    ua->now_ms = now_ms;
    /* A finished call's CANCEL/BYE may still retransmit in aux_tx; only an open INVITE transaction blocks. */
    if (ua->info.state != SIP_CALL_IDLE || ua->call_tx.active || !target || !*target) {
        return false;
    }
    memset(&ua->info, 0, sizeof(ua->info));
    ua->uas = false;
    char ip[16], id[25];
    local_ip_str(ua, ip);
    hex_id(ua, id, sizeof(id), 3);
    snprintf(ua->callid, sizeof(ua->callid), "%s@%s", id, ip);
    hex_id(ua, ua->local_tag, sizeof(ua->local_tag), 1);
    ua->remote_tag[0] = '\0';
    ua->remote_target[0] = '\0';
    if (ua->cfg.display[0]) {
        snprintf(ua->local_uri, sizeof(ua->local_uri), "\"%s\" <sip:%s@%s>", ua->cfg.display, ua->cfg.user,
                 ua->cfg.domain);
    } else {
        snprintf(ua->local_uri, sizeof(ua->local_uri), "<sip:%s@%s>", ua->cfg.user, ua->cfg.domain);
    }
    snprintf(ua->remote_uri, sizeof(ua->remote_uri), "<sip:%s@%s>", target, ua->cfg.domain);
    snprintf(ua->request_uri, sizeof(ua->request_uri), "sip:%s@%s", target, ua->cfg.domain);
    ua->local_cseq = 1;
    ua->invite_cseq = 1;
    ua->invite_auth_tries = 0;
    ua->cancel_pending = false;
    ua->ack_len = 0;
    ua->wait_ack = false;
    ua->session_id = ua->ops.random(ua->ops.ctx) & 0x7fffffff;
    ua->session_ver = ua->session_id;
    strncpy(ua->info.peer, target, sizeof(ua->info.peer) - 1);
    ua->info.state = SIP_CALL_OUTGOING;
    send_invite(ua, NULL);
    notify_call(ua);
    return true;
}

/* ------------------------------------------------------------------ incoming call (UAS) */

static void uas_final(sip_ua_t *ua, int code, const char *extra, const char *body)
{
    char out[SIP_BUF_SIZE];
    size_t n = build_response(ua, out, sizeof(out), &ua->invite_ids, code, ua->local_tag, extra, body);
    tx_start(ua, &ua->call_tx, out, n, ua->invite_src_ip, ua->invite_src_port, false, true);
    ua->wait_ack = true;
}

static void our_sdp(sip_ua_t *ua, char *sdp, size_t cap)
{
    sdp_build(sdp, cap, ua->cfg.local_ip, ua->cfg.rtp_port, ua->session_id, ua->pt);
}

static void contact_hdr(sip_ua_t *ua, char *out, size_t cap)
{
    char ip[16];
    local_ip_str(ua, ip);
    snprintf(out, cap, "Contact: <sip:%s@%s:%u>\r\nAllow: " ALLOW "\r\n", ua->cfg.user, ip, ua->cfg.local_port);
}

static bool wants_auto_answer(const sip_msg_t *m)
{
    sip_str_t v;
    for (int i = 0; sip_get(m, "Call-Info", i, &v); i++) {
        if (sip_str_icontains(v, "answer-after")) {
            return true;
        }
    }
    for (int i = 0; sip_get(m, "Alert-Info", i, &v); i++) {
        if (sip_str_icontains(v, "auto") || sip_str_icontains(v, "answer") || sip_str_icontains(v, "intercom")) {
            return true;
        }
    }
    return false;
}

static void on_new_invite(sip_ua_t *ua, const sip_msg_t *m, uint32_t ip, uint16_t port)
{
    if (ua->info.state != SIP_CALL_IDLE || ua->call_tx.active ||
        (ua->ops.is_busy && ua->ops.is_busy(ua->ops.ctx))) {
        char tag[20];
        hex_id(ua, tag, sizeof(tag), 1);
        respond(ua, m, 486, tag, NULL, ip, port);
        return;
    }
    sdp_media_t media;
    int pt = -1;
    if (sdp_parse(m->body.s, m->body.len, &media)) {
        pt = sdp_choose_pt(&media);
    }
    char tag[20];
    hex_id(ua, tag, sizeof(tag), 1);
    if (pt < 0) {
        respond(ua, m, 488, tag, NULL, ip, port);
        return;
    }
    sip_req_ids_t ids;
    if (!ids_from_msg(m, &ids)) {
        return;
    }

    memset(&ua->info, 0, sizeof(ua->info));
    ua->uas = true;
    ua->invite_ids = ids;
    ua->invite_src_ip = ip;
    ua->invite_src_port = port;
    strncpy(ua->callid, ids.callid, sizeof(ua->callid) - 1);
    ua->callid[sizeof(ua->callid) - 1] = '\0';
    strncpy(ua->local_tag, tag, sizeof(ua->local_tag));
    sip_str_t v;
    ua->remote_tag[0] = '\0';
    if (sip_get(m, "From", 0, &v)) {
        sip_param(v, "tag", ua->remote_tag, sizeof(ua->remote_tag));
        sip_str_copy(sip_nameaddr(v), ua->remote_uri, sizeof(ua->remote_uri));
        sip_display(v, ua->info.peer, sizeof(ua->info.peer));
    }
    if (sip_get(m, "To", 0, &v)) {
        sip_str_copy(sip_nameaddr(v), ua->local_uri, sizeof(ua->local_uri));
    }
    ua->remote_target[0] = '\0';
    sip_str_t uri;
    if (sip_get(m, "Contact", 0, &v) && sip_uri(v, &uri)) {
        sip_str_copy(uri, ua->remote_target, sizeof(ua->remote_target));
    }
    sip_str_copy(m->uri, ua->request_uri, sizeof(ua->request_uri));
    unsigned cseq;
    sip_str_t method;
    sip_cseq(m, &cseq, &method);
    ua->remote_invite_cseq = cseq;
    ua->remote_cseq = cseq;
    ua->local_cseq = 1;
    ua->wait_ack = false;
    ua->session_id = ua->ops.random(ua->ops.ctx) & 0x7fffffff;
    ua->session_ver = ua->session_id;
    set_media(ua, &media, pt);
    ua->info.auto_answer = wants_auto_answer(m);
    ua->ring_deadline_ms = ua->now_ms + RING_TIMEOUT_MS;

    char out[SIP_BUF_SIZE];
    send_to(ua, out, build_response(ua, out, sizeof(out), &ids, 100, NULL, NULL, NULL), ip, port);
    char contact[160];
    contact_hdr(ua, contact, sizeof(contact));
    ua->prov_len = build_response(ua, ua->prov, sizeof(ua->prov), &ids, 180, ua->local_tag, contact, NULL);
    send_to(ua, ua->prov, ua->prov_len, ip, port);

    ua->info.state = SIP_CALL_INCOMING;
    notify_call(ua);
}

bool sip_ua_answer(sip_ua_t *ua, uint32_t now_ms)
{
    ua->now_ms = now_ms;
    if (ua->info.state != SIP_CALL_INCOMING) {
        return false;
    }
    char sdp[600], contact[160];
    our_sdp(ua, sdp, sizeof(sdp));
    contact_hdr(ua, contact, sizeof(contact));
    uas_final(ua, 200, contact, sdp);
    ua->info.state = SIP_CALL_ACTIVE;
    notify_call(ua);
    return true;
}

/* re-INVITE or UPDATE inside an established dialog: accept, keep our codec. */
static void on_session_refresh(sip_ua_t *ua, const sip_msg_t *m, bool is_invite, uint32_t ip, uint16_t port)
{
    sdp_media_t media;
    bool has_sdp = sdp_parse(m->body.s, m->body.len, &media);
    if (has_sdp) {
        int pt = ua->pt;
        bool ours_offered = false;
        for (int i = 0; i < media.npts; i++) {
            ours_offered |= media.pts[i] == pt;
        }
        if (!ours_offered) {
            pt = sdp_choose_pt(&media);
        }
        if (pt < 0) {
            respond(ua, m, 488, NULL, NULL, ip, port);
            return;
        }
        bool changed = media.ip != ua->info.media_ip || media.port != ua->info.media_port || pt != ua->pt;
        set_media(ua, &media, pt);
        if (changed) {
            notify_call(ua);
        }
    }
    sip_req_ids_t ids;
    char out[SIP_BUF_SIZE], sdp[600], contact[160];
    if (!ids_from_msg(m, &ids)) {
        return;
    }
    our_sdp(ua, sdp, sizeof(sdp));
    contact_hdr(ua, contact, sizeof(contact));
    size_t n = build_response(ua, out, sizeof(out), &ids, 200, NULL, contact, (has_sdp || is_invite) ? sdp : NULL);
    if (is_invite) {
        /* Retransmit until the ACK, like the first answer. */
        ua->invite_ids = ids;
        ua->invite_src_ip = ip;
        ua->invite_src_port = port;
        tx_start(ua, &ua->call_tx, out, n, ip, port, false, true);
        ua->wait_ack = true;
    } else {
        send_to(ua, out, n, ip, port);
    }
}

/* ------------------------------------------------------------------ hangup */

void sip_ua_hangup(sip_ua_t *ua, uint32_t now_ms)
{
    ua->now_ms = now_ms;
    switch (ua->info.state) {
    case SIP_CALL_INCOMING:
        uas_final(ua, 486, NULL, NULL);
        call_ended(ua, SIP_END_LOCAL, 0);
        break;
    case SIP_CALL_OUTGOING:
    case SIP_CALL_RINGBACK:
        if (ua->got_provisional) {
            send_cancel(ua);
        } else {
            ua->cancel_pending = true; /* CANCEL once a provisional response arrives */
        }
        call_ended(ua, SIP_END_LOCAL, 0);
        break;
    case SIP_CALL_ACTIVE:
        if (ua->uas) {
            tx_stop(&ua->call_tx); /* no longer waiting for an ACK */
            ua->wait_ack = false;
        }
        send_bye(ua);
        call_ended(ua, SIP_END_LOCAL, 0);
        break;
    case SIP_CALL_IDLE:
        break;
    }
}

/* ------------------------------------------------------------------ dispatch */

static bool same_callid(const sip_msg_t *m, const char *callid)
{
    sip_str_t v;
    return callid[0] && sip_get(m, "Call-ID", 0, &v) && sip_str_eq(v, callid);
}

static void on_request(sip_ua_t *ua, const sip_msg_t *m, uint32_t ip, uint16_t port)
{
    unsigned cseq;
    sip_str_t cseq_method;
    if (!sip_cseq(m, &cseq, &cseq_method)) {
        return;
    }
    bool ours = same_callid(m, ua->callid);
    bool in_call = ours && ua->info.state != SIP_CALL_IDLE;

    if (sip_str_eq(m->method, "ACK")) {
        if (ours && ua->wait_ack) {
            tx_stop(&ua->call_tx);
            ua->wait_ack = false;
        }
        return;
    }
    if (sip_str_eq(m->method, "INVITE")) {
        if (ours && ua->uas && cseq == ua->remote_invite_cseq) {
            /* Retransmitted INVITE: repeat our last response. */
            if (ua->wait_ack) {
                send_to(ua, ua->call_tx.buf, ua->call_tx.len, ip, port);
            } else if (ua->info.state == SIP_CALL_INCOMING) {
                send_to(ua, ua->prov, ua->prov_len, ip, port);
            }
            return;
        }
        if (in_call && ua->info.state == SIP_CALL_ACTIVE && cseq > ua->remote_cseq) {
            ua->remote_cseq = cseq;
            on_session_refresh(ua, m, true, ip, port);
            return;
        }
        if (same_callid(m, ua->ended_callid)) {
            return; /* stray retransmission of a finished call */
        }
        on_new_invite(ua, m, ip, port);
        return;
    }
    if (sip_str_eq(m->method, "CANCEL")) {
        if (in_call && ua->uas && ua->info.state == SIP_CALL_INCOMING && cseq == ua->remote_invite_cseq) {
            respond(ua, m, 200, ua->local_tag, NULL, ip, port);
            uas_final(ua, 487, NULL, NULL);
            call_ended(ua, SIP_END_MISSED, 0);
        } else {
            respond(ua, m, 481, NULL, NULL, ip, port);
        }
        return;
    }
    if (sip_str_eq(m->method, "BYE")) {
        if (ours && (in_call || ua->aux_tx.active)) {
            respond(ua, m, 200, NULL, NULL, ip, port);
            tx_stop(&ua->call_tx);
            ua->wait_ack = false;
            call_ended(ua, SIP_END_REMOTE, 0);
        } else {
            respond(ua, m, 481, NULL, NULL, ip, port);
        }
        return;
    }
    if (sip_str_eq(m->method, "UPDATE")) {
        if (in_call) {
            on_session_refresh(ua, m, false, ip, port);
        } else {
            respond(ua, m, 481, NULL, NULL, ip, port);
        }
        return;
    }
    if (sip_str_eq(m->method, "OPTIONS")) {
        char extra[128];
        snprintf(extra, sizeof(extra), "Allow: " ALLOW "\r\nAccept: application/sdp\r\n");
        respond(ua, m, 200, NULL, extra, ip, port);
        return;
    }
    if (sip_str_eq(m->method, "INFO") || sip_str_eq(m->method, "NOTIFY") || sip_str_eq(m->method, "MESSAGE")) {
        respond(ua, m, 200, NULL, NULL, ip, port);
        return;
    }
    respond(ua, m, 501, NULL, NULL, ip, port);
}

static void on_response(sip_ua_t *ua, const sip_msg_t *m)
{
    unsigned cseq;
    sip_str_t method;
    if (!sip_cseq(m, &cseq, &method)) {
        return;
    }
    if (sip_str_eq(method, "REGISTER")) {
        if (same_callid(m, ua->reg_callid) && cseq == ua->reg_cseq && ua->reg_tx.active) {
            on_register_response(ua, m);
        }
        return;
    }
    if (!same_callid(m, ua->callid)) {
        return;
    }
    if ((sip_str_eq(method, "BYE") || sip_str_eq(method, "CANCEL")) && m->status >= 200) {
        tx_stop(&ua->aux_tx);
        return;
    }
    if (!ua->uas && sip_str_eq(method, "INVITE") && cseq == ua->invite_cseq) {
        if (ua->call_tx.active || m->status < 300 || ua->info.state != SIP_CALL_IDLE) {
            on_invite_response(ua, m);
        } else {
            ack_error(ua, m); /* retransmitted final response after we finished */
        }
        return;
    }
}

void sip_ua_on_datagram(sip_ua_t *ua, const char *buf, size_t len, uint32_t src_ip, uint16_t src_port,
                        uint32_t now_ms)
{
    ua->now_ms = now_ms;
    if (src_ip != ua->cfg.server_ip) {
        return; /* only the PBX may talk to us */
    }
    sip_msg_t m;
    if (len < 12 || !sip_parse(buf, len, &m)) {
        return;
    }
    if (m.is_request) {
        on_request(ua, &m, src_ip, src_port);
    } else {
        on_response(ua, &m);
    }
}

void sip_ua_tick(sip_ua_t *ua, uint32_t now_ms)
{
    ua->now_ms = now_ms;
    if (tx_tick(ua, &ua->reg_tx)) {
        reg_retry_later(ua, 408);
    }
    if (ua->reg_due_valid && (int32_t)(now_ms - ua->reg_due_ms) >= 0) {
        ua->reg_auth_tries = 0;
        send_register(ua, NULL);
    }

    bool was_invite = ua->call_tx.active && ua->call_tx.invite;
    if (tx_tick(ua, &ua->call_tx)) {
        if (was_invite) {
            call_ended(ua, SIP_END_UNREACHABLE, 408);
        } else if (ua->wait_ack) {
            ua->wait_ack = false;
            if (ua->info.state == SIP_CALL_ACTIVE) {
                send_bye(ua); /* answered but the ACK never came */
                call_ended(ua, SIP_END_FAILED, 408);
            }
        }
    }
    tx_tick(ua, &ua->aux_tx);

    if (ua->info.state == SIP_CALL_INCOMING && (int32_t)(now_ms - ua->ring_deadline_ms) >= 0) {
        uas_final(ua, 480, NULL, NULL);
        call_ended(ua, SIP_END_MISSED, 0);
    }
}

const sip_call_info_t *sip_ua_call_info(const sip_ua_t *ua)
{
    return &ua->info;
}

sip_reg_state_t sip_ua_reg_state(const sip_ua_t *ua)
{
    return ua->reg_state;
}

void sip_ua_init(sip_ua_t *ua, const sip_ua_cfg_t *cfg, const sip_ua_ops_t *ops, uint32_t now_ms)
{
    memset(ua, 0, sizeof(*ua));
    ua->cfg = *cfg;
    ua->ops = *ops;
    ua->now_ms = now_ms;
    if (ua->cfg.expires <= 0) {
        ua->cfg.expires = 300;
    }
    if (!ua->cfg.server_port) {
        ua->cfg.server_port = 5060;
    }
    char ip[16], id[17];
    local_ip_str(ua, ip);
    hex_id(ua, id, sizeof(id), 2);
    snprintf(ua->reg_callid, sizeof(ua->reg_callid), "reg-%s@%s", id, ip);
    hex_id(ua, ua->reg_tag, sizeof(ua->reg_tag), 1);
}

const char *sip_call_state_name(sip_call_state_t s)
{
    switch (s) {
    case SIP_CALL_IDLE: return "idle";
    case SIP_CALL_INCOMING: return "incoming";
    case SIP_CALL_OUTGOING: return "outgoing";
    case SIP_CALL_RINGBACK: return "ringback";
    case SIP_CALL_ACTIVE: return "active";
    }
    return "?";
}

const char *sip_end_reason_name(sip_end_reason_t r)
{
    switch (r) {
    case SIP_END_NONE: return "none";
    case SIP_END_LOCAL: return "local";
    case SIP_END_REMOTE: return "remote";
    case SIP_END_MISSED: return "missed";
    case SIP_END_BUSY: return "busy";
    case SIP_END_REJECTED: return "rejected";
    case SIP_END_UNREACHABLE: return "unreachable";
    case SIP_END_FAILED: return "failed";
    }
    return "?";
}
