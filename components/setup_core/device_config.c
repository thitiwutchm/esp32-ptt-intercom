#include "device_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef enum { FIELD_ABSENT, FIELD_TOO_LONG, FIELD_OK } field_t;

static field_t field_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        size_t nlen = eq ? (size_t)(eq - p) : (size_t)(end - p);
        if (nlen == klen && memcmp(p, key, klen) == 0) {
            size_t n = 0;
            for (const char *v = eq ? eq + 1 : end; v < end; v++) {
                char c = *v;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && v + 2 < end && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    c = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 2;
                }
                if (n + 1 >= cap) {
                    return FIELD_TOO_LONG;
                }
                out[n++] = c;
            }
            out[n] = '\0';
            return FIELD_OK;
        }
        p = amp ? amp + 1 : NULL;
    }
    return FIELD_ABSENT;
}

bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    return field_get(body, key, out, cap) == FIELD_OK;
}

static bool printable_ascii(const char *s, const char *forbid)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E || (forbid && strchr(forbid, *s))) {
            return false;
        }
    }
    return true;
}

static bool dial_chars(const char *s)
{
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr("._-+*#", *s)) {
            return false;
        }
    }
    return true;
}

static bool host_chars(const char *s)
{
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr(".-_", *s)) {
            return false;
        }
    }
    return true;
}

#define FAIL(f, msg)                                \
    do {                                            \
        snprintf(field, field_cap, "%s", f);       \
        snprintf(err, err_cap, "%s", msg);         \
        return false;                               \
    } while (0)

/* Read a field into its buffer; a missing field keeps `def`, an oversized one is an error. */
#define GET(key, buf, def)                                    \
    do {                                                      \
        field_t r_ = field_get(body, key, buf, sizeof(buf)); \
        if (r_ == FIELD_TOO_LONG) {                           \
            FAIL(key, "ยาวเกินไป");                            \
        }                                                     \
        if (r_ == FIELD_ABSENT) {                             \
            snprintf(buf, sizeof(buf), "%s", def);            \
        }                                                     \
    } while (0)

bool config_apply_form(device_config_t *cfg, const char *body, char *field, size_t field_cap, char *err,
                       size_t err_cap)
{
    device_config_t n = *cfg;
    char tmp[16];

    GET("name", n.name, cfg->name);
    if (!n.name[0] || !printable_ascii(n.name, NULL)) {
        FAIL("name", "ชื่อ 1-16 ตัว ใช้อักษรอังกฤษ ตัวเลข หรือสัญลักษณ์");
    }

    GET("wifi_ssid", n.wifi_ssid, cfg->wifi_ssid);
    if (!n.wifi_ssid[0]) {
        FAIL("wifi_ssid", "เลือกหรือพิมพ์ชื่อ Wi-Fi");
    }
    char pass[sizeof(n.wifi_pass)];
    GET("wifi_pass", pass, "");
    if (pass[0]) {
        snprintf(n.wifi_pass, sizeof(n.wifi_pass), "%s", pass);
    } else if (strcmp(n.wifi_ssid, cfg->wifi_ssid) != 0) {
        n.wifi_pass[0] = '\0'; /* another network, no password: open network */
    }
    size_t pl = strlen(n.wifi_pass);
    if (pl != 0 && (pl < 8 || pl > 64)) {
        FAIL("wifi_pass", "รหัส Wi-Fi ต้องยาว 8-63 ตัว (หรือเว้นว่างถ้าเป็นเครือข่ายเปิด)");
    }

    GET("sip_enabled", tmp, n.sip_enabled ? "1" : "0");
    n.sip_enabled = strcmp(tmp, "1") == 0 || strcmp(tmp, "on") == 0;
    GET("sip_auto_answer", tmp, n.sip_auto_answer ? "1" : "0");
    n.sip_auto_answer = strcmp(tmp, "1") == 0 || strcmp(tmp, "on") == 0;
    GET("sip_server", n.sip_server, cfg->sip_server);
    GET("sip_user", n.sip_user, cfg->sip_user);
    GET("sip_display", n.sip_display, cfg->sip_display);
    GET("sip_target", n.sip_target, cfg->sip_target);
    char port[16];
    snprintf(tmp, sizeof(tmp), "%u", cfg->sip_port);
    GET("sip_port", port, tmp);
    char sp[sizeof(n.sip_pass)];
    GET("sip_pass", sp, "");
    if (sp[0]) {
        snprintf(n.sip_pass, sizeof(n.sip_pass), "%s", sp);
    }

    if (n.sip_enabled) {
        if (!host_chars(n.sip_server)) {
            FAIL("sip_server", "ใส่ IP เช่น 192.168.1.10 หรือชื่อเครื่อง");
        }
        char *e;
        long p = strtol(port, &e, 10);
        if (*e || p < 1 || p > 65535) {
            FAIL("sip_port", "พอร์ตต้องอยู่ระหว่าง 1-65535");
        }
        n.sip_port = (uint16_t)p;
        if (!dial_chars(n.sip_user)) {
            FAIL("sip_user", "เบอร์ภายในใช้ตัวเลขหรืออักษรอังกฤษ");
        }
        if (!dial_chars(n.sip_target)) {
            FAIL("sip_target", "เบอร์ที่จะโทรใช้ตัวเลขหรืออักษรอังกฤษ");
        }
        if (!printable_ascii(n.sip_display, "\"\\")) {
            FAIL("sip_display", "ชื่อใช้อักษรอังกฤษ ห้ามมีเครื่องหมายคำพูด");
        }
        if (!printable_ascii(n.sip_pass, NULL)) {
            FAIL("sip_pass", "รหัสผ่านมีอักขระที่ใช้ไม่ได้");
        }
    }
    *cfg = n;
    return true;
}

size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in; in++) {
        char esc[8];
        const char *piece = esc;
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            esc[2] = 0;
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
        } else {
            esc[0] = (char)c;
            esc[1] = 0;
        }
        size_t l = strlen(piece);
        if (n + l + 1 > cap) {
            return 0;
        }
        memcpy(out + n, piece, l);
        n += l;
    }
    if (cap) {
        out[n] = '\0';
    }
    return n;
}

size_t config_to_json(const device_config_t *c, char *out, size_t cap)
{
    char name[3 * sizeof(c->name)], ssid[3 * sizeof(c->wifi_ssid) * 2], server[3 * sizeof(c->sip_server)],
        user[3 * sizeof(c->sip_user)], display[3 * sizeof(c->sip_display)], target[3 * sizeof(c->sip_target)];
    json_escape(c->name, name, sizeof(name));
    json_escape(c->wifi_ssid, ssid, sizeof(ssid));
    json_escape(c->sip_server, server, sizeof(server));
    json_escape(c->sip_user, user, sizeof(user));
    json_escape(c->sip_display, display, sizeof(display));
    json_escape(c->sip_target, target, sizeof(target));
    int n = snprintf(out, cap,
                     "{\"name\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_pass_set\":%s,"
                     "\"sip_enabled\":%s,\"sip_server\":\"%s\",\"sip_port\":%u,\"sip_user\":\"%s\","
                     "\"sip_pass_set\":%s,\"sip_display\":\"%s\",\"sip_target\":\"%s\",\"sip_auto_answer\":%s}",
                     name, ssid, c->wifi_pass[0] ? "true" : "false", c->sip_enabled ? "true" : "false", server,
                     c->sip_port, user, c->sip_pass[0] ? "true" : "false", display, target,
                     c->sip_auto_answer ? "true" : "false");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
