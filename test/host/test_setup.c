/* Host unit tests for components/setup_core. Run: make -C test/host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "captive_dns.h"
#include "device_config.h"

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            failures++;                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

static device_config_t base(void)
{
    device_config_t c = {0};
    strcpy(c.name, "PTT-1A2B");
    c.channel = 3;
    c.volume = 70;
    strcpy(c.wifi_ssid, "Home");
    strcpy(c.wifi_pass, "oldpassword");
    c.sip_enabled = true;
    strcpy(c.sip_server, "192.168.1.10");
    c.sip_port = 5060;
    strcpy(c.sip_user, "200");
    strcpy(c.sip_pass, "oldsip");
    strcpy(c.sip_display, "Desk");
    strcpy(c.sip_target, "100");
    return c;
}

static void test_form_get(void)
{
    char v[32];
    const char *body = "a=1&name=Desk+Phone&enc=%E0%B8%81%41&empty=&flag&bad=%zz%4";
    CHECK(form_get(body, "a", v, sizeof(v)) && strcmp(v, "1") == 0);
    CHECK(form_get(body, "name", v, sizeof(v)) && strcmp(v, "Desk Phone") == 0);
    CHECK(form_get(body, "enc", v, sizeof(v)) && strcmp(v, "\xE0\xB8\x81" "A") == 0); /* Thai "ko kai" + A */
    CHECK(form_get(body, "empty", v, sizeof(v)) && v[0] == '\0');
    CHECK(form_get(body, "flag", v, sizeof(v)) && v[0] == '\0');
    CHECK(form_get(body, "bad", v, sizeof(v)) && strcmp(v, "%zz%4") == 0); /* invalid escapes kept literally */
    CHECK(!form_get(body, "nam", v, sizeof(v)));                           /* no prefix matches */
    CHECK(!form_get(body, "missing", v, sizeof(v)));
    CHECK(!form_get("name=0123456789", "name", v, 5)); /* too long */
}

static void test_apply(void)
{
    char field[24], err[192];

    /* Full form: everything changes. */
    device_config_t c = base();
    CHECK(config_apply_form(&c,
                            "name=Desk&wifi_ssid=Office+5&wifi_pass=newpass123&sip_enabled=1&sip_server=pbx.lan"
                            "&sip_port=5070&sip_user=201&sip_pass=s3cret&sip_display=Front+Desk&sip_target=600"
                            "&sip_auto_answer=1",
                            field, sizeof(field), err, sizeof(err)));
    CHECK(strcmp(c.name, "Desk") == 0 && strcmp(c.wifi_ssid, "Office 5") == 0 && strcmp(c.wifi_pass, "newpass123") == 0);
    CHECK(strcmp(c.sip_server, "pbx.lan") == 0 && c.sip_port == 5070 && strcmp(c.sip_user, "201") == 0);
    CHECK(strcmp(c.sip_pass, "s3cret") == 0 && strcmp(c.sip_display, "Front Desk") == 0 &&
          strcmp(c.sip_target, "600") == 0 && c.sip_auto_answer);
    CHECK(c.channel == 3 && c.volume == 70); /* not on the form: untouched */

    /* Empty passwords keep the stored ones on the same network. */
    c = base();
    CHECK(config_apply_form(&c, "name=X&wifi_ssid=Home&wifi_pass=&sip_pass=&sip_enabled=1", field, sizeof(field),
                            err, sizeof(err)));
    CHECK(strcmp(c.wifi_pass, "oldpassword") == 0 && strcmp(c.sip_pass, "oldsip") == 0);

    /* Another network with no password: open network. */
    c = base();
    CHECK(config_apply_form(&c, "wifi_ssid=Cafe&wifi_pass=", field, sizeof(field), err, sizeof(err)));
    CHECK(strcmp(c.wifi_ssid, "Cafe") == 0 && c.wifi_pass[0] == '\0');

    /* Thai SSID is fine; Thai device name is not (the screen font has no Thai). */
    c = base();
    CHECK(config_apply_form(&c, "wifi_ssid=%E0%B8%9A%E0%B9%89%E0%B8%B2%E0%B8%99&wifi_pass=12345678", field,
                            sizeof(field), err, sizeof(err)));
    CHECK(strcmp(c.wifi_ssid, "\xE0\xB8\x9A\xE0\xB9\x89\xE0\xB8\xB2\xE0\xB8\x99") == 0);

    struct {
        const char *body;
        const char *field;
    } bad[] = {
        {"name=%E0%B8%81", "name"},
        {"name=", "name"},
        {"name=12345678901234567", "name"}, /* 17 chars */
        {"wifi_ssid=", "wifi_ssid"},
        {"wifi_pass=short", "wifi_pass"},
        {"sip_enabled=1&sip_server=", "sip_server"},
        {"sip_enabled=1&sip_server=a+b", "sip_server"},
        {"sip_enabled=1&sip_port=0", "sip_port"},
        {"sip_enabled=1&sip_port=70000", "sip_port"},
        {"sip_enabled=1&sip_port=50x", "sip_port"},
        {"sip_enabled=1&sip_user=", "sip_user"},
        {"sip_enabled=1&sip_user=2%2000", "sip_user"},
        {"sip_enabled=1&sip_target=", "sip_target"},
        {"sip_enabled=1&sip_display=Say+%22hi%22", "sip_display"},
        {"sip_enabled=1&sip_pass=a%0Ab", "sip_pass"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        c = base();
        device_config_t before = c;
        bool ok = config_apply_form(&c, bad[i].body, field, sizeof(field), err, sizeof(err));
        CHECK(!ok && strcmp(field, bad[i].field) == 0 && err[0]);
        CHECK(strlen(err) < sizeof(err) - 1); /* never cut inside a Thai (UTF-8) character */
        CHECK(memcmp(&c, &before, sizeof(c)) == 0); /* nothing changed on error */
        if (ok || strcmp(field, bad[i].field) != 0) {
            fprintf(stderr, "  case %zu: %s -> %s (%s)\n", i, bad[i].body, field, err);
        }
    }

    /* SIP off: SIP fields are not validated, so an unused bad value cannot block saving Wi-Fi. */
    c = base();
    CHECK(config_apply_form(&c, "sip_enabled=0&sip_server=&sip_user=", field, sizeof(field), err, sizeof(err)));
    CHECK(!c.sip_enabled);
}

static void test_json(void)
{
    char out[64];
    CHECK(json_escape("a\"b\\c\n", out, sizeof(out)) && strcmp(out, "a\\\"b\\\\c\\u000a") == 0);
    CHECK(json_escape("toolong", out, 4) == 0);

    device_config_t c = base();
    strcpy(c.name, "Q\"uote");
    char js[1024];
    CHECK(config_to_json(&c, js, sizeof(js)) > 0);
    CHECK(strstr(js, "\"name\":\"Q\\\"uote\""));
    CHECK(strstr(js, "\"wifi_pass_set\":true") && strstr(js, "\"sip_pass_set\":true"));
    CHECK(!strstr(js, "oldpassword") && !strstr(js, "oldsip")); /* never sent back */
    CHECK(strstr(js, "\"sip_port\":5060") && strstr(js, "\"sip_enabled\":true"));
    CHECK(config_to_json(&c, js, 50) == 0);
}

static void test_dns(void)
{
    /* Query for connectivitycheck.gstatic.com A, id 0xBEEF, RD set. */
    uint8_t q[64] = {0xBE, 0xEF, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    size_t n = 12;
    const char *labels[] = {"connectivitycheck", "gstatic", "com"};
    for (int i = 0; i < 3; i++) {
        q[n++] = (uint8_t)strlen(labels[i]);
        memcpy(q + n, labels[i], strlen(labels[i]));
        n += strlen(labels[i]);
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1; /* A */
    q[n++] = 0;
    q[n++] = 1; /* IN */
    uint8_t ip[4] = {192, 168, 4, 1};
    uint32_t ip32;
    memcpy(&ip32, ip, 4);
    uint8_t r[128];
    size_t rn = captive_dns_reply(q, n, ip32, r, sizeof(r));
    CHECK(rn == n + 16);
    CHECK(r[0] == 0xBE && r[1] == 0xEF && (r[2] & 0x80) && (r[2] & 0x01) && r[3] == 0);
    CHECK(r[7] == 1 && memcmp(r + 12, q + 12, n - 12) == 0);
    CHECK(r[n] == 0xC0 && r[n + 1] == 12 && r[n + 3] == 1 && r[n + 11] == 4 && memcmp(r + n + 12, ip, 4) == 0);

    /* AAAA: no answer, still a valid reply. */
    q[n - 3] = 28;
    rn = captive_dns_reply(q, n, ip32, r, sizeof(r));
    CHECK(rn == n && r[7] == 0);

    /* Ignored: responses, truncated packets, compressed question names, tiny buffers. */
    q[n - 3] = 1;
    q[2] |= 0x80;
    CHECK(captive_dns_reply(q, n, ip32, r, sizeof(r)) == 0);
    q[2] &= 0x7F;
    CHECK(captive_dns_reply(q, n - 3, ip32, r, sizeof(r)) == 0);
    CHECK(captive_dns_reply(q, n, ip32, r, n) == 0);
    uint8_t comp[] = {0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    CHECK(captive_dns_reply(comp, sizeof(comp), ip32, r, sizeof(r)) == 0);
}

int main(void)
{
    test_form_get();
    test_apply();
    test_json();
    test_dns();
    printf("setup: %d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
