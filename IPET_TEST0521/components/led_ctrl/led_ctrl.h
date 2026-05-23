#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 2 颗串联 WS2812 LED（默认 GPIO48）。重复调用是安全的。
esp_err_t led_ctrl_init(gpio_num_t data_gpio);

// 旧接口保留：不再启动闪烁 demo，只做初始化，避免抢占第一颗状态灯。
void led_ctrl_demo_start(void);

// 在关机前触发第二颗灯的淡出效果（阻塞直到完成）
void led_ctrl_shutdown_fade(void);

// 设置单颗 LED 的颜色（RGB，0-based index），会立即更新整条链
void led_ctrl_set_led_color(int idx, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
