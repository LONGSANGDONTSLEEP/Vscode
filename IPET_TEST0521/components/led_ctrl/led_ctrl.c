#include "led_ctrl.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "led_ctrl";

// 默认数据引脚
static gpio_num_t s_data_gpio = GPIO_NUM_48;

// LED 数量（你的模块是2颗串联）
#define LED_COUNT 2

// 初始化标志
static bool s_inited = false;

// RMT 配置
static const rmt_channel_t RMT_CHANNEL = RMT_CHANNEL_0;
static const uint8_t RMT_CLK_DIV = 2; // 分频

// LED 缓冲（GRB per LED）
static uint8_t s_led_data[LED_COUNT * 3];

esp_err_t led_ctrl_init(gpio_num_t data_gpio)
{
    s_data_gpio = data_gpio;

    rmt_config_t rmt_cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)s_data_gpio, RMT_CHANNEL);
    rmt_cfg.clk_div = RMT_CLK_DIV;
    ESP_ERROR_CHECK(rmt_config(&rmt_cfg));
    ESP_ERROR_CHECK(rmt_driver_install(rmt_cfg.channel, 0, 0));

    // 初始化数据为 0
    memset(s_led_data, 0, sizeof(s_led_data));

    s_inited = true;

    ESP_LOGI(TAG, "WS2812 (RMT) initialized on GPIO%d", data_gpio);
    return ESP_OK;
}

static void rainbow_step(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // pos: 0..255
    if (pos < 85) {
        *r = pos * 3;
        *g = 255 - pos * 3;
        *b = 0;
    } else if (pos < 170) {
        pos -= 85;
        *r = 255 - pos * 3;
        *g = 0;
        *b = pos * 3;
    } else {
        pos -= 170;
        *r = 0;
        *g = pos * 3;
        *b = 255 - pos * 3;
    }
}

// 将 s_led_data (GRB per LED) 转换为 RMT items 并发送
static void ws2812_send(uint8_t *data, int led_num)
{
    // RMT tick = 1 / (80MHz / clk_div) = clk_div / 80MHz
    // 使用近似值（clk_div = 2），tick = 25ns
    // 定义时序（tick 单位）
    const uint32_t T0H = 16; // 0.4us
    const uint32_t T0L = 34; // 0.85us
    const uint32_t T1H = 32; // 0.8us
    const uint32_t T1L = 18; // 0.45us

    int items_len = led_num * 24;
    rmt_item32_t *items = (rmt_item32_t *)malloc(sizeof(rmt_item32_t) * items_len);
    if (!items) return; // 内存分配失败时直接返回（没有发送）
    int idx = 0;

    for (int i = 0; i < led_num; ++i) {
        // WS2812 接受 GRB 顺序
        uint8_t g = data[i * 3 + 0];
        uint8_t r = data[i * 3 + 1];
        uint8_t b = data[i * 3 + 2];
        uint8_t bytes[3] = {g, r, b};
 
        for (int byte = 0; byte < 3; ++byte) {
            for (int bit = 7; bit >= 0; --bit) {
                bool bit_is_one = (bytes[byte] >> bit) & 0x1;
                if (bit_is_one) {
                    items[idx].level0 = 1;
                    items[idx].duration0 = T1H;
                    items[idx].level1 = 0;
                    items[idx].duration1 = T1L;
                } else {
                    items[idx].level0 = 1;
                    items[idx].duration0 = T0H;
                    items[idx].level1 = 0;
                    items[idx].duration1 = T0L;
                }
                idx++;
            }
        }
    }

    // 发送
    rmt_write_items(RMT_CHANNEL, items, items_len, true);
    // Reset code: >50us low
    vTaskDelay(pdMS_TO_TICKS(1));
    free(items);
}

static void led_blink_task(void *arg)
{
    bool on = false;
    const TickType_t delay = pdMS_TO_TICKS(500); // 500ms 闪烁周期
    while (1) {
        if (on) {
            // 打开第一颗 LED，使用中等亮度的蓝色（可按需修改）
            s_led_data[0] = 0;  // G
            s_led_data[1] = 100;   // R
            s_led_data[2] = 0; // B
        } else {
            // 关闭第一颗 LED
            s_led_data[0] = 0;
            s_led_data[1] = 0;
            s_led_data[2] = 0;
        }
        // 第二颗 LED 保留给按键逻辑控制，不在此处修改
        ws2812_send(s_led_data, LED_COUNT);
        on = !on;
        vTaskDelay(delay);
    }
}

void led_ctrl_demo_start(void)
{
    if (!s_inited) {
        if (led_ctrl_init(s_data_gpio) != ESP_OK) return;
    }

    ESP_LOGI(TAG, "LED demo started on GPIO%d", s_data_gpio);
    xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 5, NULL);
}

void led_ctrl_shutdown_fade(void)
{
    if (!s_inited) return;
    // 将第2颗从 50% 白色淡出到 0（如果当前是其它颜色，需要额外逻辑）
    int steps = 50;
    for (int i = steps; i >= 0; --i) {
        uint8_t val = (uint8_t)((i * 255) / steps);
        // 第一颗保持原样（此处保持关闭），第二颗设置为灰阶
        s_led_data[0] = 0; s_led_data[1] = 0; s_led_data[2] = 0;
        s_led_data[3] = val; s_led_data[4] = val; s_led_data[5] = val;
        ws2812_send(s_led_data, LED_COUNT);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void led_ctrl_set_led_color(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_inited) {
        // 如果还没初始化，尝试自动初始化（使用默认 data GPIO）
        if (led_ctrl_init(s_data_gpio) != ESP_OK) return;
    }
    if (idx < 0 || idx >= LED_COUNT) return;
    // s_led_data 为 GRB
    s_led_data[idx * 3 + 0] = g;
    s_led_data[idx * 3 + 1] = r;
    s_led_data[idx * 3 + 2] = b;
    ws2812_send(s_led_data, LED_COUNT);
}
