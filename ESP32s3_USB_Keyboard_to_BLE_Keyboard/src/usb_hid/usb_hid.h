#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback khi có HID keyboard report mới
 */
typedef void (*usb_hid_report_cb_t)(uint8_t modifier, const uint8_t keys[6]);

/**
 * @brief Callback khi có ký tự ASCII
 */
typedef void (*usb_hid_char_cb_t)(char ch, uint8_t keycode);

/**
 * @brief Callback khi kết nối thay đổi
 */
typedef void (*usb_hid_conn_cb_t)(bool connected);

typedef struct {
    usb_hid_report_cb_t  report_cb;
    usb_hid_char_cb_t    char_cb;
    usb_hid_conn_cb_t    conn_cb;
} usb_hid_config_t;

esp_err_t usb_hid_init(const usb_hid_config_t *config);
bool usb_hid_is_connected(void);
char usb_hid_keycode_to_ascii(uint8_t keycode, bool shift_pressed);

/**
 * @brief ⭐ Gửi LED state xuống bàn phím USB
 * @param leds  bit0=NumLock, bit1=CapsLock, bit2=ScrollLock
 */
esp_err_t usb_hid_set_leds(uint8_t leds);

#ifdef __cplusplus
}
#endif

#endif /* USB_HID_H */
