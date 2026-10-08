#include "sdkconfig.h"

#if CONFIG_PTT_PRESENCE_ENABLE

#if !CONFIG_BT_NIMBLE_ENABLED
#error "PTT_PRESENCE_ENABLE needs NimBLE: set CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED (see sdkconfig.defaults.cube)"
#endif

#include "ble_presence.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "presence_db.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void); /* NimBLE's NVS-backed bond store */

static const char *TAG = "ble_presence";

#define ENROLL_MS (CONFIG_PTT_PRESENCE_ENROLL_SECS * 1000)
#define NAME_READ_TIMEOUT_MS 3000

static ble_notice_cb_t s_notice;
static char s_name[20];
static uint8_t s_own_addr_type;

static volatile bool s_enrolling;
static uint32_t s_enroll_until_ms;

/* One pending enrolment between "bonded" and "name read done / timed out". */
static volatile bool s_pending;
static uint32_t s_pending_since_ms;
static uint16_t s_pending_conn;
static uint8_t s_pending_irk[16];
static uint8_t s_pending_addr[6];
static char s_pending_name[PRESENCE_NAME_LEN];

/* A tiny GATT service whose one characteristic needs encryption to read, so a
 * phone that connects is nudged into pairing (we also start it ourselves). */
/* 0xA1B2... random 128-bit UUIDs, little-endian as NimBLE wants. */
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0xb2, 0xa1, 0x00, 0x00, 0x00, 0xcb);
static const ble_uuid128_t chr_uuid =
    BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0xb2, 0xa1, 0x01, 0x00, 0x00, 0xcb);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int gap_event(struct ble_gap_event *event, void *arg);

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *s = "CUBE";
        return os_mbuf_append(ctxt->om, s, 4) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &chr_uuid.u,
                    .access_cb = gatt_access,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                },
                {0},
            },
    },
    {0},
};

/* ------------------------------------------------------------------ scan */

static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        .itvl = 0x0140,  /* 200 ms between scan windows... */
        .window = 0x0030, /* ...30 ms listening: ~15% duty, gentle on Wi-Fi */
        .passive = 1,
        .filter_duplicates = 0, /* we want repeat sightings to refresh presence */
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
    }
}

static void start_adv(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = (uint8_t)strlen(s_name);
    f.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) {
        ESP_LOGW(TAG, "adv fields failed");
        return;
    }
    struct ble_gap_adv_params a = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, ENROLL_MS, &a, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv start failed: %d", rc);
    }
}

/* ------------------------------------------------------------------ enrolment finish */

static void finalize_enroll(void)
{
    if (!s_pending) {
        return;
    }
    s_pending = false;
    int id = presence_db_enroll(s_pending_irk, s_pending_addr, s_pending_name[0] ? s_pending_name : NULL);
    if (id >= 0 && s_notice) {
        char msg[40];
        snprintf(msg, sizeof(msg), "Paired: %s", s_pending_name[0] ? s_pending_name : "phone");
        s_notice(msg, false);
    }
    /* One phone per window: stop advertising and drop the link. */
    s_enrolling = false;
    ble_gap_adv_stop();
    if (s_pending_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_pending_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int name_read_cb(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)arg;
    if (error && error->status == 0 && attr && attr->om) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > PRESENCE_NAME_LEN - 1) {
            len = PRESENCE_NAME_LEN - 1;
        }
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(attr->om, s_pending_name, len, &copied);
        s_pending_name[copied] = '\0';
    }
    finalize_enroll();
    return 0;
}

/* Try to read the phone's GAP device name (0x2A00). Best effort: finalize runs
 * either way (here, or from the housekeeping timeout). */
static void read_peer_name(uint16_t conn)
{
    static const ble_uuid16_t name_uuid = BLE_UUID16_INIT(0x2A00);
    int rc = ble_gattc_read_by_uuid(conn, 0x0001, 0xffff, &name_uuid.u, name_read_cb, NULL);
    if (rc != 0) {
        ESP_LOGD(TAG, "name read start failed: %d", rc);
        finalize_enroll();
    }
}

/* ------------------------------------------------------------------ GAP events */

static void on_disc(struct ble_gap_event *event)
{
    struct ble_hs_adv_fields f;
    char name[PRESENCE_NAME_LEN] = "";
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0 && f.name_len) {
        uint8_t n = f.name_len < PRESENCE_NAME_LEN - 1 ? f.name_len : PRESENCE_NAME_LEN - 1;
        memcpy(name, f.name, n);
        name[n] = '\0';
    }
    presence_db_on_seen(event->disc.addr.val, event->disc.rssi, name[0] ? name : NULL);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        on_disc(event);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan(); /* keep scanning forever */
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            return 0;
        }
        if (!s_enrolling) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        /* Ask to pair now so we receive the identity key (IRK). */
        ble_gap_security_initiate(event->connect.conn_handle);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (!s_enrolling || event->enc_change.status != 0) {
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
            return 0;
        }
        union ble_store_key key = {0};
        key.sec.peer_addr = desc.peer_id_addr;
        union ble_store_value value = {0};
        if (ble_store_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &key, &value) != 0 || !value.sec.irk_present) {
            ESP_LOGW(TAG, "bonded but no IRK: phone has privacy off?");
            if (s_notice) {
                s_notice("Pair failed (no IRK)", true);
            }
            s_enrolling = false;
            ble_gap_adv_stop();
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        memcpy(s_pending_irk, value.sec.irk, 16);
        memcpy(s_pending_addr, desc.peer_id_addr.val, 6);
        s_pending_name[0] = '\0';
        s_pending_conn = event->enc_change.conn_handle;
        s_pending_since_ms = now_ms();
        s_pending = true;
        read_peer_name(event->enc_change.conn_handle);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        if (s_pending && event->disconnect.conn.conn_handle == s_pending_conn) {
            s_pending_conn = BLE_HS_CONN_HANDLE_NONE;
            finalize_enroll(); /* enrol with whatever name we have */
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_enrolling = false; /* window elapsed with no pairing */
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Re-enrolling the same phone: drop the stale bond and pair again. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ housekeeping */

static void housekeeping(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        presence_db_sweep();
        if (s_enrolling && (int32_t)(now_ms() - s_enroll_until_ms) >= 0) {
            s_enrolling = false;
            ble_gap_adv_stop();
        }
        if (s_pending && (now_ms() - s_pending_since_ms) > NAME_READ_TIMEOUT_MS) {
            finalize_enroll(); /* name read never came back */
        }
    }
}

/* ------------------------------------------------------------------ host start */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    start_scan();
    ESP_LOGI(TAG, "scanning as \"%s\"", s_name);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run(); /* returns only at nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_presence_start(const char *suffix, ble_notice_cb_t notice)
{
    s_notice = notice;
    s_pending_conn = BLE_HS_CONN_HANDLE_NONE;
    snprintf(s_name, sizeof(s_name), "CUBE-%s", suffix ? suffix : "0000");

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Just Works bonding with identity-key exchange: we need their IRK. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "GATT register failed: %d", rc);
    }
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCore(housekeeping, "presence_hk", 3072, NULL, 4, NULL, 0);
    return ESP_OK;
}

void ble_presence_enroll_begin(void)
{
    if (s_enrolling) {
        return;
    }
    s_enrolling = true;
    s_enroll_until_ms = now_ms() + ENROLL_MS;
    start_adv();
    if (s_notice) {
        s_notice("Pairing: open phone BT", false);
    }
    ESP_LOGI(TAG, "enrol window open for %d s", CONFIG_PTT_PRESENCE_ENROLL_SECS);
}

bool ble_presence_enroll_active(void)
{
    return s_enrolling;
}

const char *ble_presence_name(void)
{
    return s_name;
}

#endif /* CONFIG_PTT_PRESENCE_ENABLE */
