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
         * /pet 当前状态上传间隔。
         * 行为识别仍然使用多秒历史窗口；这里仅控制网页/服务器看到设备在线的心跳频率。
         * 建议正常模式 1000ms；0 表示使用默认 1000ms。
         */
        uint32_t http_report_interval_ms;

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
 * @brief 请求文件上传器立即扫描并上传已经完成的 CSV。
 *
 * 用于网页结束录制后尽快上传 R000xxx.CSV 高频 raw 文件。
 */
void pet_collar_monitor_request_file_upload(void);

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
