#include "pet_collar_monitor.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "PET_MON"

static qmi8658a_handle_t s_imu;
static pet_behavior_handle_t s_behavior;
static TaskHandle_t s_task;
static pet_collar_monitor_config_t s_cfg;
static pet_behavior_result_t s_last_result;
static bool s_have_result;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void pet_monitor_task(void *arg)
{
    qmi8658a_sample_t sample;
    pet_behavior_result_t result;
    char event_buf[96];
    uint32_t last_log_ms = 0;
    pet_state_t last_print_state = PET_STATE_UNKNOWN;

    if (s_cfg.enable_gyro_calibration)
    {
        ESP_LOGI(TAG, "gyro calibration start, keep device still if possible");
        esp_err_t ret = qmi8658a_calibrate_gyro_bias(s_imu, 100, 20);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "gyro calibration failed: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "behavior monitor task started, period=%lu ms", (unsigned long)s_cfg.sample_period_ms);

    while (1)
    {
        if (qmi8658a_read_sample(s_imu, &sample) == ESP_OK)
        {
            bool changed = pet_behavior_update(s_behavior, &sample, now_ms(), &result);
            s_last_result = result;
            s_have_result = true;

            uint32_t t = now_ms();

            bool state_changed = (result.state != last_print_state);
            bool time_to_print = (t - last_log_ms) >= 1000;

            if (changed && (state_changed || time_to_print))
            {
                pet_events_to_str(result.events, event_buf, sizeof(event_buf));

                ESP_LOGI(TAG,
                         "state=%s candidate=%s event=%s acc=%.3f gyro=%.2f "
                         "acc_std=%.3f gyro_std=%.2f pitch=%.1f roll=%.1f",
                         pet_state_to_str(result.state),
                         pet_state_to_str(result.candidate_state),
                         event_buf,
                         result.acc_norm_g,
                         result.gyro_norm_dps,
                         result.acc_norm_std,
                         result.gyro_norm_std,
                         result.pitch_deg,
                         result.roll_deg);

                last_log_ms = t;
                last_print_state = result.state;
            }
        }
        else
        {
            ESP_LOGW(TAG, "QMI8658A read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.sample_period_ms));
    }
}

esp_err_t pet_collar_monitor_start(const pet_collar_monitor_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->bus, ESP_ERR_INVALID_ARG, TAG, "invalid config");
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_cfg = *cfg;
    if (s_cfg.qmi8658a_addr == 0)
    {
        s_cfg.qmi8658a_addr = QMI8658A_I2C_ADDR_LOW;
    }
    if (s_cfg.sample_period_ms == 0)
    {
        s_cfg.sample_period_ms = 20;
    }
    if (s_cfg.task_stack_size == 0)
    {
        s_cfg.task_stack_size = 4096;
    }
    if (s_cfg.task_priority == 0)
    {
        s_cfg.task_priority = 5;
    }

    qmi8658a_config_t imu_cfg = {
        .bus = s_cfg.bus,
        .i2c_addr = s_cfg.qmi8658a_addr,
        .scl_speed_hz = 400000,
        .accel_fs = QMI8658A_ACCEL_FS_8G,
        .gyro_fs = QMI8658A_GYRO_FS_512DPS,
        .accel_odr = QMI8658A_ACC_ODR_500HZ,
        .gyro_odr = QMI8658A_GYR_ODR_448HZ,
        .enable_lpf = true,
    };

    ESP_RETURN_ON_ERROR(qmi8658a_create(&imu_cfg, &s_imu), TAG, "qmi8658a_create failed");

    esp_err_t ret = qmi8658a_init(s_imu);
    if (ret != ESP_OK)
    {
        qmi8658a_delete(s_imu);
        s_imu = NULL;
        return ret;
    }

    pet_behavior_config_t behavior_cfg;
    pet_behavior_default_config(&behavior_cfg);
    behavior_cfg.sample_rate_hz = 1000.0f / (float)s_cfg.sample_period_ms;

    s_behavior = pet_behavior_create(&behavior_cfg);
    if (!s_behavior)
    {
        qmi8658a_delete(s_imu);
        s_imu = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(pet_monitor_task,
                                "pet_monitor",
                                s_cfg.task_stack_size,
                                NULL,
                                s_cfg.task_priority,
                                &s_task);
    if (ok != pdPASS)
    {
        pet_behavior_delete(s_behavior);
        qmi8658a_delete(s_imu);
        s_behavior = NULL;
        s_imu = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "pet collar monitor started");
    return ESP_OK;
}

esp_err_t pet_collar_monitor_stop(void)
{
    if (s_task)
    {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_behavior)
    {
        pet_behavior_delete(s_behavior);
        s_behavior = NULL;
    }
    if (s_imu)
    {
        qmi8658a_delete(s_imu);
        s_imu = NULL;
    }
    s_have_result = false;
    return ESP_OK;
}

bool pet_collar_monitor_get_last_result(pet_behavior_result_t *out)
{
    if (!out || !s_have_result)
    {
        return false;
    }
    *out = s_last_result;
    return true;
}
