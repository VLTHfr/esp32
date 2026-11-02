#ifndef LED_RGB_H
#define LED_RGB_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  LED RGB Module - Status indicator cho USB-BLE Bridge
 *  Dùng WS2812 LED trên ESP32-S3-DevKitC-1 (GPIO 48 mặc định)
 * ============================================================ */

/**
 * @brief Các trạng thái hiển thị của LED
 */
typedef enum {
    LED_STATE_BOOT,              // 🔵 Xanh dương nhạt - khởi động
    LED_STATE_WAIT_USB_AND_BLE,  // 🟠 Cam - chờ cả USB + BLE
    LED_STATE_WAIT_USB,          // 🟡 Vàng - có BLE, chờ USB
    LED_STATE_WAIT_BLE,          // 🔴 Đỏ - có USB, chờ BLE
    LED_STATE_READY,             // 🟢 Xanh lá - sẵn sàng bridge
    LED_STATE_ERROR,             // 🔴 Đỏ nháy - lỗi
} led_state_t;

/**
 * @brief Cấu hình khởi tạo LED RGB
 */
typedef struct {
    int gpio_num;       // GPIO chân LED (mặc định 48 cho DevKitC-1)
    uint8_t brightness; // Độ sáng 0-255 (khuyến nghị 50-100)
} led_rgb_config_t;

/**
 * @brief Khởi tạo LED RGB module (RMT driver + task)
 * 
 * @param config  Cấu hình GPIO + brightness
 * @return ESP_OK nếu thành công
 */
esp_err_t led_rgb_init(const led_rgb_config_t *config);

/**
 * @brief Set trạng thái LED (LED task tự update màu)
 * 
 * @param state  Trạng thái mới (BOOT/WAIT_USB/WAIT_BLE/READY/...)
 */
void led_rgb_set_state(led_state_t state);

/**
 * @brief Trigger 1 nháy xanh ngắn (~50ms) khi có keystroke
 *        Sau khi nháy xong tự về trạng thái hiện tại
 */
void led_rgb_blink_key(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_RGB_H */
