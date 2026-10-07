#include "ptt_app.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "buttons.h"
#include "call_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ptt_adpcm.h"
#include "ptt_floor.h"
#include "ptt_jitter.h"
#include "ptt_net.h"
#include "ptt_peers.h"
#include "ptt_proto.h"
#include "sdkconfig.h"
#include "settings.h"
#include "setup_portal.h"
#include "sip_client.h"
#include "tones.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "ptt";

#define HELLO_PERIOD_MS 2000
#define TICK_MS 50
#define CONTROL_REPEATS 3 /* TALK_START / TALK_END copies, one per 20 ms frame */
#define NOTICE_MS 1500
#define DIM_AFTER_MS 30000
#define BACKLIGHT_ON 100
#define BACKLIGHT_DIM 15

/* Events the app task handles besides ui_event_t (same queue). */
#define APP_EV_BOOT_CLICK 100   /* BOOT tapped */
#define APP_EV_BOOT_LONG 101    /* BOOT held */
#define APP_EV_BOOT_RELEASE 102 /* BOOT let go after a hold */
#define APP_EV_AUTO_ANSWER 103  /* the PBX asked us to answer at once */
#define APP_EV_BOOT_VERY_LONG 104 /* BOOT held 8 s: phone setup */
#define APP_EV_SETUP_SAVED 105  /* the setup page saved new settings */
#define APP_EV_SETUP_CANCEL 106 /* the setup page was left without saving */

#define SETUP_AUTO_AFTER_MS 90000      /* never connected this long after boot: open setup */
#define SETUP_TIMEOUT_MS (15 * 60000)  /* close an unused setup network */
#define REBOOT_DELAY_MS 1500

#if CONFIG_PTT_SIP_ENABLE
#define SIP_BUILT 1
#else
#define SIP_BUILT 0
#endif

#if CONFIG_PTT_CODEC_PCM16
#define TX_CODEC PTT_CODEC_PCM16
#else
#define TX_CODEC PTT_CODEC_IMA_ADPCM
#endif

typedef struct {
    SemaphoreHandle_t lock; /* guards everything below except the task handles */
    ptt_floor_t floor;
    ptt_peers_t peers;
    ptt_jitter_t jb;
    settings_t cfg;
    uint32_t my_id;
    bool wifi_up;
    int battery;

    /* transmit */
    uint16_t tx_seq;
    int start_repeats;
    int end_repeats;
    uint32_t end_talk_id;

    /* receive */
    bool rx_roger; /* play the roger beep when this talk drains */

    /* phone calls through the PBX */
    sip_call_info_t call; /* latest state from the SIP task */
    sip_reg_state_t sip_reg;
    uint32_t call_start_ms;
    bool boot_ptt; /* walkie-talkie held with BOOT */

    /* phone setup */
    bool setup_active;
    char setup_ssid[33];
    char setup_pass[17];
    uint32_t setup_until_ms;
    bool setup_auto_done;
    uint32_t setup_retry_ms; /* automatic setup failed: try again at this time */
    uint32_t reboot_at_ms; /* 0 = none */

    /* screen */
    char notice[32];
    bool notice_warn;
    uint32_t notice_until_ms;
    uint32_t last_activity_ms;
    bool dimmed;
} app_t;

static app_t s;
static device_config_t s_saved_cfg; /* from the setup page, applied by the app task */

/* SIP is compiled in and switched on in the settings (fixed until the next reboot). */
static bool sip_on(void)
{
    return SIP_BUILT && s.cfg.sip_enabled;
}
static QueueHandle_t s_events;
static TaskHandle_t s_rx_task;

typedef struct {
    bool wake_player;
    bool ui;
} effects_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ helpers (call with lock held) */

static void set_notice_locked(const char *text, bool warn, uint32_t now)
{
    strlcpy(s.notice, text, sizeof(s.notice));
    s.notice_warn = warn;
    s.notice_until_ms = now + NOTICE_MS;
}

static void handle_floor_event_locked(ptt_floor_event_t ev, uint32_t now, effects_t *fx)
{
    if (ev == PTT_EV_NONE) {
        return;
    }
    ESP_LOGI(TAG, "floor: %s (talker %08lx)", ptt_floor_event_name(ev), (unsigned long)s.floor.talker_id);
    s.last_activity_ms = now;
    fx->ui = true;
    switch (ev) {
    case PTT_EV_TX_START:
        s.tx_seq = 0;
        s.start_repeats = CONTROL_REPEATS;
        s.end_repeats = 0;
        break;
    case PTT_EV_TX_STOP:
    case PTT_EV_TX_TIMEOUT:
        s.start_repeats = 0;
        s.end_repeats = CONTROL_REPEATS;
        s.end_talk_id = s.floor.talk_id;
        if (ev == PTT_EV_TX_TIMEOUT) {
            set_notice_locked("Talk time limit", true, now);
        }
        break;
    case PTT_EV_TX_YIELD:
        /* Lost a simultaneous press: go quiet without TALK_END, the winner keeps the floor. */
        s.start_repeats = 0;
        s.end_repeats = 0;
        /* fall through */
    case PTT_EV_RX_START:
    case PTT_EV_RX_SWITCH:
        ptt_jb_reset(&s.jb);
        s.rx_roger = false;
        fx->wake_player = true;
        break;
    case PTT_EV_RX_END:
    case PTT_EV_RX_TIMEOUT:
        s.rx_roger = true;
        break;
    case PTT_EV_DENIED:
        set_notice_locked("Channel busy", true, now);
        break;
    default:
        break;
    }
}

static bool in_call_locked(void)
{
    return s.call.state != SIP_CALL_IDLE;
}

static void build_view_locked(ui_view_t *v, uint32_t now)
{
    memset(v, 0, sizeof(*v));
    v->sip = sip_on() ? (s.sip_reg == SIP_REG_OK) : -1;
    if (s.setup_active && !in_call_locked()) {
        v->mode = UI_MODE_SETUP;
        strlcpy(v->setup_ssid, s.setup_ssid, sizeof(v->setup_ssid));
        strlcpy(v->setup_pass, s.setup_pass, sizeof(v->setup_pass));
        uint32_t ip, bcast;
        if (wifi_get_addresses(&ip, &bcast)) {
            snprintf(v->setup_lan_ip, sizeof(v->setup_lan_ip), "%u.%u.%u.%u", (unsigned)(ip & 0xFF),
                     (unsigned)((ip >> 8) & 0xFF), (unsigned)((ip >> 16) & 0xFF), (unsigned)(ip >> 24));
        }
    } else if (!s.wifi_up) {
        v->mode = UI_MODE_WIFI;
        v->wifi_unset = s.cfg.wifi_ssid[0] == '\0';
    } else if (in_call_locked()) {
        v->mode = s.call.state == SIP_CALL_INCOMING ? UI_MODE_CALL_IN
                  : s.call.state == SIP_CALL_ACTIVE ? UI_MODE_CALL
                                                    : UI_MODE_CALL_OUT;
        strlcpy(v->call_peer, s.call.peer[0] ? s.call.peer : "Unknown", sizeof(v->call_peer));
        v->call_ringing = s.call.state == SIP_CALL_RINGBACK;
        v->call_secs = s.call.state == SIP_CALL_ACTIVE ? (int)((now - s.call_start_ms) / 1000) : 0;
    } else if (s.floor.state == PTT_FLOOR_TX) {
        v->mode = UI_MODE_TX;
    } else if (s.floor.state == PTT_FLOOR_RX) {
        v->mode = UI_MODE_RX;
        const ptt_peer_t *p = ptt_peers_find(&s.peers, s.floor.talker_id);
        if (p && strcmp(p->name, "?") != 0) {
            strlcpy(v->talker, p->name, sizeof(v->talker));
        } else {
            snprintf(v->talker, sizeof(v->talker), "ID %04lX", (unsigned long)(s.floor.talker_id & 0xFFFF));
        }
    } else {
        v->mode = UI_MODE_IDLE;
    }
    v->channel = s.cfg.channel;
    v->online = ptt_peers_count(&s.peers, s.cfg.channel);
    v->battery = s.battery;
    strlcpy(v->name, s.cfg.name, sizeof(v->name));
    if (s.notice[0] && (int32_t)(s.notice_until_ms - now) > 0) {
        strlcpy(v->notice, s.notice, sizeof(v->notice));
        v->notice_warn = s.notice_warn;
    } else {
        s.notice[0] = '\0';
    }
}

static void apply_effects(const effects_t *fx)
{
    if (fx->wake_player) {
        xTaskNotifyGive(s_rx_task);
    }
    if (fx->ui) {
        ui_view_t v;
        uint32_t now = now_ms();
        xSemaphoreTake(s.lock, portMAX_DELAY);
        build_view_locked(&v, now);
        xSemaphoreGive(s.lock);
        ui_update(&v);
    }
}

static void send_hello(void)
{
    uint8_t payload[PTT_HELLO_LEN];
    uint8_t pkt[PTT_HDR_LEN + PTT_HELLO_LEN];
    ptt_hello_t hello = {0};

    xSemaphoreTake(s.lock, portMAX_DELAY);
    strlcpy(hello.name, s.cfg.name, sizeof(hello.name));
    hello.battery = s.battery < 0 ? 255 : (uint8_t)s.battery;
    hello.state = s.floor.state == PTT_FLOOR_TX   ? PTT_PEER_TALKING
                  : s.floor.state == PTT_FLOOR_RX ? PTT_PEER_LISTENING
                                                  : PTT_PEER_IDLE;
    ptt_hdr_t h = {.type = PTT_MSG_HELLO, .device_id = s.my_id, .channel = s.cfg.channel};
    xSemaphoreGive(s.lock);

    h.payload_len = (uint16_t)ptt_hello_encode(payload, sizeof(payload), &hello);
    size_t n = ptt_proto_build(pkt, sizeof(pkt), &h, payload);
    ptt_net_broadcast(pkt, n);
}

/* ------------------------------------------------------------------ network receive (net_rx task) */

static void on_packet(const ptt_hdr_t *hdr, const uint8_t *payload, uint32_t src_ip)
{
    if (hdr->device_id == s.my_id) {
        return; /* our own broadcast */
    }
    uint32_t now = now_ms();
    effects_t fx = {0};

    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (hdr->type == PTT_MSG_HELLO) {
        ptt_hello_t hello;
        if (ptt_hello_decode(payload, hdr->payload_len, &hello)) {
            const ptt_peer_t *before = ptt_peers_find(&s.peers, hdr->device_id);
            bool changed = !before || before->channel != hdr->channel || strcmp(before->name, hello.name) != 0;
            if (!ptt_peers_update(&s.peers, hdr->device_id, src_ip, hdr->channel, &hello, now)) {
                ESP_LOGW(TAG, "peer table full");
            }
            fx.ui = changed;
        }
    } else if (hdr->channel == s.cfg.channel && !in_call_locked()) {
        ptt_peers_touch(&s.peers, hdr->device_id, src_ip, hdr->channel, now);
        bool play = false;
        ptt_floor_event_t ev = ptt_floor_on_packet(&s.floor, hdr->type, hdr->device_id, hdr->talk_id, now, &play);
        handle_floor_event_locked(ev, now, &fx);
        if (play && (hdr->codec == PTT_CODEC_PCM16 || hdr->codec == PTT_CODEC_IMA_ADPCM)) {
            ptt_jb_put(&s.jb, hdr->seq, hdr->codec, payload, hdr->payload_len);
        }
    }
    xSemaphoreGive(s.lock);
    apply_effects(&fx);
}

/* ------------------------------------------------------------------ transmit (audio_tx task) */

static void send_to_all(const uint32_t *ips, int n, const uint8_t *pkt, size_t len)
{
    for (int i = 0; i < n; i++) {
        ptt_net_send(ips[i], pkt, len);
    }
}

static void audio_tx_task(void *arg)
{
    (void)arg;
    static int16_t pcm[AUDIO_FRAME_SAMPLES];
    static int16_t ref[AUDIO_FRAME_SAMPLES];
    static uint8_t enc[AUDIO_FRAME_SAMPLES * 2];
    const bool hw_ref = audio_io_has_hw_ref();
    static uint8_t pkt[PTT_MAX_PACKET];
    static uint32_t ips[PTT_MAX_PEERS];
    ptt_adpcm_state_t adpcm;
    ptt_adpcm_init(&adpcm);

    for (;;) {
        /* Read continuously so the DMA never holds stale audio when the button goes down. */
        if (audio_io_read_ref(pcm, ref, AUDIO_FRAME_SAMPLES) != AUDIO_FRAME_SAMPLES) {
            continue;
        }

        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool call_active = s.call.state == SIP_CALL_ACTIVE;
        xSemaphoreGive(s.lock);
        if (call_active) {
            call_audio_mic(pcm, hw_ref ? ref : NULL); /* echo control, 8 kHz, RTP */
            continue;
        }

        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool talking = s.floor.state == PTT_FLOOR_TX;
        bool send_start = talking && s.start_repeats > 0;
        bool send_end = !send_start && s.end_repeats > 0;
        if (send_start) {
            s.start_repeats--;
        }
        if (send_end) {
            s.end_repeats--;
        }
        ptt_hdr_t h = {
            .device_id = s.my_id,
            .talk_id = s.floor.talk_id,
            .seq = s.tx_seq,
            .channel = s.cfg.channel,
        };
        uint32_t end_talk_id = s.end_talk_id;
        if (talking) {
            s.tx_seq++;
        }
        int n = (talking || send_end) ? ptt_peers_ips(&s.peers, s.cfg.channel, ips, PTT_MAX_PEERS) : 0;
        xSemaphoreGive(s.lock);

        if (talking && h.seq == 0) {
            ptt_adpcm_init(&adpcm); /* fresh encoder per talk */
        }
        if (n == 0) {
            continue;
        }
        if (send_start) {
            h.type = PTT_MSG_TALK_START;
            h.payload_len = 0;
            send_to_all(ips, n, pkt, ptt_proto_build(pkt, sizeof(pkt), &h, NULL));
        }
        if (talking) {
            h.type = PTT_MSG_AUDIO;
            h.codec = TX_CODEC;
            if (TX_CODEC == PTT_CODEC_IMA_ADPCM) {
                h.payload_len = (uint16_t)ptt_adpcm_encode(&adpcm, pcm, AUDIO_FRAME_SAMPLES, enc);
            } else {
                memcpy(enc, pcm, sizeof(pcm)); /* ESP32 is little endian like the wire */
                h.payload_len = sizeof(pcm);
            }
            send_to_all(ips, n, pkt, ptt_proto_build(pkt, sizeof(pkt), &h, enc));
        }
        if (send_end) {
            h.type = PTT_MSG_TALK_END;
            h.talk_id = end_talk_id; /* the talk that ended, even if a new one already started */
            h.codec = 0;
            h.payload_len = 0;
            send_to_all(ips, n, pkt, ptt_proto_build(pkt, sizeof(pkt), &h, NULL));
        }
    }
}

/* ------------------------------------------------------------------ receive playback (audio_rx task) */

static void play_tone(tone_t tone, int16_t *pcm)
{
    for (int i = 0; i < tone_frames(tone); i++) {
        tone_frame(tone, i, pcm);
        audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    }
}

/* One walkie-talkie reception, from the start beep to the roger beep. */
static void play_ptt_session(void)
{
    static int16_t pcm[AUDIO_FRAME_SAMPLES];
    static int16_t last[AUDIO_FRAME_SAMPLES];
    static uint8_t frame[PTT_JB_FRAME_MAX];

    audio_io_speaker_enable(true);
    play_tone(TONE_RX_START, pcm); /* the jitter buffer fills meanwhile */
    bool have_last = false;
    bool aborted = false;

    for (;;) {
        uint16_t len = 0;
        uint8_t codec = 0;
        xSemaphoreTake(s.lock, portMAX_DELAY);
        ptt_jb_result_t r = ptt_jb_get(&s.jb, frame, &len, &codec);
        ptt_floor_state_t state = s.floor.state;
        bool call = in_call_locked();
        xSemaphoreGive(s.lock);

        if (state == PTT_FLOOR_TX || call) {
            aborted = true; /* we started talking or a call took over: stop now */
            break;
        }
        if (r == PTT_JB_FRAME) {
            int n = 0;
            if (codec == PTT_CODEC_IMA_ADPCM) {
                n = ptt_adpcm_decode(frame, len, pcm, AUDIO_FRAME_SAMPLES);
            } else if (codec == PTT_CODEC_PCM16) {
                n = len / 2 > AUDIO_FRAME_SAMPLES ? AUDIO_FRAME_SAMPLES : len / 2;
                memcpy(pcm, frame, n * 2);
            }
            for (int i = n; i < AUDIO_FRAME_SAMPLES; i++) {
                pcm[i] = 0;
            }
            memcpy(last, pcm, sizeof(pcm));
            have_last = true;
        } else if (r == PTT_JB_LOST && have_last) {
            /* Conceal one lost frame with the previous one at half level, then silence. */
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                pcm[i] = last[i] / 2;
            }
            have_last = false;
        } else {
            if (r == PTT_JB_BUFFERING && state != PTT_FLOOR_RX) {
                break; /* talk over and everything played */
            }
            memset(pcm, 0, sizeof(pcm));
        }
        audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool roger = s.rx_roger && !aborted;
    s.rx_roger = false;
    ESP_LOGI(TAG, "rx done: %lu frames, %lu lost, %lu late, %lu underruns",
             (unsigned long)s.jb.stat_received, (unsigned long)s.jb.stat_lost,
             (unsigned long)s.jb.stat_late, (unsigned long)s.jb.stat_underruns);
    xSemaphoreGive(s.lock);

    if (roger) {
        play_tone(TONE_ROGER, pcm);
    }
    if (!aborted) {
        /* Let the DMA drain before cutting the amplifier, or the tail clicks. */
        memset(pcm, 0, sizeof(pcm));
        audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
        audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    }
    audio_io_speaker_enable(false);
}

static sip_call_state_t call_state(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    sip_call_state_t st = s.call.state;
    xSemaphoreGive(s.lock);
    return st;
}

/* Ring (incoming) or ringback (outgoing, ringing there) until the call state moves on. */
static void play_ringing(tone_t tone, sip_call_state_t while_state)
{
    static int16_t pcm[AUDIO_FRAME_SAMPLES];
    audio_io_speaker_enable(true);
    for (int i = 0; call_state() == while_state; i = (i + 1) % tone_frames(tone)) {
        tone_frame(tone, i, pcm);
        audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    }
    audio_io_speaker_enable(false);
}

/* Full-duplex phone call: one speaker frame every 20 ms, written without gaps so the echo path stays fixed. */
static void play_call(void)
{
    static int16_t pcm[AUDIO_FRAME_SAMPLES];
    call_audio_begin();
    audio_io_speaker_enable(true);
    while (call_state() == SIP_CALL_ACTIVE) {
        call_audio_speaker_frame(pcm);
        audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    }
    call_audio_end();
    play_tone(TONE_HANGUP, pcm);
    memset(pcm, 0, sizeof(pcm));
    audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    audio_io_write(pcm, AUDIO_FRAME_SAMPLES);
    audio_io_speaker_enable(false);
}

static void audio_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));

        xSemaphoreTake(s.lock, portMAX_DELAY);
        sip_call_state_t call = s.call.state;
        bool ptt_rx = s.floor.state == PTT_FLOOR_RX;
        xSemaphoreGive(s.lock);

        switch (call) {
        case SIP_CALL_INCOMING:
            play_ringing(TONE_RING, SIP_CALL_INCOMING);
            break;
        case SIP_CALL_RINGBACK:
            play_ringing(TONE_RINGBACK, SIP_CALL_RINGBACK);
            break;
        case SIP_CALL_ACTIVE:
            play_call();
            break;
        case SIP_CALL_OUTGOING:
            vTaskDelay(pdMS_TO_TICKS(20)); /* dialling: wait for ringing or answer */
            break;
        case SIP_CALL_IDLE:
            if (ptt_rx) {
                play_ptt_session();
            }
            break;
        }
        /* Whatever just finished, look again at once: the next state may already be here. */
        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool again = s.call.state != SIP_CALL_IDLE || s.floor.state == PTT_FLOOR_RX;
        xSemaphoreGive(s.lock);
        if (again) {
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

/* ------------------------------------------------------------------ input and housekeeping (app task) */

static void post_event(ui_event_t ev)
{
    xQueueSend(s_events, &ev, 0);
}

static void post_app_event(int ev)
{
    ui_event_t e = (ui_event_t)ev;
    xQueueSend(s_events, &e, 0);
}

static void on_button(board_button_role_t role, button_event_t ev)
{
    switch (role) {
    case BOARD_BTN_PTT:
        if (ev == BUTTON_VERY_LONG) {
            post_app_event(APP_EV_BOOT_VERY_LONG);
        } else if (!sip_on()) {
            /* Walkie-talkie only: talk from the moment the button goes down. */
            if (ev == BUTTON_PRESS) {
                post_event(UI_EV_PTT_DOWN);
            } else if (ev == BUTTON_RELEASE) {
                post_event(UI_EV_PTT_UP);
            }
        } else if (ev == BUTTON_CLICK) {
            post_app_event(APP_EV_BOOT_CLICK);
        } else if (ev == BUTTON_LONG_PRESS) {
            post_app_event(APP_EV_BOOT_LONG);
        } else if (ev == BUTTON_RELEASE) {
            post_app_event(APP_EV_BOOT_RELEASE);
        }
        break;
    case BOARD_BTN_VOL_UP:
        if (ev == BUTTON_CLICK) {
            post_event(UI_EV_VOL_UP);
        } else if (ev == BUTTON_LONG_PRESS) {
            post_event(UI_EV_CH_UP);
        }
        break;
    case BOARD_BTN_VOL_DOWN:
        if (ev == BUTTON_CLICK) {
            post_event(UI_EV_VOL_DOWN);
        } else if (ev == BUTTON_LONG_PRESS) {
            post_event(UI_EV_CH_DOWN);
        }
        break;
    }
}

static void on_wifi(bool connected, uint32_t ip)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.wifi_up = connected;
    xSemaphoreGive(s.lock);
    effects_t fx = {.ui = true};
    apply_effects(&fx);
    if (connected) {
        send_hello();
        sip_client_network_up(ip);
    } else {
        sip_client_network_down();
    }
}

/* ------------------------------------------------------------------ SIP callbacks (SIP task) */

static void on_sip_call(const sip_call_info_t *info)
{
    uint32_t now = now_ms();
    effects_t fx = {.ui = true, .wake_player = true};
    char text[48];
    xSemaphoreTake(s.lock, portMAX_DELAY);
    sip_call_state_t prev = s.call.state;
    s.call = *info;
    s.last_activity_ms = now;
    if (info->state == SIP_CALL_ACTIVE && prev != SIP_CALL_ACTIVE) {
        s.call_start_ms = now;
    }
    if (info->state == SIP_CALL_IDLE && prev != SIP_CALL_IDLE) {
        switch (info->reason) {
        case SIP_END_MISSED:
            snprintf(text, sizeof(text), "Missed: %s", info->peer);
            set_notice_locked(text, true, now);
            s.notice_until_ms = now + 10000; /* keep a missed call visible longer */
            break;
        case SIP_END_BUSY:
            set_notice_locked("Busy", true, now);
            break;
        case SIP_END_REJECTED:
            set_notice_locked("Declined", true, now);
            break;
        case SIP_END_UNREACHABLE:
            set_notice_locked("No answer", true, now);
            break;
        case SIP_END_FAILED:
            snprintf(text, sizeof(text), "Call failed (%d)", info->status);
            set_notice_locked(text, true, now);
            break;
        default:
            set_notice_locked("Call ended", false, now);
            break;
        }
    }
    bool auto_answer = info->state == SIP_CALL_INCOMING && prev != SIP_CALL_INCOMING && info->auto_answer &&
                       s.cfg.sip_auto_answer;
    xSemaphoreGive(s.lock);
    if (auto_answer) {
        post_app_event(APP_EV_AUTO_ANSWER);
    }
    apply_effects(&fx);
}

static void on_sip_reg(sip_reg_state_t st)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.sip_reg = st;
    xSemaphoreGive(s.lock);
    effects_t fx = {.ui = true};
    apply_effects(&fx);
}

static bool sip_is_busy(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool busy = s.floor.state != PTT_FLOOR_IDLE;
    xSemaphoreGive(s.lock);
    return busy;
}

/* ------------------------------------------------------------------ phone setup */

/* HTTP task: only hand over to the app task (stopping the portal from its own handler would deadlock). */
static void on_setup_saved(const device_config_t *cfg)
{
    s_saved_cfg = *cfg;
    post_app_event(APP_EV_SETUP_SAVED);
}

static void on_setup_cancelled(void)
{
    post_app_event(APP_EV_SETUP_CANCEL);
}

/* App task, without s.lock. */
static void setup_enter(uint32_t now)
{
    if (setup_portal_active()) {
        return;
    }
    ESP_LOGI(TAG, "phone setup: %u bytes internal RAM free", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    char ssid[33], pass[17];
    snprintf(ssid, sizeof(ssid), "PTT-%04X-Setup", (unsigned)(s.my_id & 0xFFFF));
    snprintf(pass, sizeof(pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    xSemaphoreTake(s.lock, portMAX_DELAY);
    device_config_t cfg = s.cfg;
    xSemaphoreGive(s.lock);
    const setup_portal_cb_t cb = {.saved = on_setup_saved, .cancelled = on_setup_cancelled};
    esp_err_t err = setup_portal_start(&cfg, ssid, pass, &cb);
    if (err != ESP_OK) {
        /* Show why on screen (there may be no serial console) and let the app task try again. */
        char text[32];
        snprintf(text, sizeof(text), "Setup error %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", text);
        xSemaphoreTake(s.lock, portMAX_DELAY);
        set_notice_locked(text, true, now);
        s.notice_until_ms = now + 5000;
        s.setup_auto_done = false;
        s.setup_retry_ms = now + 5000;
        xSemaphoreGive(s.lock);
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.setup_active = true;
    strlcpy(s.setup_ssid, ssid, sizeof(s.setup_ssid));
    strlcpy(s.setup_pass, pass, sizeof(s.setup_pass));
    s.setup_until_ms = now + SETUP_TIMEOUT_MS;
    s.last_activity_ms = now;
    xSemaphoreGive(s.lock);
}

static void setup_leave(void)
{
    setup_portal_stop();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.setup_active = false;
    xSemaphoreGive(s.lock);
}

typedef enum { SIP_ACT_NONE, SIP_ACT_CALL, SIP_ACT_ANSWER, SIP_ACT_HANGUP } sip_action_t;

/* What a tap on BOOT or the phone button means right now. */
static sip_action_t call_button_action_locked(uint32_t now)
{
    switch (s.call.state) {
    case SIP_CALL_INCOMING:
        return SIP_ACT_ANSWER;
    case SIP_CALL_OUTGOING:
    case SIP_CALL_RINGBACK:
    case SIP_CALL_ACTIVE:
        return SIP_ACT_HANGUP;
    case SIP_CALL_IDLE:
        break;
    }
    if (!sip_on()) {
        return SIP_ACT_NONE;
    }
    if (s.floor.state != PTT_FLOOR_IDLE) {
        set_notice_locked("Walkie-talkie busy", true, now);
        return SIP_ACT_NONE;
    }
    if (s.sip_reg != SIP_REG_OK) {
        set_notice_locked("Phone not registered", true, now);
        return SIP_ACT_NONE;
    }
    return SIP_ACT_CALL;
}

static void handle_input(ui_event_t ev, uint32_t now, effects_t *fx, bool *hello_now)
{
    char text[32];
    bool save = false;
    sip_action_t sip_action = SIP_ACT_NONE;
    enum { SETUP_NONE, SETUP_ENTER, SETUP_LEAVE } setup_action = SETUP_NONE;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.last_activity_ms = now;
    fx->ui = true;
    /* In setup mode BOOT (or the X on screen) leaves it, if there is a network to go back to. */
    if (s.setup_active && ((int)ev == APP_EV_BOOT_CLICK || (int)ev == UI_EV_SETUP_EXIT ||
                           (int)ev == APP_EV_SETUP_CANCEL)) {
        if (s.cfg.wifi_ssid[0]) {
            setup_action = SETUP_LEAVE;
        } else {
            set_notice_locked("Set up Wi-Fi first", true, now);
        }
        ev = (ui_event_t)-1;
    }
    switch ((int)ev) {
    case APP_EV_BOOT_VERY_LONG:
        if (in_call_locked()) {
            break;
        }
        if (s.floor.state == PTT_FLOOR_TX) {
            handle_floor_event_locked(ptt_floor_release(&s.floor, now), now, fx);
        }
        s.boot_ptt = false;
        setup_action = SETUP_ENTER;
        break;
    case APP_EV_SETUP_SAVED:
        /* Keep what changed on the device meanwhile; take Wi-Fi, SIP and name from the page. */
        s_saved_cfg.channel = s.cfg.channel;
        s_saved_cfg.volume = s.cfg.volume;
        settings_save_all(&s_saved_cfg);
        set_notice_locked("Saved - restarting", false, now);
        s.notice_until_ms = now + REBOOT_DELAY_MS + 1000;
        s.reboot_at_ms = now + REBOOT_DELAY_MS;
        break;
    case UI_EV_CALL:
    case APP_EV_BOOT_CLICK:
        sip_action = call_button_action_locked(now);
        break;
    case UI_EV_REJECT:
        if (s.call.state == SIP_CALL_INCOMING) {
            sip_action = SIP_ACT_HANGUP;
        }
        break;
    case APP_EV_AUTO_ANSWER:
        if (s.call.state == SIP_CALL_INCOMING) {
            sip_action = SIP_ACT_ANSWER;
        }
        break;
    case APP_EV_BOOT_LONG:
        if (s.call.state == SIP_CALL_INCOMING) {
            sip_action = SIP_ACT_HANGUP; /* hold to decline */
            break;
        }
        if (in_call_locked()) {
            break;
        }
        s.boot_ptt = true; /* hold BOOT = walkie-talkie */
        /* fall through */
    case UI_EV_PTT_DOWN:
        if (in_call_locked()) {
            set_notice_locked("In a call", true, now);
            break;
        }
        if (!s.wifi_up) {
            set_notice_locked("No Wi-Fi", true, now);
            break;
        }
        handle_floor_event_locked(ptt_floor_press(&s.floor, esp_random(), now), now, fx);
        break;
    case APP_EV_BOOT_RELEASE:
        if (!s.boot_ptt) {
            break;
        }
        s.boot_ptt = false;
        /* fall through */
    case UI_EV_PTT_UP:
        handle_floor_event_locked(ptt_floor_release(&s.floor, now), now, fx);
        break;
    case UI_EV_CH_UP:
    case UI_EV_CH_DOWN:
        if (s.floor.state != PTT_FLOOR_IDLE) {
            set_notice_locked("Wait until idle", true, now);
            break;
        }
        s.cfg.channel = ev == UI_EV_CH_UP ? (s.cfg.channel % PTT_MAX_CHANNEL) + 1
                                          : (s.cfg.channel + PTT_MAX_CHANNEL - 2) % PTT_MAX_CHANNEL + 1;
        ptt_floor_reset(&s.floor);
        ptt_jb_reset(&s.jb);
        snprintf(text, sizeof(text), "Channel %d", s.cfg.channel);
        set_notice_locked(text, false, now);
        save = true;
        *hello_now = true;
        break;
    case UI_EV_VOL_UP:
    case UI_EV_VOL_DOWN: {
        int v = s.cfg.volume + (ev == UI_EV_VOL_UP ? 10 : -10);
        s.cfg.volume = v < 0 ? 0 : (v > 100 ? 100 : v);
        audio_io_set_volume(s.cfg.volume);
        snprintf(text, sizeof(text), "Volume %d", s.cfg.volume);
        set_notice_locked(text, false, now);
        save = true;
        break;
    }
    }
    settings_t cfg = s.cfg;
    xSemaphoreGive(s.lock);
    if (save) {
        settings_save(&cfg);
    }
    /* SIP calls take the SIP lock, whose callbacks take ours: never call them with s.lock held. */
    switch (sip_action) {
    case SIP_ACT_CALL:
        if (!sip_client_call(s.cfg.sip_target)) {
            xSemaphoreTake(s.lock, portMAX_DELAY);
            set_notice_locked("Cannot call now", true, now);
            xSemaphoreGive(s.lock);
        }
        break;
    case SIP_ACT_ANSWER:
        sip_client_answer();
        break;
    case SIP_ACT_HANGUP:
        sip_client_hangup();
        break;
    case SIP_ACT_NONE:
        break;
    }
    if (setup_action == SETUP_ENTER) {
        setup_enter(now);
    } else if (setup_action == SETUP_LEAVE) {
        setup_leave();
    }
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_hello = 0;
    uint32_t notice_shown_until = 0;
    uint32_t last_call_tick = 0;

    for (;;) {
        ui_event_t ev;
        effects_t fx = {0};
        bool hello_now = false;
        uint32_t now = now_ms();

        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            now = now_ms();
            handle_input(ev, now, &fx, &hello_now);
        }

        xSemaphoreTake(s.lock, portMAX_DELAY);
        handle_floor_event_locked(ptt_floor_tick(&s.floor, now), now, &fx);
        /* Redraw once when a notice expires. */
        if (s.notice[0] && notice_shown_until != s.notice_until_ms && (int32_t)(now - s.notice_until_ms) >= 0) {
            notice_shown_until = s.notice_until_ms;
            fx.ui = true;
        }
        bool active = s.floor.state != PTT_FLOOR_IDLE || in_call_locked() || (now - s.last_activity_ms) < DIM_AFTER_MS;
        bool call_active = s.call.state == SIP_CALL_ACTIVE;
        xSemaphoreGive(s.lock);
        if (call_active && now - last_call_tick >= 1000) {
            last_call_tick = now; /* call timer */
            fx.ui = true;
        }

        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool setup_on = s.setup_active;
        bool have_wifi = s.cfg.wifi_ssid[0] != '\0';
        bool want_setup = !setup_on && !s.setup_auto_done && (int32_t)(now - s.setup_retry_ms) >= 0 &&
                          (!have_wifi || (!wifi_ever_connected() && now > SETUP_AUTO_AFTER_MS));
        bool setup_expired = setup_on && have_wifi && (int32_t)(now - s.setup_until_ms) > 0;
        uint32_t reboot_at = s.reboot_at_ms;
        if (want_setup) {
            s.setup_auto_done = true; /* only once per boot; BOOT 8 s opens it again */
        }
        xSemaphoreGive(s.lock);
        if (want_setup) {
            if (have_wifi) {
                ESP_LOGW(TAG, "Wi-Fi not reachable: opening phone setup");
            } else {
                ESP_LOGW(TAG, "no Wi-Fi yet: opening phone setup");
            }
            setup_enter(now);
            fx.ui = true;
        } else if (setup_expired) {
            setup_leave();
            fx.ui = true;
        }
        if (reboot_at && (int32_t)(now - reboot_at) >= 0) {
            apply_effects(&fx);
            ESP_LOGI(TAG, "restarting with the new settings");
            esp_restart();
        }

        if (active == s.dimmed) {
            s.dimmed = !active;
            board_backlight_set(active ? BACKLIGHT_ON : BACKLIGHT_DIM);
        }

        if (hello_now || now - last_hello >= HELLO_PERIOD_MS) {
            last_hello = now;
            int battery = board_battery_percent();
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.battery = battery;
            if (ptt_peers_expire(&s.peers, now)) {
                fx.ui = true;
            }
            xSemaphoreGive(s.lock);
            send_hello();
            fx.ui = true; /* battery and online count */
        }
        apply_effects(&fx);
    }
}

/* ------------------------------------------------------------------ start */

esp_err_t ptt_app_start(const board_t *b)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    s.lock = xSemaphoreCreateMutex();
    s.my_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    s.battery = -1;
    settings_load(&s.cfg, s.my_id);
    ptt_floor_init(&s.floor, s.my_id);
    ptt_peers_init(&s.peers);
    ptt_jb_init(&s.jb, CONFIG_PTT_JITTER_FRAMES);
    s.last_activity_ms = now_ms();
    s_events = xQueueCreate(16, sizeof(ui_event_t));
    ESP_LOGI(TAG, "device %08lx \"%s\" channel %d", (unsigned long)s.my_id, s.cfg.name, s.cfg.channel);

    audio_io_set_volume(s.cfg.volume);
    ESP_ERROR_CHECK(ui_init(b, post_event));
    board_backlight_set(BACKLIGHT_ON);
    effects_t fx = {.ui = true};
    apply_effects(&fx);

    xTaskCreatePinnedToCore(audio_rx_task, "audio_rx", 6144, NULL, 19, &s_rx_task, 1);
    xTaskCreatePinnedToCore(audio_tx_task, "audio_tx", 6144, NULL, 20, NULL, 1);

    ESP_ERROR_CHECK(call_audio_init(audio_io_has_hw_ref()));
    /* DHCP host names allow letters, digits and '-' only. */
    char hostname[sizeof(s.cfg.name)];
    for (size_t i = 0; i < sizeof(hostname); i++) {
        char c = s.cfg.name[i];
        hostname[i] = (c == '\0' || isalnum((unsigned char)c)) ? c : '-';
    }
    ESP_ERROR_CHECK(wifi_start(hostname, s.cfg.wifi_ssid, s.cfg.wifi_pass, on_wifi));
    ESP_ERROR_CHECK(ptt_net_start(CONFIG_PTT_UDP_PORT, on_packet));
    const sip_client_cb_t sip_cb = {
        .call_changed = on_sip_call,
        .reg_changed = on_sip_reg,
        .is_busy = sip_is_busy,
        .rtp_frame = call_audio_rtp_in,
    };
    ESP_ERROR_CHECK(sip_client_start(&sip_cb, &s.cfg));
    /* Last: it may open phone setup at once, which needs Wi-Fi running. */
    xTaskCreatePinnedToCore(app_task, "app", 6144, NULL, 5, NULL, 0);
    buttons_start(b, on_button);
    ESP_LOGI(TAG, "running: %u bytes internal RAM free (largest block %u), %u PSRAM",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}
