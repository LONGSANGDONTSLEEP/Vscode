// pwrkeep.c - 电源保持与按键长按关机组件
#include "pwrkeep.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_PWRKEEP_GPIO
#define CONFIG_PWRKEEP_GPIO 41
#endif

#ifndef CONFIG_PWRKEY_GPIO
#define CONFIG_PWRKEY_GPIO 40
#endif

#ifndef CONFIG_PWRKEY_ACTIVE_LOW
#define CONFIG_PWRKEY_ACTIVE_LOW 1
#endif

#ifndef CONFIG_PWRKEEP_HOLD_ACTIVE_HIGH
#define CONFIG_PWRKEEP_HOLD_ACTIVE_HIGH 1
#endif

#ifndef CONFIG_PWRKEY_LONG_PRESS_MS
#define CONFIG_PWRKEY_LONG_PRESS_MS 3000
#endif

#ifndef CONFIG_PWRKEY_INTERNAL_PULLUP
#define CONFIG_PWRKEY_INTERNAL_PULLUP 1
#endif

#ifndef CONFIG_PWRKEY_INTERNAL_PULLDOWN
#define CONFIG_PWRKEY_INTERNAL_PULLDOWN 0
#endif

#ifndef CONFIG_PWRKEEP_RELEASE_USE_PULLDOWN
#define CONFIG_PWRKEEP_RELEASE_USE_PULLDOWN 1
#endif

/* 上电后忽略按键长按的时间，避免开机按键在上电瞬间被识别为长按关机（毫秒） */
#ifndef CONFIG_PWRKEEP_BOOT_IGNORE_MS
#define CONFIG_PWRKEEP_BOOT_IGNORE_MS 5000
#endif

#ifndef CONFIG_PWRKEEP_TASK_STACK
#define CONFIG_PWRKEEP_TASK_STACK 4096
#endif

#ifndef CONFIG_PWRKEEP_TASK_PRIO
#define CONFIG_PWRKEEP_TASK_PRIO 10
#endif

static const char *TAG = "PWRKEEP";

static TaskHandle_t s_pwrkeep_task = NULL;
static bool s_inited = false;
/* 如果上电时按键被按住，先等待松开再开始长按检测，避免上电瞬间被识别为长按关机 */
static bool s_wait_release_on_boot = false;

static inline int hold_active_level(void) { return CONFIG_PWRKEEP_HOLD_ACTIVE_HIGH ? 1 : 0; }
static inline int hold_inactive_level(void) { return 1 - hold_active_level(); }
static inline int key_active_level(void) { return CONFIG_PWRKEY_ACTIVE_LOW ? 0 : 1; }

static void pwrkeep_task(void *arg)
{
    const int poll_ms = 10;
    int pressed_ms = 0;
    int prev_lvl = -1;
    const int long_press_ms = CONFIG_PWRKEY_LONG_PRESS_MS;
    /* 记录任务启动时刻并计算上电忽略窗口（防止用户在按键上电时保持按下导致误判）
     * 忽略窗口至少为长按阈值，避免用户持续按住开机键跨过上电阶段被误判为关机 */
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t boot_ignore_ticks = pdMS_TO_TICKS(CONFIG_PWRKEEP_BOOT_IGNORE_MS);
    TickType_t long_press_ticks = pdMS_TO_TICKS(long_press_ms);
    TickType_t initial_ignore = (boot_ignore_ticks > long_press_ticks) ? boot_ignore_ticks : long_press_ticks;
    TickType_t ignore_until = start_tick + initial_ignore;

    /* 用于周期性输出按键/保持脚的电平及长按计时调试信息 */
    TickType_t last_dbg_tick = start_tick;

    /* 如果启动时按键被保持按下，则等待松开；松开后再额外忽略一段时间以避免抖动 */
    s_wait_release_on_boot = (gpio_get_level((gpio_num_t)CONFIG_PWRKEY_GPIO) == key_active_level());
    if (s_wait_release_on_boot) {
        ESP_LOGI(TAG, "Power key held at boot, will wait for release and ignore long-press for %d ms (min %d ms) after release",
                 CONFIG_PWRKEEP_BOOT_IGNORE_MS, (int)pdTICKS_TO_MS(initial_ignore));
    }

    ESP_LOGI(TAG, "Task started: key=GPIO%d (%s), keep=GPIO%d (active %s), long=%dms, boot_ignore=%dms",
             (int)CONFIG_PWRKEY_GPIO, CONFIG_PWRKEY_ACTIVE_LOW ? "active_low" : "active_high",
             (int)CONFIG_PWRKEEP_GPIO, CONFIG_PWRKEEP_HOLD_ACTIVE_HIGH ? "high" : "low",
             long_press_ms, CONFIG_PWRKEEP_BOOT_IGNORE_MS);

    /* 记录启动时按键电平，方便排查硬件接线与上下拉 */
    int init_key_lvl = gpio_get_level((gpio_num_t)CONFIG_PWRKEY_GPIO);
    ESP_LOGI(TAG, "Initial key level at task start: %d (active_level=%d)", init_key_lvl, key_active_level());

    while (1) {
        int lvl = gpio_get_level((gpio_num_t)CONFIG_PWRKEY_GPIO);
        TickType_t now = xTaskGetTickCount();

        /* 周期性 debug 日志，输出 key/keep 的电平、已按下时间和忽略窗口剩余 */
        if ((now - last_dbg_tick) >= pdMS_TO_TICKS(500)) {
            int keep_lvl = gpio_get_level((gpio_num_t)CONFIG_PWRKEEP_GPIO);
            TickType_t ignore_remain = (now > ignore_until) ? 0 : (ignore_until - now);
            ESP_LOGI(TAG, "DBG: key_gpio=%d keep_gpio=%d key_lvl=%d keep_lvl=%d pressed_ms=%d ignore_remain=%dms",
                     (int)CONFIG_PWRKEY_GPIO, (int)CONFIG_PWRKEEP_GPIO,
                     lvl, keep_lvl, pressed_ms, (int)pdTICKS_TO_MS(ignore_remain));
            last_dbg_tick = now;
        }

        /* 如果上电时按键被按住，等待松开后再启用长按检测（并在松开后额外忽略一段时间） */
        if (s_wait_release_on_boot) {
            if (lvl != key_active_level()) {
                s_wait_release_on_boot = false;
                pressed_ms = 0;
                /* 释放后设定忽略窗口，至少等于长按阈值，避免一次跨越上电全过程的按压触发关机 */
                ignore_until = now + ((boot_ignore_ticks > long_press_ticks) ? boot_ignore_ticks : long_press_ticks);
                ESP_LOGI(TAG, "Power key released after boot; long-press detection enabled after %d ms",
                         (int)pdTICKS_TO_MS(ignore_until - now));
            } else {
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
                continue;
            }
        }

        /* 记录按键电平变化以便调试：按下/释放事件 */
        if (prev_lvl == -1) {
            prev_lvl = lvl;
        } else if (lvl != prev_lvl) {
            if (lvl == key_active_level()) {
                ESP_LOGI(TAG, "Key pressed (GPIO%d)", (int)CONFIG_PWRKEY_GPIO);
            } else {
                ESP_LOGI(TAG, "Key released (GPIO%d), pressed_ms=%d", (int)CONFIG_PWRKEY_GPIO, pressed_ms);
            }
            prev_lvl = lvl;
        }

        /* 在忽略窗口内不进行长按计时（避免用户在按键开机时被误判为长按） */
        if (now <= ignore_until) {
            pressed_ms = 0;
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            continue;
        }

        if (lvl == key_active_level()) {
            pressed_ms += poll_ms;
            if (pressed_ms >= long_press_ms) {
                ESP_LOGW(TAG, "Long-press detected (%d ms) -> releasing hold to power off", pressed_ms);

                // 关机策略：优先使用下拉释放以适配外部上拉/栅极电荷（硬件建议）
#if CONFIG_PWRKEEP_RELEASE_USE_PULLDOWN
                // 先拉到无效电平快速放电
                gpio_set_level((gpio_num_t)CONFIG_PWRKEEP_GPIO, hold_inactive_level());
                // 立即将保持引脚改为输入下拉，确保在关断瞬间提供下拉路径
                gpio_config_t keep_down = {
                    .pin_bit_mask = 1ULL << CONFIG_PWRKEEP_GPIO,
                    .mode = GPIO_MODE_INPUT,
                    .pull_up_en = 0,
                    .pull_down_en = 1,
                    .intr_type = GPIO_INTR_DISABLE,
                };
                gpio_config(&keep_down);
                ESP_LOGI(TAG, "PWRKEEP set to INPUT with PULLDOWN for shutdown (GPIO%d)", (int)CONFIG_PWRKEEP_GPIO);
#else
                // 直接输出无效电平
                gpio_set_level((gpio_num_t)CONFIG_PWRKEEP_GPIO, hold_inactive_level());
#endif
                // 等待电源切断；若电源未立即断开则延时一小段
                vTaskDelay(pdMS_TO_TICKS(500));
                // 若仍未断电，继续保持空转（通常不会运行到这里）
                pressed_ms = 0;
            }
        } else {
            // 松开按键，计时清零
            pressed_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void pwrkeep_set_hold(bool enable)
{
    int lvl = enable ? hold_active_level() : hold_inactive_level();
    gpio_set_level((gpio_num_t)CONFIG_PWRKEEP_GPIO, lvl);
    ESP_LOGI(TAG, "Hold %s (GPIO%d=%d)", enable ? "EN" : "DIS", (int)CONFIG_PWRKEEP_GPIO, lvl);
}

static void pwrkeep_hw_init_once(void)
{
    // 配置保持引脚为输出，并默认拉到保持电平以维持供电
    gpio_config_t keep_conf = {
        .pin_bit_mask = 1ULL << CONFIG_PWRKEEP_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&keep_conf);
    gpio_set_level((gpio_num_t)CONFIG_PWRKEEP_GPIO, hold_active_level());

    // 配置按键引脚为输入，默认上拉（active_low）或下拉（active_high）
    gpio_config_t key_conf = {
        .pin_bit_mask = 1ULL << CONFIG_PWRKEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_PWRKEY_ACTIVE_LOW ? CONFIG_PWRKEY_INTERNAL_PULLUP : 0,
        .pull_down_en = CONFIG_PWRKEY_ACTIVE_LOW ? 0 : CONFIG_PWRKEY_INTERNAL_PULLDOWN,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&key_conf);
}

esp_err_t pwrkeep_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    pwrkeep_hw_init_once();

    BaseType_t rc = xTaskCreate(
        pwrkeep_task,
        "pwrkeep_task",
        CONFIG_PWRKEEP_TASK_STACK,
        NULL,
        CONFIG_PWRKEEP_TASK_PRIO,
        &s_pwrkeep_task
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pwrkeep task");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "Initialized: KEEP=GPIO%d, KEY=GPIO%d", (int)CONFIG_PWRKEEP_GPIO, (int)CONFIG_PWRKEY_GPIO);
    return ESP_OK;
}
