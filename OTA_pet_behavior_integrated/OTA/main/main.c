#include "main.h"

#include "gpio_drv.h"
#include "hwinit.h"
#include "ota_update.h"
#include "pet_collar_monitor.h"
#include "qmi8658a.h"
#include "wifi_manager.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

//==============================
// WiFi 配置
//==============================
#define MY_WIFI_SSID "TYZX"
#define MY_WIFI_PASS "ty20260101"

//==============================
// OTA 配置
//==============================
#define OTA_BUTTON_GPIO GPIO_NUM_0
#define OTA_URL "http://192.168.1.12:8070/OTA.bin" // 本地测试：python -m http.server 8070

static const char *TAG = "SYS";

//==============================
// OTA 启动
//==============================
static void trigger_ota(void)
{
    ESP_LOGI(TAG, "Trigger OTA: %s", OTA_URL);

    // 确保在启动 OTA 前 Wi-Fi 网络栈已准备并已连接。
    if (!wifi_manager_connect_blocking(MY_WIFI_SSID, MY_WIFI_PASS, 15000))
    {
        ESP_LOGW(TAG, "WiFi not connected, abort OTA");
        return;
    }

    ota_update_start_bg(OTA_URL, NULL);
}

//==============================
// OTA 按键回调
//==============================
static void ota_button_cb(gpio_num_t gpio, uint32_t level)
{
    (void)gpio;

    // BOOT 按键低电平按下
    if (level == 0)
    {
        ESP_LOGI(TAG, "OTA button pressed");
        trigger_ota();
    }
}

//==============================
// 宠物行为监测启动
//==============================
static void start_pet_monitor(void)
{
    pet_collar_monitor_config_t cfg = {
        .bus = hwinit_get_i2c_bus(),
        .qmi8658a_addr = QMI8658A_I2C_ADDR_LOW, // 你的日志中 QMI8658A 是 0x6B
        .sample_period_ms = 20,                 // 50Hz 行为识别
        .task_stack_size = 4096,
        .task_priority = 5,
        .enable_gyro_calibration = true,
    };

    esp_err_t ret = pet_collar_monitor_start(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "pet monitor start failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "pet monitor started");
    }
}

//==============================
// app_main
//==============================
void app_main(void)
{
    ESP_LOGI(TAG, "System boot");

    hw_init();
    ESP_LOGI(TAG, "hardware init done");

    start_pet_monitor();

    ESP_LOGI(TAG, "Bluetooth OTA disabled");
    ESP_LOGI(TAG, "Use BOOT key for WiFi OTA");

    esp_err_t ret = gpio_drv_register_callback(OTA_BUTTON_GPIO, ota_button_cb);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA button register failed");
    }
    else
    {
        ESP_LOGI(TAG, "OTA button registered on GPIO%d", OTA_BUTTON_GPIO);
    }

    while (1)
    {
        pet_behavior_result_t r;
        if (pet_collar_monitor_get_last_result(&r))
        {
            ESP_LOGI(TAG,
                     "Main alive, state=%s, stack=%u",
                     pet_state_to_str(r.state),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        else
        {
            ESP_LOGI(TAG,
                     "Main alive, stack=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
