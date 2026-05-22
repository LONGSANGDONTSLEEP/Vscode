#include "pet_collar_monitor.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pet_data_logger.h"
#include "pet_telemetry.h"
#include "pet_file_uploader.h"

#define TAG "PET_MON"

/* 终端只看稳定后的当前状态，不打印 candidate/raw/debug，避免误解短窗口抖动。 */
#define PET_TERMINAL_STATE_REPORT_MS 5000

static qmi8658a_handle_t s_imu;
static pet_behavior_handle_t s_behavior;
static TaskHandle_t s_task;
static pet_collar_monitor_config_t s_cfg;
static pet_behavior_result_t s_last_result;
static bool s_have_result;
static bool s_logger_ready;
static bool s_telemetry_ready;
static bool s_file_uploader_ready;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void pet_monitor_task(void *arg)
{
    qmi8658a_sample_t sample;
    pet_behavior_result_t result;
    uint32_t last_user_report_ms = 0;
    pet_state_t last_print_state = PET_STATE_UNKNOWN;

    /*
     * 初始化 SD 卡数据记录器。
     * 如果 SD 卡没插或挂载失败，这里只打印 warning，不影响宠物行为监测。
     */
    esp_err_t log_ret = pet_data_logger_init();
    if (log_ret == ESP_OK)
    {
        s_logger_ready = true;
        ESP_LOGI(TAG, "pet data logger ready");
    }
    else
    {
        s_logger_ready = false;
        ESP_LOGW(TAG, "pet data logger unavailable: %s", esp_err_to_name(log_ret));
    }

    /*
     * 初始化 HTTP 实时上传。
     * 注意：Wi-Fi 如果还没连上，HTTP POST 会失败，但不会影响 SD 卡记录和行为监测。
     */
    if (s_cfg.enable_http_upload && s_cfg.http_url && s_cfg.http_url[0])
    {
        pet_telemetry_config_t tel_cfg = {
            .url = s_cfg.http_url,
            .queue_size = 16,
            .task_stack_size = 6144,
            .task_priority = 4,
            .timeout_ms = s_cfg.http_timeout_ms ? s_cfg.http_timeout_ms : 1500,
        };

        esp_err_t tel_ret = pet_telemetry_start(&tel_cfg);
        if (tel_ret == ESP_OK)
        {
            s_telemetry_ready = true;
            ESP_LOGI(TAG, "pet telemetry ready: %s", s_cfg.http_url);
        }
        else
        {
            s_telemetry_ready = false;
            ESP_LOGW(TAG, "pet telemetry unavailable: %s", esp_err_to_name(tel_ret));
        }
    }
    else
    {
        s_telemetry_ready = false;
        ESP_LOGI(TAG, "pet telemetry disabled");
    }

    /*
     * 初始化 CSV 文件上传器。
     * 它会上传已经轮转完成的 Sxxxxxx.CSV / Exxxxxx.CSV。
     * 当前正在写的分段不会上传。
     */
    if (s_logger_ready &&
        s_cfg.enable_file_upload &&
        s_cfg.file_upload_url &&
        s_cfg.file_upload_url[0])
    {

        pet_file_uploader_config_t file_cfg = {
            .url = s_cfg.file_upload_url,
            .scan_interval_ms = s_cfg.file_upload_scan_interval_ms ? s_cfg.file_upload_scan_interval_ms : 60000,
            .task_stack_size = 6144,
            .task_priority = 3,
            .timeout_ms = 5000,
        };

        esp_err_t up_ret = pet_file_uploader_start(&file_cfg);
        if (up_ret == ESP_OK)
        {
            s_file_uploader_ready = true;
            ESP_LOGI(TAG, "pet file uploader ready: %s", s_cfg.file_upload_url);
        }
        else
        {
            s_file_uploader_ready = false;
            ESP_LOGW(TAG, "pet file uploader unavailable: %s", esp_err_to_name(up_ret));
        }
    }
    else
    {
        s_file_uploader_ready = false;
        ESP_LOGI(TAG, "pet file uploader disabled");
    }

    /*
     * 上电后不要立刻校准 gyro。
     *
     * 原因：
     * 1. 启动阶段 PWRKEEP 按键可能还没释放
     * 2. 用户可能还在拿着板子
     * 3. 这时候校准会把手动晃动当成 gyro 零偏
     */
    if (s_cfg.enable_gyro_calibration)
    {
        ESP_LOGI(TAG, "wait before gyro calibration...");
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGI(TAG, "gyro calibration start, keep device still");
        esp_err_t ret = qmi8658a_calibrate_gyro_bias(s_imu, 100, 20);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "gyro calibration failed: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG,
             "behavior monitor task started, period=%lu ms",
             (unsigned long)s_cfg.sample_period_ms);

    while (1)
    {
        if (qmi8658a_read_sample(s_imu, &sample) == ESP_OK)
        {
            uint32_t t = now_ms();

            bool changed = pet_behavior_update(s_behavior, &sample, t, &result);
            s_last_result = result;
            s_have_result = true;

            /*
             * pet_behavior_update() 返回 true 时，通常代表一个状态窗口结束，
             * 也就是现在适合打印日志和写 SD 卡。
             */
            if (changed)
            {
                bool state_changed = (result.state != last_print_state);
                bool time_to_report = (t - last_user_report_ms) >= PET_TERMINAL_STATE_REPORT_MS;
                bool should_report_user_state = state_changed || time_to_report;

                /*
                 * 终端只显示“用户真正需要看的当前状态”。
                 * candidate/raw_state/score 仍然会写入 SD 卡 CSV，便于后续调参；
                 * 但不再每秒刷一堆内部字段，避免测试时看起来很乱。
                 */
                if (should_report_user_state)
                {
                    ESP_LOGI(TAG,
                             "current_state=%s",
                             pet_state_to_str(result.state));

                    last_user_report_ms = t;
                    last_print_state = result.state;
                }

                /*
                 * 写入 SD 卡状态日志。
                 * 失败只 warning，不影响主监测任务继续跑。
                 */
                if (s_logger_ready)
                {
                    esp_err_t ret = pet_data_logger_write_state(t, &result);
                    if (ret != ESP_OK)
                    {
                        ESP_LOGW(TAG, "write state log failed: %s", esp_err_to_name(ret));
                    }

                    if (result.events != PET_EVENT_NONE)
                    {
                        ret = pet_data_logger_write_event(t, &result);
                        if (ret != ESP_OK)
                        {
                            ESP_LOGW(TAG, "write event log failed: %s", esp_err_to_name(ret));
                        }
                    }
                }
                /*
                 * HTTP 上传只入队，不直接阻塞 pet_monitor。
                 * 如果 Wi-Fi 断开或电脑服务没开，上传失败也不会影响 SD 卡记录。
                 */
                if (s_telemetry_ready && should_report_user_state)
                {
                    esp_err_t ret = pet_telemetry_enqueue_state(t, &result);
                    if (ret != ESP_OK)
                    {
                        ESP_LOGW(TAG, "enqueue telemetry failed: %s", esp_err_to_name(ret));
                    }
                }
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
        /*
         * IMU 读取周期提高到 10ms，也就是 100Hz。
         * 原来 20ms = 50Hz，慢走/走走停停的细节容易被漏掉。
         */
        s_cfg.sample_period_ms = 10;
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
        /*
         * 提高加速度计分辨率：±8g -> ±4g。
         * QMI8658A 在 ±4g 下是 8192 LSB/g，比 ±8g 的 4096 LSB/g 更细。
         * 宠物项圈正常走路/趴着/小跑通常不会超过 ±4g；如果你主要测试剧烈甩动，才改回 ±8g。
         */
        .accel_fs = QMI8658A_ACCEL_FS_4G,
        /*
         * 陀螺仪保持 ±512dps，避免 SHAKE/PLAY 时过早饱和。
         */
        .gyro_fs = QMI8658A_GYRO_FS_512DPS,
        /*
         * 提高 IMU 内部 ODR，主任务仍按 sample_period_ms 读取。
         */
        .accel_odr = QMI8658A_ACC_ODR_1000HZ,
        .gyro_odr = QMI8658A_GYR_ODR_896HZ,
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
    /*
     * 每 1 秒完成一个判断窗口，这样终端日志和 HTTP POST 都能 1 秒级刷新。
     * 如果后面觉得状态抖动，再把 min_state_hold_ms 调到 2000~3000。
     */
    behavior_cfg.window_ms = 1000;
    behavior_cfg.min_state_hold_ms = 2000;

    s_behavior = pet_behavior_create(&behavior_cfg);
    if (!s_behavior)
    {
        qmi8658a_delete(s_imu);
        s_imu = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(
        pet_monitor_task,
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

    if (s_telemetry_ready)
    {
        pet_telemetry_stop();
    }

    if (s_file_uploader_ready)
    {
        pet_file_uploader_stop();
    }
    s_have_result = false;
    s_logger_ready = false;
    s_telemetry_ready = false;
    s_file_uploader_ready = false;

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