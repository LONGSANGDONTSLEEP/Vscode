// pwrkeep.c
// ESP32-S3 单按键开机 + 长按关机
// 重新整理版
//
// 特点：
// 1. 不依赖 GPIO 中断
// 2. 不依赖 callback
// 3. 纯轮询，稳定优先
// 4. 解决“开机时错过按键”问题
// 5. 解决长按误触发
// 6. 适合 PMOS 自保持电路
//
// 人类总喜欢把“一个按键”搞成电源管理、状态机、去抖、长按识别、低功耗控制。
// 然后惊讶于它为什么会坏。真是充满探索精神。

#include "pwrkeep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdint.h>

// 不直接包含 led_ctrl.h（避免组件 include 路径依赖），使用外部声明来调用需要的接口
extern void led_ctrl_shutdown_fade(void);
extern void led_ctrl_set_led_color(int idx, uint8_t r, uint8_t g, uint8_t b);

#define TAG "PWRKEEP"

/* forward declaration for internal helper used below */
static inline void hold_power(bool en);

void pwrkeep_set_hold(bool enable)
{
    hold_power(enable);

    ESP_LOGI(TAG,
             "hold=%d",
             enable);
}


//==============================
// 用户配置
//==============================

// 电源保持 GPIO
#define PWR_KEEP_GPIO                 GPIO_NUM_41

// 电源按键 GPIO
#define PWR_KEY_GPIO                  GPIO_NUM_40

// 是否低电平按下
#define KEY_ACTIVE_LEVEL              0

// PWR_KEEP 有效电平
#define KEEP_ACTIVE_LEVEL             1

// 长按关机时间
#define LONG_PRESS_MS                 3000

// 去抖时间
#define DEBOUNCE_MS                   30

// 轮询周期
#define POLL_MS                       10

// 开机后忽略时间
// 防止“按住开机”直接触发关机
#define BOOT_IGNORE_MS                2000



//==============================
// 内部状态
//==============================

static bool s_inited = false;



//==============================
// GPIO辅助
//==============================

static inline bool key_is_pressed(void)
{
    return gpio_get_level(PWR_KEY_GPIO) == KEY_ACTIVE_LEVEL;
}

/* internal helper: control PWR_KEEP output pin */
static inline void hold_power(bool en)
{
    gpio_set_level(
        PWR_KEEP_GPIO,
        en ? KEEP_ACTIVE_LEVEL : !KEEP_ACTIVE_LEVEL
    );
}





//==============================
// 硬件初始化
//==============================

static void pwrkeep_gpio_init(void)
{
    //==========================
    // KEEP
    //==========================

    gpio_config_t keep_cfg = {
        .pin_bit_mask = 1ULL << PWR_KEEP_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&keep_cfg);

    // 上电立即保持
    hold_power(true);

    //==========================
    // KEY
    //==========================

    gpio_config_t key_cfg = {
        .pin_bit_mask = 1ULL << PWR_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&key_cfg);

    // 强制浮空
    gpio_set_pull_mode(PWR_KEY_GPIO, GPIO_FLOATING);

    // 防止 HOLD 残留
    gpio_hold_dis(PWR_KEY_GPIO);
    gpio_hold_dis(PWR_KEEP_GPIO);

    ESP_LOGI(TAG, "GPIO init done");
}



//==============================
// 关机
//==============================

static void power_off(void)
{
    ESP_LOGW(TAG, "POWER OFF");

    // 先取消保持
    hold_power(false);

    // 在断电前触发 LED 第二颗淡出（尝试优雅提示）
    // led_ctrl_shutdown_fade 是阻塞直到完成的（会短暂停顿）
    extern void led_ctrl_shutdown_fade(void);
    led_ctrl_shutdown_fade();

    // 再切输入下拉
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PWR_KEEP_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&cfg);

    // 等待断电
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}



//==============================
// 按键任务
//==============================

static void pwrkeep_task(void *arg)
{
    ESP_LOGI(TAG, "task start");

    //==========================
    // 开机保护
    //==========================

    vTaskDelay(pdMS_TO_TICKS(BOOT_IGNORE_MS));

    // 等待按键释放
    // 防止按住开机直接进入长按
    while (key_is_pressed()) {
        ESP_LOGI(TAG, "wait key release...");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "key released, start monitor");



    //==========================
    // 状态机
    //==========================

    bool last_raw = !KEY_ACTIVE_LEVEL;

    bool stable_state = !KEY_ACTIVE_LEVEL;

    uint32_t debounce_cnt = 0;

    uint32_t press_ms = 0;



    while (1) {

        bool raw = key_is_pressed();



        //======================
        // 去抖
        //======================

        if (raw != last_raw) {

            last_raw = raw;

            debounce_cnt = 0;
        }
        else {

            if (debounce_cnt < DEBOUNCE_MS) {

                debounce_cnt += POLL_MS;
            }
            else {

                stable_state = raw;
            }
        }



        //======================
        // 长按检测
        //======================

        if (stable_state) {

            press_ms += POLL_MS;

            // 调试输出
            static uint32_t dbg = 0;

            dbg += POLL_MS;

            if (dbg >= 500) {

                dbg = 0;

                ESP_LOGI(TAG,
                         "pressed: %d ms",
                         press_ms);
            }

            // 长按关机
            if (press_ms >= LONG_PRESS_MS) {

                ESP_LOGW(TAG,
                         "long press detected");

                power_off();
            }
            else {
                // 在按住过程中，用第二颗 LED 显示红色并缓慢变暗
                // 亮度随剩余时间线性减小，从 255 到 0
                uint32_t rem = (press_ms >= LONG_PRESS_MS) ? 0 : (LONG_PRESS_MS - press_ms);
                uint8_t val = (uint8_t)((rem * 255) / LONG_PRESS_MS);
                // idx=1 为第二颗灯（0-based）
                led_ctrl_set_led_color(1, val, 0, 0);
            }
        }
        else {

            // 松开
            if (press_ms > 0) {

                ESP_LOGI(TAG,
                         "key released: %d ms",
                         press_ms);
            }

            // 松开时关闭第二颗灯
            led_ctrl_set_led_color(1, 0, 0, 0);

            press_ms = 0;
        }



        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}



//==============================
// 外部接口
//==============================

esp_err_t pwrkeep_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    pwrkeep_gpio_init();

    BaseType_t ret = xTaskCreate(
        pwrkeep_task,
        "pwrkeep_task",
        4096,
        NULL,
        10,
        NULL
    );

    if (ret != pdPASS) {

        ESP_LOGE(TAG,
                 "task create failed");

        return ESP_FAIL;
    }

    s_inited = true;

    ESP_LOGI(TAG,
             "init success");

    return ESP_OK;
}