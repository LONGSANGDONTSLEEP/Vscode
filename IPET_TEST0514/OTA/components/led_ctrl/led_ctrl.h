#pragma once
#include "driver/gpio.h"

// 初始化 LED 控制（data GPIO，可选）
esp_err_t led_ctrl_init(gpio_num_t data_gpio);

// 启动演示（默认使用 GPIO48）
void led_ctrl_demo_start(void);

// 在关机前触发第二颗灯的淡出效果（阻塞直到完成）
void led_ctrl_shutdown_fade(void);

// 设置单颗 LED 的颜色（RGB，0-based index），会立即更新整条链
void led_ctrl_set_led_color(int idx, uint8_t r, uint8_t g, uint8_t b);
