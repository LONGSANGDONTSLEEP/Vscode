#include "led_ctrl.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/rmt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "led_ctrl";

#define LED_COUNT 2

static gpio_num_t s_data_gpio = GPIO_NUM_48;
static const rmt_channel_t RMT_CHANNEL = RMT_CHANNEL_0;
static const uint8_t RMT_CLK_DIV = 2;

static bool s_inited;
static SemaphoreHandle_t s_lock;
/* LED 缓冲：GRB per LED */
static uint8_t s_led_data[LED_COUNT * 3];

static void ws2812_send_locked(uint8_t *data, int led_num)
{
    const uint32_t T0H = 16;
    const uint32_t T0L = 34;
    const uint32_t T1H = 32;
    const uint32_t T1L = 18;

    int items_len = led_num * 24;
    rmt_item32_t *items = (rmt_item32_t *)malloc(sizeof(rmt_item32_t) * items_len);
    if (!items) {
        return;
    }

    int idx = 0;
    for (int i = 0; i < led_num; ++i) {
        uint8_t g = data[i * 3 + 0];
        uint8_t r = data[i * 3 + 1];
        uint8_t b = data[i * 3 + 2];
        uint8_t bytes[3] = {g, r, b};

        for (int byte = 0; byte < 3; ++byte) {
            for (int bit = 7; bit >= 0; --bit) {
                bool one = (bytes[byte] >> bit) & 0x1;
                items[idx].level0 = 1;
                items[idx].duration0 = one ? T1H : T0H;
                items[idx].level1 = 0;
                items[idx].duration1 = one ? T1L : T0L;
                idx++;
            }
        }
    }

    rmt_write_items(RMT_CHANNEL, items, items_len, true);
    vTaskDelay(pdMS_TO_TICKS(1));
    free(items);
}

esp_err_t led_ctrl_init(gpio_num_t data_gpio)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_inited) {
        return ESP_OK;
    }

    s_data_gpio = data_gpio;

    rmt_config_t rmt_cfg = RMT_DEFAULT_CONFIG_TX(s_data_gpio, RMT_CHANNEL);
    rmt_cfg.clk_div = RMT_CLK_DIV;

    esp_err_t ret = rmt_config(&rmt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_driver_install(rmt_cfg.channel, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "rmt_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(s_led_data, 0, sizeof(s_led_data));
    s_inited = true;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        ws2812_send_locked(s_led_data, LED_COUNT);
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "WS2812 initialized on GPIO%d", data_gpio);
    return ESP_OK;
}

void led_ctrl_demo_start(void)
{
    /* 兼容旧调用。现在不再启动闪烁任务，否则会覆盖第一颗行为状态灯。 */
    (void)led_ctrl_init(s_data_gpio);
    ESP_LOGI(TAG, "LED demo disabled; controlled by pet_power_led");
}

void led_ctrl_shutdown_fade(void)
{
    if (!s_inited) {
        return;
    }

    for (int i = 50; i >= 0; --i) {
        uint8_t val = (uint8_t)((i * 255) / 50);
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* 第一颗保持当前状态，第二颗红色淡出。 */
            s_led_data[3] = 0;   /* G */
            s_led_data[4] = val; /* R */
            s_led_data[5] = 0;   /* B */
            ws2812_send_locked(s_led_data, LED_COUNT);
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void led_ctrl_set_led_color(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_inited) {
        if (led_ctrl_init(s_data_gpio) != ESP_OK) {
            return;
        }
    }
    if (idx < 0 || idx >= LED_COUNT) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_led_data[idx * 3 + 0] = g;
        s_led_data[idx * 3 + 1] = r;
        s_led_data[idx * 3 + 2] = b;
        ws2812_send_locked(s_led_data, LED_COUNT);
        xSemaphoreGive(s_lock);
    }
}
