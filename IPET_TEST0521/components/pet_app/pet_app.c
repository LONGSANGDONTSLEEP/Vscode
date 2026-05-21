#include "pet_app.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio_drv.h"
#include "hwinit.h"
#include "ota_update.h"
#include "pet_behavior.h"
#include "pet_collar_monitor.h"
#include "pet_config.h"
#include "pet_time.h"
#include "qmi8658a.h"
#include "wifi_manager.h"

#define TAG "PET_APP"

#define PET_APP_OTA_BUTTON_GPIO GPIO_NUM_0
#define PET_APP_OTA_URL "http://192.168.1.12:8070/OTA.bin"

#define PET_APP_WIFI_TIMEOUT_MS 15000U
#define PET_APP_NTP_TIMEOUT_MS 10000U
#define PET_APP_HTTP_TIMEOUT_MS 2000U
#define PET_APP_HEARTBEAT_PERIOD_MS 30000U
#define PET_APP_MONITOR_SAMPLE_PERIOD_MS 20U
#define PET_APP_MONITOR_STACK_SIZE 4096U
#define PET_APP_MONITOR_TASK_PRIORITY 5U
#define PET_APP_HEARTBEAT_STACK_SIZE 3072U
#define PET_APP_HEARTBEAT_TASK_PRIORITY 2U

static pet_config_t s_app_cfg;
static TaskHandle_t s_heartbeat_task;
static bool s_started;

static void pet_app_print_stack(const char *tag)
{
    ESP_LOGI(tag,
             "stack high water mark: %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static bool pet_app_connect_wifi_for_telemetry(void)
{
    ESP_LOGI(TAG, "connecting WiFi for telemetry...");

    if (!wifi_manager_connect_blocking(s_app_cfg.wifi_ssid,
                                       s_app_cfg.wifi_pass,
                                       PET_APP_WIFI_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "WiFi connect failed, telemetry will be unavailable");
        return false;
    }

    ESP_LOGI(TAG, "WiFi connected for telemetry");

    esp_err_t ret = pet_time_sync_ntp(PET_APP_NTP_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NTP time sync success");
    } else {
        ESP_LOGW(TAG, "NTP time sync failed: %s", esp_err_to_name(ret));
    }

    return true;
}

static void pet_app_trigger_ota(void)
{
    ESP_LOGI(TAG, "trigger OTA: %s", PET_APP_OTA_URL);

    if (!wifi_manager_connect_blocking(s_app_cfg.wifi_ssid,
                                       s_app_cfg.wifi_pass,
                                       PET_APP_WIFI_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "WiFi not connected, abort OTA");
        return;
    }

    ota_update_start_bg(PET_APP_OTA_URL, NULL);
}

static void pet_app_ota_button_cb(gpio_num_t gpio, uint32_t level)
{
    (void)gpio;

    if (level == 0) {
        ESP_LOGI(TAG, "OTA button pressed");
        pet_app_trigger_ota();
    }
}

static esp_err_t pet_app_register_ota_button(void)
{
    esp_err_t ret = gpio_drv_register_callback(PET_APP_OTA_BUTTON_GPIO,
                                               pet_app_ota_button_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA button register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OTA button registered on GPIO%d", PET_APP_OTA_BUTTON_GPIO);
    return ESP_OK;
}

static esp_err_t pet_app_start_monitor(bool wifi_ok)
{
    const bool enable_json = wifi_ok && s_app_cfg.enable_json_upload;
    const bool enable_file = wifi_ok && s_app_cfg.enable_file_upload;

    pet_collar_monitor_config_t cfg = {
        .bus = hwinit_get_i2c_bus(),
        .qmi8658a_addr = QMI8658A_I2C_ADDR_LOW,
        .sample_period_ms = PET_APP_MONITOR_SAMPLE_PERIOD_MS,
        .task_stack_size = PET_APP_MONITOR_STACK_SIZE,
        .task_priority = PET_APP_MONITOR_TASK_PRIORITY,
        .enable_gyro_calibration = true,

        .enable_http_upload = enable_json,
        .http_url = s_app_cfg.pet_http_url,
        .http_timeout_ms = PET_APP_HTTP_TIMEOUT_MS,

        .enable_file_upload = enable_file,
        .file_upload_url = s_app_cfg.pet_file_upload_url,
        .file_upload_scan_interval_ms = s_app_cfg.file_upload_scan_ms,
    };

    ESP_LOGI(TAG,
             "upload config: json=%d, file=%d",
             enable_json ? 1 : 0,
             enable_file ? 1 : 0);

    esp_err_t ret = pet_collar_monitor_start(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pet monitor start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "pet monitor started");
    return ESP_OK;
}

static void pet_app_heartbeat_task(void *arg)
{
    (void)arg;

    while (1) {
        pet_behavior_result_t r;
        if (pet_collar_monitor_get_last_result(&r)) {
            ESP_LOGI(TAG,
                     "alive, state=%s, stack=%u",
                     pet_state_to_str(r.state),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGI(TAG,
                     "alive, stack=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(PET_APP_HEARTBEAT_PERIOD_MS));
    }
}

static esp_err_t pet_app_start_heartbeat(void)
{
    if (s_heartbeat_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(pet_app_heartbeat_task,
                                "pet_app_alive",
                                PET_APP_HEARTBEAT_STACK_SIZE,
                                NULL,
                                PET_APP_HEARTBEAT_TASK_PRIORITY,
                                &s_heartbeat_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

static void pet_app_load_config(void)
{
    esp_err_t ret = pet_config_load_or_create(&s_app_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "pet config load failed: %s, use defaults",
                 esp_err_to_name(ret));
        pet_config_set_defaults(&s_app_cfg);
    }

    pet_config_print(&s_app_cfg);
}

esp_err_t pet_app_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "application start");
    pet_app_print_stack(TAG);

    pet_time_init();
    pet_app_load_config();

    bool wifi_ok = pet_app_connect_wifi_for_telemetry();

    esp_err_t monitor_ret = pet_app_start_monitor(wifi_ok);
    esp_err_t button_ret = pet_app_register_ota_button();
    esp_err_t heartbeat_ret = pet_app_start_heartbeat();

    ESP_LOGI(TAG, "Bluetooth OTA disabled");
    ESP_LOGI(TAG, "Use BOOT key for WiFi OTA");

    s_started = true;

    if (monitor_ret != ESP_OK) {
        return monitor_ret;
    }
    if (button_ret != ESP_OK) {
        return button_ret;
    }
    return heartbeat_ret;
}
