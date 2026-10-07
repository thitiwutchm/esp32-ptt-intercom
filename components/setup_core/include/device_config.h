/*
 * Everything a user can set from the phone setup page, kept in NVS.
 * Plain data; defaults come from menuconfig (main/settings.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_NAME_LEN 16

typedef struct {
    char name[CFG_NAME_LEN + 1]; /* shown to other devices and as SIP caller name fallback */
    uint8_t channel;             /* walkie-talkie channel 1..16 */
    uint8_t volume;              /* 0..100 */

    char wifi_ssid[33];
    char wifi_pass[65];

    bool sip_enabled;
    char sip_server[64];
    uint16_t sip_port;
    char sip_user[32];
    char sip_pass[64];
    char sip_display[32];
    char sip_target[32];
    bool sip_auto_answer;
} device_config_t;

/*
 * Apply an application/x-www-form-urlencoded body from the setup page.
 * Empty password fields keep the stored password (the page never receives
 * passwords), except that a new Wi-Fi network with an empty password means
 * an open network. On error nothing is changed and `field` names the
 * offending input (for the page to highlight) and `err` says why, in Thai
 * (the page is Thai; keep err_cap >= 160 for UTF-8).
 */
bool config_apply_form(device_config_t *cfg, const char *body, char *field, size_t field_cap, char *err,
                       size_t err_cap);

/* Current settings as JSON for the page; passwords only as "*_set": true/false. */
size_t config_to_json(const device_config_t *cfg, char *out, size_t cap);

/* URL-decoded value of `key` in a form body. False if absent. */
bool form_get(const char *body, const char *key, char *out, size_t cap);

/* JSON string escaping (adds no quotes). Returns length written, 0 if it does not fit. */
size_t json_escape(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
