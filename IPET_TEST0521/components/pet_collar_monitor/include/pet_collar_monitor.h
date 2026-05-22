#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "pet_behavior.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        i2c_master_bus_handle_t bus;
        uint8_t qmi8658a_addr;
        uint32_t sample_period_ms;
        uint32_t task_stack_size;
        uint32_t task_priority;
        bool enable_gyro_calibration;

        /*
         * JSON 实时状态上传。
         */
        bool enable_http_upload;
        const char *http_url;
        uint32_t http_timeout_ms;

        /*
         * CSV 文件分段上传。
         */
        bool enable_file_upload;
        const char *file_upload_url;
        uint32_t file_upload_scan_interval_ms;
    } pet_collar_monitor_config_t;

    esp_err_t pet_collar_monitor_start(const pet_collar_monitor_config_t *cfg);
    esp_err_t pet_collar_monitor_stop(void);
    bool pet_collar_monitor_get_last_result(pet_behavior_result_t *out);

    /**
     * @brief 进入/退出高频录制模式。
     *
     * enabled=true 时，监测任务使用更短 sample_period_ms，并可把每个 IMU sample 写入 Rxxxxxx.CSV。
     * sample_period_ms 建议 5~10ms；0 表示使用默认 5ms。
     */
    esp_err_t pet_collar_monitor_set_recording_mode(bool enabled,
                                                    uint32_t sample_period_ms,
                                                    bool raw_sample_log);
    bool pet_collar_monitor_is_recording_mode(void);

#ifdef __cplusplus
}
#endif
