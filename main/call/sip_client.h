/*
 * The device as a SIP extension: owns the SIP and RTP sockets and a task
 * that drives voip_core's sip_ua. All functions are safe from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_config.h"
#include "esp_err.h"
#include "sip_ua.h"

typedef struct {
    void (*call_changed)(const sip_call_info_t *info); /* from the SIP task; must not block */
    void (*reg_changed)(sip_reg_state_t state);
    bool (*is_busy)(void); /* e.g. walkie-talkie in use: incoming calls get 486 */
    void (*rtp_frame)(uint16_t seq, int pt, const uint8_t *payload, size_t len); /* received audio */
} sip_client_cb_t;

/* Uses the SIP fields of cfg; does nothing when cfg->sip_enabled is false. */
esp_err_t sip_client_start(const sip_client_cb_t *cb, const device_config_t *cfg);

/* Wi-Fi got an address: (re)register from it. */
void sip_client_network_up(uint32_t local_ip);
void sip_client_network_down(void);

bool sip_client_call(const char *target);
bool sip_client_answer(void);
void sip_client_hangup(void);
sip_reg_state_t sip_client_reg_state(void);

/* Send one 20 ms frame of 8 kHz audio to the far end (G.711 with the negotiated payload type). */
void sip_client_send_audio(const int16_t *pcm8k, int samples);
