#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ============================================================
   GPIO事件回调函数类型
   ============================================================ */
typedef void (*gpio_irq_cb_t)(gpio_num_t gpio_num, uint32_t level);

/* ============================================================
   GPIO事件队列
   ============================================================ */
extern QueueHandle_t gpio_evt_queue;

/* ============================================================
   GPIO驱动初始化
   ============================================================ */
esp_err_t gpio_drv_init(void);

/* ============================================================
   GPIO输出控制接口
   ============================================================ */
void gpio_drv_set_level(gpio_num_t gpio_num, uint32_t level);

/* ============================================================
   注册GPIO事件回调函数
   ============================================================ */
esp_err_t gpio_drv_register_callback(gpio_num_t gpio, gpio_irq_cb_t cb);

/* ============================================================
   简单 LED (PWM) 控制接口（基于 LEDC）
   gpio_drv_led_start  : 开始控制指定 GPIO 的 LED（可选择常亮或闪烁）
       - gpio: LED 所在 GPIO
       - duty_percent: 0-100 亮度（占空比百分比）
       - blink_ms: 0 表示常亮；>0 表示以该毫秒为周期进行开/关闪烁
   gpio_drv_led_set_brightness: 动态调整亮度（0-100）
   gpio_drv_led_set_blink_interval: 动态调整闪烁间隔（毫秒，0 表示常亮）
   gpio_drv_led_stop   : 停止并释放该 GPIO 的 LED 控制
*/
esp_err_t gpio_drv_led_start(gpio_num_t gpio, uint8_t duty_percent, uint32_t blink_ms, bool active_low);
esp_err_t gpio_drv_led_set_brightness(gpio_num_t gpio, uint8_t duty_percent);
esp_err_t gpio_drv_led_set_blink_interval(gpio_num_t gpio, uint32_t blink_ms);
esp_err_t gpio_drv_led_stop(gpio_num_t gpio);