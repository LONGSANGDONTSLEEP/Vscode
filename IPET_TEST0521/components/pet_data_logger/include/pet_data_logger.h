#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "pet_behavior.h"
#include "qmi8658a.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_data_logger_init(void);
bool pet_data_logger_is_ready(void);

esp_err_t pet_data_logger_write_state(uint32_t now_ms, const pet_behavior_result_t *result);
esp_err_t pet_data_logger_write_event(uint32_t now_ms, const pet_behavior_result_t *result);

/**
 * @brief 开关高频 raw IMU 日志。
 *
 * 开启后会生成 Rxxxxxx.CSV，按采样周期记录 ax/gx 原始值和换算值。
 */
esp_err_t pet_data_logger_set_raw_enabled(bool enabled);
bool pet_data_logger_raw_is_enabled(void);

/**
 * @brief 结束一次 raw 录制：关闭 Rxxxxxx.CSV，并强制切到下一段。
 *
 * 这样刚结束的 R/S/E 文件不再是 current segment，文件上传器可以立刻上传。
 */
esp_err_t pet_data_logger_finish_raw_recording(void);
esp_err_t pet_data_logger_write_raw_sample(uint32_t now_ms,
                                           const qmi8658a_sample_t *sample,
                                           const pet_behavior_result_t *result);

/**
 * @brief 获取当前测试 session 目录。
 *
 * 例如：
 * /sdcard/D260521/T125011/2b324939
 */
const char *pet_data_logger_get_session_dir(void);

/**
 * @brief 获取当前正在写入的分段编号。
 *
 * 当前正在写 S000003.CSV 时，返回 3。
 * 上传器只上传小于当前编号的文件。
 */
uint32_t pet_data_logger_get_current_segment(void);

/**
 * @brief 手动切换到下一个分段文件。
 */
esp_err_t pet_data_logger_force_rotate(void);

#ifdef __cplusplus
}
#endif