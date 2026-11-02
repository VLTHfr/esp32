#include "usb_hid.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

static const char *TAG = "USB_HID";

/* ============================================================
 *  Module state
 * ============================================================ */
static usb_hid_report_cb_t s_report_cb = NULL;
static usb_hid_char_cb_t   s_char_cb = NULL;
static usb_hid_conn_cb_t   s_conn_cb = NULL;
static bool                s_is_connected = false;
static hid_host_device_handle_t s_keyboard_handle = NULL;   // ⭐ Handle để set LED

/* ============================================================
 *  HID Keycode → ASCII (Boot Protocol, US layout)
 *  Index: 0x00 → 0x67 = 104 entries
 * ============================================================ */
static const char hid_keycode_to_ascii_table[2][0x68] = {
    // [0] = không Shift
    {
        /* 0x00 */ 0, 0, 0, 0,
        /* 0x04 */ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
        /* 0x10 */ 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
        /* 0x1E */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        /* 0x28 */ '\n', 0, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
        /* 0x39 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x46 */ 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x4F */ 0, 0, 0, 0,
        /* 0x53 */ 0, '/', '*', '-', '+', '\n',
        /* 0x59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
        /* 0x64 */ 0, 0, 0, 0
    },
    // [1] = có Shift
    {
        /* 0x00 */ 0, 0, 0, 0,
        /* 0x04 */ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
        /* 0x10 */ 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        /* 0x1E */ '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
        /* 0x28 */ '\n', 0, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
        /* 0x39 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x46 */ 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x4F */ 0, 0, 0, 0,
        /* 0x53 */ 0, '/', '*', '-', '+', '\n',
        /* 0x59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
        /* 0x64 */ 0, 0, 0, 0
    }
};

/* ============================================================
 *  ENUM FILTER CALLBACK - Force enumerate composite devices
 * ============================================================ */
static bool enum_filter_cb(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    ESP_LOGI(TAG, "🔍 Device detected: VID=0x%04X, PID=0x%04X, class=0x%02X",
             dev_desc->idVendor, dev_desc->idProduct, dev_desc->bDeviceClass);
    *bConfigurationValue = 1;
    return true;
}

/* ============================================================
 *  USB Host library task
 * ============================================================ */
static void usb_lib_task(void *arg)
{
    ESP_LOGI(TAG, "USB Lib task started on core %d", xPortGetCoreID());
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "⚠️ USB: No clients");
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGW(TAG, "⚠️ USB: All devices freed");
        }
    }
}

/* ============================================================
 *  Parse HID keyboard report
 * ============================================================ */
static void parse_keyboard_report(const uint8_t *data, int length)
{
    if (length < 8) {
        ESP_LOGW(TAG, "HID report quá ngắn: %d bytes", length);
        return;
    }

    uint8_t modifier = data[0];
    uint8_t keys[6] = {data[2], data[3], data[4], data[5], data[6], data[7]};

    if (s_report_cb) {
        s_report_cb(modifier, keys);
    }

    if (s_char_cb) {
        bool shift_pressed = (modifier & 0x22) != 0;
        for (int i = 0; i < 6; i++) {
            uint8_t kc = keys[i];
            if (kc == 0) continue;
            if (kc < 0x04 || kc > 0x67) continue;

            char ch = hid_keycode_to_ascii_table[shift_pressed ? 1 : 0][kc];
            if (ch != 0) {
                s_char_cb(ch, kc);
            }
        }
    }
}

/* ============================================================
 *  Interface callback
 * ============================================================ */
static void hid_interface_cb(hid_host_device_handle_t hid_device_handle,
                              const hid_host_interface_event_t event,
                              void *arg)
{
    uint8_t data[64] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(
            hid_device_handle, data, sizeof(data), &data_length));

        if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
            parse_keyboard_report(data, data_length);
        }
        break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "🔌 HID Device DISCONNECTED");
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        s_is_connected = false;
        s_keyboard_handle = NULL;      // ⭐ Reset handle
        if (s_conn_cb) s_conn_cb(false);
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "⚠️ HID Transfer error");
        break;

    default:
        ESP_LOGW(TAG, "HID unhandled event: %d", event);
        break;
    }
}

/* ============================================================
 *  Device event
 * ============================================================ */
static void hid_device_event(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event,
                              void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        const char *proto_str =
            (dev_params.proto == HID_PROTOCOL_KEYBOARD) ? "KEYBOARD" :
            (dev_params.proto == HID_PROTOCOL_MOUSE) ? "MOUSE" : "OTHER";

        ESP_LOGI(TAG, "🎹 HID CONNECTED - proto: %s, sub_class: %d",
                 proto_str, dev_params.sub_class);

        const hid_host_device_config_t dev_config = {
            .callback = hid_interface_cb,
            .callback_arg = NULL
        };

        esp_err_t ret = hid_host_device_open(hid_device_handle, &dev_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ hid_host_device_open: 0x%x", ret);
            return;
        }

        if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "⚠️ SET_PROTOCOL failed (0x%x) - bỏ qua", ret);
            }

            if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
                ret = hid_class_request_set_idle(hid_device_handle, 0, 0);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "⚠️ SET_IDLE failed (0x%x) - bỏ qua", ret);
                }
            }
        }

        ret = hid_host_device_start(hid_device_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ hid_host_device_start: 0x%x", ret);
        } else {
            ESP_LOGI(TAG, "🚀 HID device started");
            s_is_connected = true;

            /* ⭐ Lưu handle keyboard để gửi Output Report (LED) */
            if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
                s_keyboard_handle = hid_device_handle;
            }

            if (s_conn_cb) s_conn_cb(true);
        }
    }
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */
esp_err_t usb_hid_init(const usb_hid_config_t *config)
{
    if (config) {
        s_report_cb = config->report_cb;
        s_char_cb   = config->char_cb;
        s_conn_cb   = config->conn_cb;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = enum_filter_cb,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ usb_host_install: 0x%x", ret);
        return ret;
    }
    ESP_LOGI(TAG, "✅ USB Host installed (with enum filter)");

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_events", 4096, NULL, 2, NULL, 0);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "❌ xTaskCreate usb_events failed");
        return ESP_FAIL;
    }

    const hid_host_driver_config_t hid_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 8192,
        .core_id = 0,
        .callback = hid_device_event,
        .callback_arg = NULL
    };
    ret = hid_host_install(&hid_driver_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ hid_host_install: 0x%x", ret);
        return ret;
    }
    ESP_LOGI(TAG, "✅ HID Host driver installed");

    ESP_LOGI(TAG, "🔌 Ready! Plug in a USB keyboard via OTG...");
    return ESP_OK;
}

bool usb_hid_is_connected(void)
{
    return s_is_connected;
}

char usb_hid_keycode_to_ascii(uint8_t keycode, bool shift_pressed)
{
    if (keycode < 0x04 || keycode > 0x67) return 0;
    return hid_keycode_to_ascii_table[shift_pressed ? 1 : 0][keycode];
}

/* ⭐ Gửi LED state xuống bàn phím USB */
esp_err_t usb_hid_set_leds(uint8_t leds)
{
    if (!s_keyboard_handle) {
        ESP_LOGW(TAG, "⚠️ Chưa có keyboard, không set LED được");
        return ESP_ERR_INVALID_STATE;
    }

    /* HID Class Request: SET_REPORT (Output, Report ID = 0) */
    esp_err_t ret = hid_class_request_set_report(
        s_keyboard_handle,
        HID_REPORT_TYPE_OUTPUT,
        0,              // Report ID = 0 (Boot Protocol)
        &leds,
        1               // 1 byte
    );

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ set_leds failed: 0x%x", ret);
    } else {
        ESP_LOGI(TAG, "💡 LED set: 0x%02X (Num=%d Caps=%d Scroll=%d)",
                 leds,
                 (leds & 0x01) ? 1 : 0,
                 (leds & 0x02) ? 1 : 0,
                 (leds & 0x04) ? 1 : 0);
    }
    return ret;
}
