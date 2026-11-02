#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  BLE HID Module - Public API
 *  ESP32-S3 làm BLE Keyboard Peripheral
 * ============================================================ */

/**
 * @brief Callback khi trạng thái BLE thay đổi
 */
typedef void (*ble_hid_conn_cb_t)(bool connected);

/**
 * @brief Callback khi host gửi Output Report (LED state)
 * @param leds  bit0=NumLock, bit1=CapsLock, bit2=ScrollLock
 */
typedef void (*ble_hid_leds_cb_t)(uint8_t leds);

/**
 * @brief Cấu hình khởi tạo BLE HID
 */
typedef struct {
    const char *device_name;
    ble_hid_conn_cb_t conn_cb;
    ble_hid_leds_cb_t leds_cb;   // ⭐ Callback khi host gửi LED state
} ble_hid_config_t;

esp_err_t ble_hid_init(const ble_hid_config_t *config);
bool ble_hid_is_connected(void);
esp_err_t ble_hid_send_key(uint8_t modifier, uint8_t keycode);
esp_err_t ble_hid_send_report(uint8_t modifier, const uint8_t keys[6]);
esp_err_t ble_hid_send_string(const char *text);
esp_err_t ble_hid_release_all(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_HID_H */
