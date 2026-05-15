#ifndef HWINIT_H
#define HWINIT_H


// 仅保留“对外接口”所需的最小依赖，避免把大量 ESP-IDF / 其他组件头文件
// 通过 hwinit.h 传染到整个工程，从而造成编译变慢与组件循环依赖。
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"



/*



#include "sc7a20.h"
*/


/*------------------蜂鸣器------------------*/
#define BUZZ_GPIO ((gpio_num_t)CONFIG_BUZZER_GPIO)
#define BUZZ_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZ_LEDC_TIMER LEDC_TIMER_0
#define BUZZ_LEDC_MODE LEDC_LOW_SPEED_MODE


/*------------------按键------------------*/
#define BUTTON_1_GPIO ((gpio_num_t)CONFIG_BUTTON_1_GPIO)
#define BUTTON_2_GPIO ((gpio_num_t)CONFIG_BUTTON_2_GPIO)

/*------------------I2C总线------------------*/


/*------------------SC7A20------------------*/
#define SC7A20HTR_INT1_PIN ((gpio_num_t)CONFIG_SC7A20_INT1_GPIO)
#define SC7A20HTR_INT2_PIN ((gpio_num_t)CONFIG_SC7A20_INT2_GPIO)

/*------------------WS2812B------------------*/
#define WS2812B_GPIO ((gpio_num_t)CONFIG_WS2812B_GPIO)


/*------------------触摸屏------------------*/
#define TP_CS_GPIO  ((gpio_num_t)CONFIG_TP_CS_GPIO)
#define TP_IRQ_GPIO ((gpio_num_t)CONFIG_TP_IRQ_GPIO)

/*------------------I2S------------------*/
#define I2S_WS_GPIO  ((gpio_num_t)CONFIG_I2S_WS_GPIO)
#define I2S_SD_GPIO  ((gpio_num_t)CONFIG_I2S_SD_GPIO)
#define I2S_SCK_GPIO ((gpio_num_t)CONFIG_I2S_SCK_GPIO)






void hw_init(void);
void check_stack(void);

/**
 * 简单的阻塞式 Wi‑Fi STA 连接函数（会等待连接或超时）。
 * 请把你的 SSID/密码 写入调用处，然后重新编译刷机。
 * timeout_ms: 等待连接的超时时间（毫秒），建议 10000~30000
 */
void wifi_connect_sta(const char *ssid, const char *pass, int timeout_ms);

#endif// HWINIT_H