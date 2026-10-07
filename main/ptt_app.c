#include "ptt_app.h"

#include <stdio.h>
#include <string.h>

#include "audio_io.h"
#include "buttons.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
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

    /* screen */
    char notice[32];
    bool notice_warn;
    uint32_t notice_until_ms;
    uint32_t last_activity_ms;
    bool dimmed;
} app_t;

static app_t s;
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

static void build_view_locked(ui_view_t *v, uint32_t now)
{
    memset(v, 0, sizeof(*v));
    if (!s.wifi_up) {
        v->mode = UI_MODE_WIFI;
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
    } else if (hdr->channel == s.cfg.channel) {
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
    static uint8_t enc[AUDIO_FRAME_SAMPLES * 2];
    static uint8_t pkt[PTT_MAX_PACKET];
    static uint32_t ips[PTT_MAX_PEERS];
    ptt_adpcm_state_t adpcm;
    ptt_adpcm_init(&adpcm);

    for (;;) {
        /* Read continuously so the DMA never holds stale audio when the button goes down. */
        if (audio_io_read(pcm, AUDIO_FRAME_SAMPLES) != AUDIO_FRAME_SAMPLES) {
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

static void audio_rx_task(void *arg)
{
    (void)arg;
    static int16_t pcm[AUDIO_FRAME_SAMPLES];
    static int16_t last[AUDIO_FRAME_SAMPLES];
    static uint8_t frame[PTT_JB_FRAME_MAX];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool start = s.floor.state == PTT_FLOOR_RX;
        xSemaphoreGive(s.lock);
        if (!start) {
            continue; /* stale wake-up */
        }

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
            xSemaphoreGive(s.lock);

            if (state == PTT_FLOOR_TX) {
                aborted = true; /* we started talking: speaker off now */
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

        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool again = s.floor.state == PTT_FLOOR_RX;
        xSemaphoreGive(s.lock);
        if (again) {
            xTaskNotifyGive(xTaskGetCurrentTaskHandle()); /* someone started while we drained */
        }
    }
}

/* ------------------------------------------------------------------ input and housekeeping (app task) */

static void post_event(ui_event_t ev)
{
    xQueueSend(s_events, &ev, 0);
}

static void on_button(board_button_role_t role, button_event_t ev)
{
    switch (role) {
    case BOARD_BTN_PTT:
        if (ev == BUTTON_PRESS) {
            post_event(UI_EV_PTT_DOWN);
        } else if (ev == BUTTON_RELEASE) {
            post_event(UI_EV_PTT_UP);
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
    (void)ip;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.wifi_up = connected;
    xSemaphoreGive(s.lock);
    effects_t fx = {.ui = true};
    apply_effects(&fx);
    if (connected) {
        send_hello();
    }
}

static void handle_input(ui_event_t ev, uint32_t now, effects_t *fx, bool *hello_now)
{
    char text[32];
    bool save = false;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.last_activity_ms = now;
    fx->ui = true;
    switch (ev) {
    case UI_EV_PTT_DOWN:
        if (!s.wifi_up) {
            set_notice_locked("No Wi-Fi", true, now);
            break;
        }
        handle_floor_event_locked(ptt_floor_press(&s.floor, esp_random(), now), now, fx);
        break;
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
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_hello = 0;
    uint32_t notice_shown_until = 0;

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
        bool active = s.floor.state != PTT_FLOOR_IDLE || (now - s.last_activity_ms) < DIM_AFTER_MS;
        xSemaphoreGive(s.lock);

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
    xTaskCreatePinnedToCore(app_task, "app", 6144, NULL, 5, NULL, 0);

    ESP_ERROR_CHECK(wifi_start(s.cfg.name, on_wifi));
    ESP_ERROR_CHECK(ptt_net_start(CONFIG_PTT_UDP_PORT, on_packet));
    buttons_start(b, on_button);
    return ESP_OK;
}
