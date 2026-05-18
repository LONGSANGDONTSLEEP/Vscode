#include "main.h"

#include "hwinit.h"
#include "ota_update.h"
#include "gpio_drv.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_ctrl.h"

// ========== 在这里填写你的 Wi‑Fi 名称和密码（直接修改下面两行） ==========
// 示例：
//    #define MY_WIFI_SSID "MyHomeWiFi"
//    #define MY_WIFI_PASS "MyPassword"
// 请把下面的占位符替换为你的 SSID / 密码，然后重新编译刷机
#define MY_WIFI_SSID "TYZX"
#define MY_WIFI_PASS "ty20260101"

/* 使用开发板的 BOOT 键（通常连接到 GPIO0）作为触发 OTA 的按键 */
#define OTA_BUTTON_GPIO GPIO_NUM_0
#define OTA_URL "http://192.168.1.12:8070/OTA.bin"

/* 触发 OTA 的封装函数（文件级别） */
static void trigger_ota(void)
{
    ESP_LOGI("OTA_CMD", "Triggering OTA from %s", OTA_URL);
    ota_update_start_bg(OTA_URL, NULL);
}

/* 已移除蓝牙 OTA 相关逻辑，如需重新启用请添加相应组件并在此调用 */

/* 按键回调：当按键按下（假设低电平触发）时启动 OTA（文件级别） */
static void ota_button_cb(gpio_num_t gpio, uint32_t level)
{
    if (level == 0) { // 依据你的按键电路，可能需要改为 level==1
        trigger_ota();
    }
}

void app_main(void)
{

    // WiFi 暂时禁用，使用蓝牙 OTA
    // 如果你以后需要启用 WiFi：取消注释下面三行并确保 wifi_manager 可用
    // extern bool wifi_manager_connect_blocking(const char *ssid, const char *pass, int timeout_ms);
    // bool ok = wifi_manager_connect_blocking(MY_WIFI_SSID, MY_WIFI_PASS, 15000);
    // if (!ok) { ESP_LOGW("SYS", "WiFi connect failed or timeout"); }
    ESP_LOGI("SYS", "本版本未启用蓝牙；默认通过 BOOT 键触发 WiFi OTA（请确保已联网）");

    /* 统一使用硬件初始化入口，避免重复初始化 GPIO/LED/任务 */
    hw_init();

    /* 注册按键回调（gpio 驱动由 hw_init 初始化） */
    if (gpio_drv_register_callback(OTA_BUTTON_GPIO, ota_button_cb) != ESP_OK) {
        ESP_LOGW("SYS", "Failed to register OTA button callback (gpio %d)", OTA_BUTTON_GPIO);
    } else {
        ESP_LOGI("SYS", "OTA button registered on gpio %d", OTA_BUTTON_GPIO);
    }

    /* 蓝牙 OTA 已移除，不再启动 */

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(30000)); // 每30秒打印一次日志
        ESP_LOGI("SYS", "Main task running...");
        ESP_LOGI("Stack", "Stack high water mark for main task: %d", uxTaskGetStackHighWaterMark(NULL));
    }
}