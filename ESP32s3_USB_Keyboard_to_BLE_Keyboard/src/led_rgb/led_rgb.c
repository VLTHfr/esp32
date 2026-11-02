#include "led_rgb.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "LED_RGB";

/* ============================================================
 *  Module state
 * ============================================================ */
static led_strip_handle_t s_strip = NULL;
static uint8_t s_brightness = 80;  // 0-255
static volatile led_state_t s_current_state = LED_STATE_BOOT;
static volatile bool s_blink_request = false;
static TaskHandle_t s_led_task_handle = NULL;

/* ============================================================
 *  Bảng màu cho từng trạng thái (R, G, B) trước khi apply brightness
 * ============================================================ */
typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t state_colors[] = {
    [LED_STATE_BOOT]             = {  0,  50, 100},  // Xanh dương nhạt
    [LED_STATE_WAIT_USB_AND_BLE] = {255,  80,   0},  // Cam
    [LED_STATE_WAIT_USB]         = {255, 150,   0},  // Vàng
    [LED_STATE_WAIT_BLE]         = {255,   0,   0},  // Đỏ
    [LED_STATE_READY]            = {  0, 255,   0},  // Xanh lá
    [LED_STATE_ERROR]            = {255,   0,   0},  // Đỏ (nháy trong task)
};

/* ============================================================
 *  Helper: set màu với brightness scaling
 * ============================================================ */
static void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;

    uint8_t rr = (r * s_brightness) / 255;
    uint8_t gg = (g * s_brightness) / 255;
    uint8_t bb = (b * s_brightness) / 255;

    led_strip_set_pixel(s_strip, 0, rr, gg, bb);
    led_strip_refresh(s_strip);
}

static void led_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
}

/* ============================================================
 *  LED task - quản lý hiển thị + xử lý blink
 * ============================================================ */
static void led_task(void *arg)
{
    led_state_t last_state = (led_state_t)-1;
    uint32_t error_blink_tick = 0;

    while (1) {
        /* 1. Xử lý blink key (ưu tiên cao nhất) */
        if (s_blink_request) {
            s_blink_request = false;
            /* Nháy trắng xanh sáng ~50ms */
            led_set_color(100, 255, 100);
            vTaskDelay(pdMS_TO_TICKS(50));
            /* Khôi phục màu theo state */
            last_state = (led_state_t)-1;  // force update
        }

        /* 2. Update màu nếu state đổi */
        led_state_t state = s_current_state;
        if (state != last_state) {
            last_state = state;

            if (state == LED_STATE_ERROR) {
                /* ERROR state dùng blink riêng ở bước 3 */
            } else if (state < sizeof(state_colors) / sizeof(state_colors[0])) {
                rgb_t c = state_colors[state];
                led_set_color(c.r, c.g, c.b);
                ESP_LOGD(TAG, "State %d: RGB(%d,%d,%d)", state, c.r, c.g, c.b);
            }
        }

        /* 3. Nháy cho ERROR state */
        if (state == LED_STATE_ERROR) {
            error_blink_tick++;
            if (error_blink_tick % 2 == 0) {
                led_set_color(255, 0, 0);
            } else {
                led_off();
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));  // check blink request mỗi 20ms
        }
    }
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */
esp_err_t led_rgb_init(const led_rgb_config_t *config)
{
    int gpio = (config && config->gpio_num > 0) ? config->gpio_num : 48;
    s_brightness = (config && config->brightness > 0) ? config->brightness : 80;

    /* ⭐ Cấu hình LED strip (WS2812) - tương thích led_strip v2.x */
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,   // ⭐ Dùng tên cũ (v2.x)
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ led_strip_new_rmt_device: %d", ret);
        return ret;
    }

    led_strip_clear(s_strip);

    /* Tạo task quản lý LED */
    BaseType_t task_ret = xTaskCreate(led_task, "led_task", 2048, NULL, 2, &s_led_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "❌ xTaskCreate led_task failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✅ LED RGB initialized (GPIO %d, brightness %d)", gpio, s_brightness);
    return ESP_OK;
}

void led_rgb_set_state(led_state_t state)
{
    s_current_state = state;
}

void led_rgb_blink_key(void)
{
    s_blink_request = true;
}
