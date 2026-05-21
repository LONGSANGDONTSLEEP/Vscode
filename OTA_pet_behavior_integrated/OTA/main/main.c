#include "main.h"

#include "gpio_drv.h"
#include "hwinit.h"
#include "ota_update.h"
#include "pet_collar_monitor.h"
#include "qmi8658a.h"
#include "wifi_manager.h"
#include "pet_time.h"
#include "pet_config.h"

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
#define PET_HTTP_URL "http://192.168.1.12:8080/pet"
#define PET_FILE_UPLOAD_URL "http://192.168.1.12:8080/upload"

static const char *TAG = "SYS";
static pet_config_t s_app_cfg;

//==============================
// OTA 启动
//==============================
static void trigger_ota(void)
{
    ESP_LOGI(TAG, "Trigger OTA: %s", OTA_URL);

    if (!wifi_manager_connect_blocking(s_app_cfg.wifi_ssid, s_app_cfg.wifi_pass, 15000))
    {
        ESP_LOGW(TAG, "WiFi not connected, abort OTA");
        return;
    }

    ota_update_start_bg(OTA_URL, NULL);
}

static bool start_wifi_for_telemetry(void)
{
    ESP_LOGI(TAG, "Connecting WiFi for telemetry...");

    if (wifi_manager_connect_blocking(s_app_cfg.wifi_ssid, s_app_cfg.wifi_pass, 15000))
    {
        ESP_LOGI(TAG, "WiFi connected for telemetry");

        esp_err_t ret = pet_time_sync_ntp(10000);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "NTP time sync success");
        }
        else
        {
            ESP_LOGW(TAG, "NTP time sync failed: %s", esp_err_to_name(ret));
        }

        return true;
    }

    ESP_LOGW(TAG, "WiFi connect failed, telemetry will be unavailable");
    return false;
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
static void start_pet_monitor(bool wifi_ok)
{
    bool enable_json = wifi_ok && s_app_cfg.enable_json_upload;
    bool enable_file = wifi_ok && s_app_cfg.enable_file_upload;

    pet_collar_monitor_config_t cfg = {
        .bus = hwinit_get_i2c_bus(),
        .qmi8658a_addr = QMI8658A_I2C_ADDR_LOW,
        .sample_period_ms = 20,
        .task_stack_size = 4096,
        .task_priority = 5,
        .enable_gyro_calibration = true,

        .enable_http_upload = enable_json,
        .http_url = s_app_cfg.pet_http_url,
        .http_timeout_ms = 2000,

        .enable_file_upload = enable_file,
        .file_upload_url = s_app_cfg.pet_file_upload_url,
        .file_upload_scan_interval_ms = s_app_cfg.file_upload_scan_ms,
    };

    ESP_LOGI(TAG,
             "upload config: json=%d, file=%d",
             enable_json ? 1 : 0,
             enable_file ? 1 : 0);

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

    pet_time_init();

    esp_err_t cfg_ret = pet_config_load_or_create(&s_app_cfg);
    if (cfg_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "pet config load failed: %s, use defaults", esp_err_to_name(cfg_ret));
        pet_config_set_defaults(&s_app_cfg);
    }
    pet_config_print(&s_app_cfg);

    bool wifi_ok = start_wifi_for_telemetry();

    start_pet_monitor(wifi_ok);

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