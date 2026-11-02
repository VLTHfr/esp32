#include "ble_hid.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Extern declaration cho hàm init bonding storage */
void ble_store_config_init(void);

static const char *TAG = "BLE_HID";

/* ============================================================
 *  Module state
 * ============================================================ */
static uint8_t  own_addr_type;
static uint16_t s_conn_handle = 0xFFFF;
static uint16_t s_hid_input_val_handle;
static uint16_t s_hid_output_val_handle;          // ⭐ Handle cho Output Report
static bool     s_is_connected = false;
static const char *s_device_name = "ESP32-S3 Keyboard";
static ble_hid_conn_cb_t s_conn_cb = NULL;
static ble_hid_leds_cb_t s_leds_cb = NULL;        // ⭐ Callback LED

/* Forward declarations */
static void ble_hid_advertise(void);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

/* ============================================================
 *  HID Report Descriptor (Boot Keyboard với Report ID = 1)
 * ============================================================ */
static const uint8_t hid_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    // Modifier byte (8 bits)
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    // Reserved byte
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    // LED output (5 bits + 3 padding)
    0x95, 0x05,
    0x75, 0x01,
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,
    0x29, 0x05,
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x95, 0x01,
    0x75, 0x03,
    0x91, 0x01,        //   Output (Const)
    // Key array (6 keys)
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0
};

/* ============================================================
 *  UUIDs
 * ============================================================ */
#define GATT_HID_SVC_UUID              0x1812
#define GATT_HID_REPORT_MAP_UUID       0x2A4B
#define GATT_HID_REPORT_UUID           0x2A4D
#define GATT_HID_INFO_UUID             0x2A4A
#define GATT_HID_CONTROL_POINT_UUID    0x2A4C
#define GATT_HID_PROTOCOL_MODE_UUID    0x2A4E
#define GATT_HID_REPORT_REF_UUID       0x2908

static uint8_t hid_info[4] = {0x11, 0x01, 0x00, 0x02};
static uint8_t hid_protocol_mode = 0x01;

/* ============================================================
 *  ASCII → HID Keycode (US layout)
 * ============================================================ */
static bool ascii_to_hid(char c, uint8_t *keycode, uint8_t *modifier)
{
    *modifier = 0;
    *keycode = 0;

    if (c >= 'a' && c <= 'z') { *keycode = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *keycode = 0x04 + (c - 'A'); *modifier = 0x02; return true; }
    if (c >= '1' && c <= '9') { *keycode = 0x1E + (c - '1'); return true; }
    if (c == '0') { *keycode = 0x27; return true; }

    switch (c) {
        case '\n': *keycode = 0x28; return true;
        case '\b': *keycode = 0x2A; return true;
        case '\t': *keycode = 0x2B; return true;
        case ' ':  *keycode = 0x2C; return true;
        case '-':  *keycode = 0x2D; return true;
        case '=':  *keycode = 0x2E; return true;
        case '[':  *keycode = 0x2F; return true;
        case ']':  *keycode = 0x30; return true;
        case '\\': *keycode = 0x31; return true;
        case ';':  *keycode = 0x33; return true;
        case '\'': *keycode = 0x34; return true;
        case '`':  *keycode = 0x35; return true;
        case ',':  *keycode = 0x36; return true;
        case '.':  *keycode = 0x37; return true;
        case '/':  *keycode = 0x38; return true;
    }

    *modifier = 0x02;
    switch (c) {
        case '!':  *keycode = 0x1E; return true;
        case '@':  *keycode = 0x1F; return true;
        case '#':  *keycode = 0x20; return true;
        case '$':  *keycode = 0x21; return true;
        case '%':  *keycode = 0x22; return true;
        case '^':  *keycode = 0x23; return true;
        case '&':  *keycode = 0x24; return true;
        case '*':  *keycode = 0x25; return true;
        case '(':  *keycode = 0x26; return true;
        case ')':  *keycode = 0x27; return true;
        case '_':  *keycode = 0x2D; return true;
        case '+':  *keycode = 0x2E; return true;
        case '{':  *keycode = 0x2F; return true;
        case '}':  *keycode = 0x30; return true;
        case '|':  *keycode = 0x31; return true;
        case ':':  *keycode = 0x33; return true;
        case '"':  *keycode = 0x34; return true;
        case '~':  *keycode = 0x35; return true;
        case '<':  *keycode = 0x36; return true;
        case '>':  *keycode = 0x37; return true;
        case '?':  *keycode = 0x38; return true;
    }

    return false;
}

/* ============================================================
 *  GATT Characteristic callbacks
 * ============================================================ */
static int hid_report_map_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map));
}

static int hid_info_cb(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info));
}

static int hid_protocol_mode_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &hid_protocol_mode, 1);
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) >= 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &hid_protocol_mode, 1, NULL);
            ESP_LOGI(TAG, "Protocol mode set to: %d", hid_protocol_mode);
        }
    }
    return 0;
}

static int hid_input_report_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t empty[8] = {0};
    return os_mbuf_append(ctxt->om, empty, 8);
}

/* Input Report Reference: [Report ID=1][Report Type=1=Input] */
static int hid_report_ref_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t report_ref[2] = {0x01, 0x01};
    return os_mbuf_append(ctxt->om, report_ref, sizeof(report_ref));
}

/* ⭐ Output Report: Host gửi LED state về device */
static int hid_output_report_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static uint8_t led_state = 0;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= 1) {
            ble_hs_mbuf_to_flat(ctxt->om, &led_state, 1, NULL);
            ESP_LOGI(TAG, "💡 LED state from host: 0x%02X (Num=%d Caps=%d Scroll=%d)",
                     led_state,
                     (led_state & 0x01) ? 1 : 0,
                     (led_state & 0x02) ? 1 : 0,
                     (led_state & 0x04) ? 1 : 0);

            if (s_leds_cb) {
                s_leds_cb(led_state);
            }
        }
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &led_state, 1);
    }
    return 0;
}

/* ⭐ Output Report Reference: [Report ID=1][Report Type=2=Output] */
static int hid_output_report_ref_cb(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t report_ref[2] = {0x01, 0x02};
    return os_mbuf_append(ctxt->om, report_ref, sizeof(report_ref));
}

/* ============================================================
 *  GATT Services
 * ============================================================ */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_HID_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(GATT_HID_REPORT_MAP_UUID),
                .access_cb = hid_report_map_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(GATT_HID_INFO_UUID),
                .access_cb = hid_info_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(GATT_HID_PROTOCOL_MODE_UUID),
                .access_cb = hid_protocol_mode_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            /* INPUT REPORT (keys → host) */
            {
                .uuid = BLE_UUID16_DECLARE(GATT_HID_REPORT_UUID),
                .access_cb = hid_input_report_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &s_hid_input_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(GATT_HID_REPORT_REF_UUID),
                        .access_cb = hid_report_ref_cb,
                        .att_flags = BLE_ATT_F_READ,
                    },
                    { 0 }
                }
            },
            /* ⭐ OUTPUT REPORT (LED state host → device) */
            {
                .uuid = BLE_UUID16_DECLARE(GATT_HID_REPORT_UUID),
                .access_cb = hid_output_report_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_hid_output_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(GATT_HID_REPORT_REF_UUID),
                        .access_cb = hid_output_report_ref_cb,
                        .att_flags = BLE_ATT_F_READ,
                    },
                    { 0 }
                }
            },
            { 0 }
        }
    },
    { 0 }
};

/* ============================================================
 *  Advertising
 * ============================================================ */
static void ble_hid_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;
    fields.appearance = 0x03C1;
    fields.appearance_is_present = 1;
    fields.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(GATT_HID_SVC_UUID)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "❌ adv_set_fields: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "❌ adv_start: %d", rc);
    } else {
        ESP_LOGI(TAG, "📡 Advertising as '%s'", s_device_name);
    }
}

/* ============================================================
 *  GAP event handler
 * ============================================================ */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "🔗 Connection %s (status=%d, handle=%d)",
                 event->connect.status == 0 ? "✅ OK" : "❌ FAIL",
                 event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_is_connected = true;

            struct ble_gap_upd_params params = {
                .itvl_min = 12,
                .itvl_max = 24,
                .latency = 10,
                .supervision_timeout = 400,
            };
            ble_gap_update_params(s_conn_handle, &params);

            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0) {
                ESP_LOGW(TAG, "⚠️ security_initiate: %d", rc);
            }

            if (s_conn_cb) s_conn_cb(true);
        } else {
            ble_hid_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "🔌 Disconnected (reason=%d / 0x%X)",
                 event->disconnect.reason, event->disconnect.reason);
        s_is_connected = false;
        s_conn_handle = 0xFFFF;
        if (s_conn_cb) s_conn_cb(false);
        ble_hid_advertise();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "🔐 Encryption changed (status=%d)", event->enc_change.status);
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "✅ Encryption ENABLED - HID ready!");
        }
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "🔑 Passkey action: %d", event->passkey.params.action);
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            struct ble_sm_io pkey = {0};
            pkey.action = BLE_SM_IOACT_NUMCMP;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "📡 Subscribe: attr=%d, notify=%d, indicate=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "📏 MTU updated: %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ESP_LOGW(TAG, "🔄 Repeat pairing - clearing old bond");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

/* ============================================================
 *  On sync / reset
 * ============================================================ */
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "❌ ensure_addr: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "❌ infer_auto: %d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    ESP_LOGI(TAG, "📱 BLE Address: %02X:%02X:%02X:%02X:%02X:%02X",
             addr_val[5], addr_val[4], addr_val[3],
             addr_val[2], addr_val[1], addr_val[0]);

    ble_hid_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "⚠️ BLE reset: %d", reason);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */
esp_err_t ble_hid_init(const ble_hid_config_t *config)
{
    if (config && config->device_name) {
        s_device_name = config->device_name;
    }
    if (config && config->conn_cb) {
        s_conn_cb = config->conn_cb;
    }
    /* ⭐ Lưu callback LED */
    if (config && config->leds_cb) {
        s_leds_cb = config->leds_cb;
    }

    /* Init NVS (bắt buộc cho bonding) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    /* Init NimBLE */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ nimble_port_init: %d", ret);
        return ret;
    }

    /* Security configuration - Just Works pairing */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Register services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "❌ gatts_count_cfg: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "❌ gatts_add_svcs: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(s_device_name);
    ble_svc_gap_device_appearance_set(0x03C1);

    /* Load bonding storage */
    ble_store_config_init();

    /* Start BLE host task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "✅ BLE HID initialized - '%s'", s_device_name);
    return ESP_OK;
}

bool ble_hid_is_connected(void)
{
    return s_is_connected;
}

esp_err_t ble_hid_send_report(uint8_t modifier, const uint8_t keys[6])
{
    if (!s_is_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[8] = {
        modifier, 0,
        keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]
    };

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_hid_input_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "⚠️ notify failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_hid_release_all(void)
{
    uint8_t zeros[6] = {0};
    return ble_hid_send_report(0, zeros);
}

esp_err_t ble_hid_send_key(uint8_t modifier, uint8_t keycode)
{
    if (!s_is_connected) {
        ESP_LOGW(TAG, "⚠️ Chưa connect");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
    esp_err_t ret = ble_hid_send_report(modifier, keys);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));
    return ble_hid_release_all();
}

esp_err_t ble_hid_send_string(const char *text)
{
    if (!s_is_connected) {
        ESP_LOGW(TAG, "⚠️ Chưa connect");
        return ESP_ERR_INVALID_STATE;
    }
    if (!text) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "⌨️ Gửi chuỗi: \"%s\"", text);

    for (const char *p = text; *p != '\0'; p++) {
        uint8_t keycode = 0;
        uint8_t modifier = 0;

        if (!ascii_to_hid(*p, &keycode, &modifier)) {
            ESP_LOGW(TAG, "⚠️ Không map được ký tự: '%c' (0x%02X), bỏ qua", *p, *p);
            continue;
        }

        uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
        esp_err_t ret = ble_hid_send_report(modifier, keys);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ send_report failed tại '%c'", *p);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        ble_hid_release_all();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}
