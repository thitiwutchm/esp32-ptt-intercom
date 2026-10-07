/*
 * A small SIP user agent over UDP for a desk intercom registered to one PBX
 * (Asterisk): REGISTER with digest auth, one call at a time, incoming and
 * outgoing, CANCEL, BYE, re-INVITE/UPDATE, OPTIONS keepalive.
 *
 * Transport and time are supplied by the caller, so the same code runs on
 * the ESP32 and in host tests against a real Asterisk. Every request goes to
 * the PBX (it acts as outbound proxy); requests from any other address are
 * ignored. Not thread safe: serialise calls into one sip_ua_t.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIP_BUF_SIZE 2048

typedef enum {
    SIP_REG_IDLE = 0,
    SIP_REG_TRYING,
    SIP_REG_OK,
    SIP_REG_FAILED,
} sip_reg_state_t;

typedef enum {
    SIP_CALL_IDLE = 0,
    SIP_CALL_INCOMING, /* ringing here: answer or reject */
    SIP_CALL_OUTGOING, /* INVITE sent, no ringing reported yet */
    SIP_CALL_RINGBACK, /* the other side is ringing */
    SIP_CALL_ACTIVE,   /* media flowing */
} sip_call_state_t;

typedef enum {
    SIP_END_NONE = 0,
    SIP_END_LOCAL,       /* we hung up, rejected or cancelled */
    SIP_END_REMOTE,      /* they hung up */
    SIP_END_MISSED,      /* incoming call cancelled before we answered */
    SIP_END_BUSY,        /* 486 / 600 */
    SIP_END_REJECTED,    /* 603 */
    SIP_END_UNREACHABLE, /* 404 / 408 / 480 / no answer from the PBX */
    SIP_END_FAILED,      /* anything else */
} sip_end_reason_t;

typedef struct {
    sip_call_state_t state;
    char peer[32];       /* caller/callee display name or number */
    bool auto_answer;    /* INCOMING: the PBX asked for auto answer (Call-Info answer-after / Alert-Info) */
    sip_end_reason_t reason; /* IDLE: why the call ended */
    int status;          /* IDLE: final SIP status of a failed call, 0 otherwise */
    uint32_t media_ip;   /* ACTIVE: where to send RTP (network byte order) */
    uint16_t media_port;
    int pt;              /* ACTIVE: 0 = PCMU, 8 = PCMA */
} sip_call_info_t;

typedef struct {
    void (*send)(void *ctx, const char *buf, size_t len, uint32_t ip, uint16_t port);
    void (*reg_changed)(void *ctx, sip_reg_state_t state, int status);
    void (*call_changed)(void *ctx, const sip_call_info_t *info);
    bool (*is_busy)(void *ctx); /* optional: answer new calls with 486 while true */
    uint32_t (*random)(void *ctx);
    void *ctx;
} sip_ua_ops_t;

typedef struct {
    char domain[64];      /* SIP domain for URIs, usually the PBX address */
    uint32_t server_ip;   /* PBX, network byte order */
    uint16_t server_port; /* 5060 */
    char user[32];        /* extension, also the auth user name */
    char password[64];
    char display[32];
    uint32_t local_ip;
    uint16_t local_port;
    uint16_t rtp_port;
    int expires; /* registration lifetime asked for, seconds */
} sip_ua_cfg_t;

typedef struct {
    bool active;
    bool invite; /* client INVITE: stop retransmitting after a provisional response */
    char buf[SIP_BUF_SIZE];
    size_t len;
    uint32_t ip;
    uint16_t port;
    uint32_t next_ms;
    uint32_t interval_ms; /* 0 = no more retransmissions, wait for deadline */
    uint32_t deadline_ms;
} sip_tx_t;

typedef struct {
    char vias[640];
    char from[200];
    char to[200];
    char callid[100];
    char cseq[48];
} sip_req_ids_t;

typedef struct {
    sip_ua_cfg_t cfg;
    sip_ua_ops_t ops;
    uint32_t now_ms;

    /* registration */
    sip_reg_state_t reg_state;
    char reg_callid[64];
    char reg_tag[20];
    unsigned reg_cseq;
    int reg_auth_tries;
    bool reg_due_valid;
    uint32_t reg_due_ms;
    sip_tx_t reg_tx;

    /* the call */
    sip_call_info_t info;
    bool uas;
    char callid[100];
    char local_tag[20];
    char remote_tag[64];
    char local_uri[192];  /* name-addr without tag */
    char remote_uri[192]; /* name-addr without tag */
    char remote_target[192];
    char request_uri[192]; /* UAC: Request-URI of the INVITE */
    char invite_branch[40];
    unsigned local_cseq;
    unsigned invite_cseq;        /* UAC */
    unsigned remote_invite_cseq; /* UAS */
    unsigned remote_cseq;        /* highest in-dialog CSeq seen from them */
    int pt;
    uint32_t session_id;
    uint32_t session_ver;
    sip_req_ids_t invite_ids; /* UAS: the INVITE we answer */
    uint32_t invite_src_ip;
    uint16_t invite_src_port;
    char prov[1024]; /* UAS: last provisional response, resent on INVITE retransmissions */
    size_t prov_len;
    char ack[1024]; /* UAC: ACK for the 2xx, resent if the 2xx is retransmitted */
    size_t ack_len;
    bool got_provisional;
    bool cancel_pending;
    int invite_auth_tries;
    bool wait_ack; /* UAS: final response in call_tx until ACK */
    uint32_t ring_deadline_ms;
    char ended_callid[100]; /* ignore stray retransmissions of a finished call */

    sip_tx_t call_tx; /* INVITE, or our final response to an INVITE */
    sip_tx_t aux_tx;  /* CANCEL / BYE */
} sip_ua_t;

void sip_ua_init(sip_ua_t *ua, const sip_ua_cfg_t *cfg, const sip_ua_ops_t *ops, uint32_t now_ms);

/* Start (or restart) registration. */
void sip_ua_register(sip_ua_t *ua, uint32_t now_ms);

/* Feed every datagram received on the SIP port. */
void sip_ua_on_datagram(sip_ua_t *ua, const char *buf, size_t len, uint32_t src_ip, uint16_t src_port,
                        uint32_t now_ms);

/* Call at least every 100 ms: retransmissions, timeouts, registration refresh. */
void sip_ua_tick(sip_ua_t *ua, uint32_t now_ms);

/* Dial an extension or number. False if a call is in progress. */
bool sip_ua_call(sip_ua_t *ua, const char *target, uint32_t now_ms);

/* Answer the ringing incoming call. */
bool sip_ua_answer(sip_ua_t *ua, uint32_t now_ms);

/* Reject, cancel or hang up, whatever fits the current state. */
void sip_ua_hangup(sip_ua_t *ua, uint32_t now_ms);

const sip_call_info_t *sip_ua_call_info(const sip_ua_t *ua);
sip_reg_state_t sip_ua_reg_state(const sip_ua_t *ua);

const char *sip_call_state_name(sip_call_state_t s);
const char *sip_end_reason_name(sip_end_reason_t r);

#ifdef __cplusplus
}
#endif
