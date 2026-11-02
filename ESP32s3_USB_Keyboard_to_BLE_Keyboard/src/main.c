#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "usb_hid/usb_hid.h"
#include "ble_hid/ble_hid.h"
#include "led_rgb/led_rgb.h"

static const char *TAG = "MAIN";

/* ============================================================
 *  Thống kê
 * ============================================================ */
static uint32_t s_total_reports = 0;
static uint32_t s_forwarded_to_ble = 0;
static uint32_t s_dropped_no_ble = 0;

/* ============================================================
 *  Helper: cập nhật LED RGB theo trạng thái USB + BLE
 * ============================================================ */
static void update_led_state(void)
{
    bool usb_ok = usb_hid_is_connected();
    bool ble_ok = ble_hid_is_connected();

    if (usb_ok && ble_ok) {
        led_rgb_set_state(LED_STATE_READY);              // 🟢 Xanh lá
    } else if (!usb_ok && !ble_ok) {
        led_rgb_set_state(LED_STATE_WAIT_USB_AND_BLE);   // 🟠 Cam
    } else if (!usb_ok) {
        led_rgb_set_state(LED_STATE_WAIT_USB);           // 🟡 Vàng
    } else {
        led_rgb_set_state(LED_STATE_WAIT_BLE);           // 🔴 Đỏ
    }
}

/* ============================================================
 *  USB HID callbacks
 * ============================================================ */
static void on_usb_report(uint8_t modifier, const uint8_t keys[6])
{
    s_total_reports++;

    /* Check có phím nào đang nhấn không (ignore all-zero release report) */
    bool has_key = (modifier != 0);
    for (int i = 0; i < 6 && !has_key; i++) {
        if (keys[i] != 0) has_key = true;
    }

    if (ble_hid_is_connected()) {
        esp_err_t ret = ble_hid_send_report(modifier, keys);
        if (ret == ESP_OK) {
            s_forwarded_to_ble++;
            if (has_key) {
                led_rgb_blink_key();
            }
        } else {
            ESP_LOGW(TAG, "⚠️ BLE send failed: 0x%x", ret);
        }
    } else {
        s_dropped_no_ble++;
    }
}

static void on_usb_char(char ch, uint8_t keycode)
{
    if (ch == '\n')      ESP_LOGI(TAG, "🎹 Key: [ENTER]");
    else if (ch == '\b') ESP_LOGI(TAG, "🎹 Key: [BACKSPACE]");
    else if (ch == '\t') ESP_LOGI(TAG, "🎹 Key: [TAB]");
    else if (ch == ' ')  ESP_LOGI(TAG, "🎹 Key: [SPACE]");
    else                 ESP_LOGI(TAG, "🎹 Key: '%c' (0x%02X)", ch, keycode);
}

static void on_usb_connection(bool connected)
{
    if (connected) {
        ESP_LOGI(TAG, "🎉 USB Keyboard CONNECTED");
    } else {
        ESP_LOGW(TAG, "🔌 USB Keyboard DISCONNECTED");
    }
    update_led_state();
}

/* ============================================================
 *  BLE HID callbacks
 * ============================================================ */
static void on_ble_connection(bool connected)
{
    if (connected) {
        ESP_LOGI(TAG, "🎉 BLE Host CONNECTED");
    } else {
        ESP_LOGW(TAG, "🔌 BLE Host DISCONNECTED");
    }
    update_led_state();
}

/* ⭐ Callback khi host (điện thoại) gửi LED state qua BLE
 * → Forward xuống bàn phím USB để đèn sáng */
static void on_ble_leds(uint8_t leds)
{
    ESP_LOGI(TAG, "💡 Forward LED state to USB keyboard: 0x%02X", leds);

    if (usb_hid_is_connected()) {
        esp_err_t ret = usb_hid_set_leds(leds);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "⚠️ Forward LED failed: 0x%x", ret);
        }
    } else {
        ESP_LOGW(TAG, "⚠️ USB chưa connect, không forward được LED");
    }
}

/* ============================================================
 *  Status monitoring task
 * ============================================================ */
static void status_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        const char *usb_status = usb_hid_is_connected() ? "✅" : "❌";
        const char *ble_status = ble_hid_is_connected() ? "✅" : "❌";

        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        ESP_LOGI(TAG, "📊 STATUS | USB: %s | BLE: %s | heap: %" PRIu32,
                 usb_status, ble_status, esp_get_free_heap_size());
        ESP_LOGI(TAG, "📈 Reports: %" PRIu32 " total | %" PRIu32 " → BLE | %" PRIu32 " dropped",
                 s_total_reports, s_forwarded_to_ble, s_dropped_no_ble);
        ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    }
}

/* ============================================================
 *  Main entry point
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ESP32-S3 USB → BLE Keyboard Bridge  ║");
    ESP_LOGI(TAG, "║  + LED CapsLock/NumLock support      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");

    /* 1️⃣ LED RGB trước tiên */
    const led_rgb_config_t led_cfg = {
        .gpio_num = 48,
        .brightness = 10,
    };
    ESP_ERROR_CHECK(led_rgb_init(&led_cfg));
    led_rgb_set_state(LED_STATE_BOOT);

    /* 2️⃣ BLE HID - với LED callback */
    ESP_LOGI(TAG, "🔵 Initializing BLE HID...");
    const ble_hid_config_t ble_cfg = {
        .device_name = "ESP32-S3 Keyboard",
        .conn_cb = on_ble_connection,
        .leds_cb = on_ble_leds,          // ⭐ Callback nhận LED state từ host
    };
    ESP_ERROR_CHECK(ble_hid_init(&ble_cfg));

    /* 3️⃣ USB HID */
    ESP_LOGI(TAG, "🟢 Initializing USB HID...");
    const usb_hid_config_t usb_cfg = {
        .report_cb = on_usb_report,
        .char_cb   = on_usb_char,
        .conn_cb   = on_usb_connection,
    };
    ESP_ERROR_CHECK(usb_hid_init(&usb_cfg));

    /* 4️⃣ Set LED về trạng thái chờ */
    update_led_state();

    /* 5️⃣ Status monitor */
    xTaskCreate(status_task, "status", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "🚀 System READY!");
    ESP_LOGI(TAG, "   💡 Bấm CapsLock/NumLock trên bàn phím → đèn sẽ sáng!");
}
