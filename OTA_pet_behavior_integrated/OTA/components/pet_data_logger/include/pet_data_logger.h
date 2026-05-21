#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "pet_behavior.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化宠物数据记录器。
 *
 * 会检查 SD 卡是否已经挂载。
 * 如果 /sdcard/pet_state.csv 或 /sdcard/pet_event.csv 不存在，会自动写入 CSV 表头。
 */
esp_err_t pet_data_logger_init(void);

/**
 * @brief 是否已经初始化成功。
 */
bool pet_data_logger_is_ready(void);

/**
 * @brief 写入一行状态数据到 /sdcard/pet_state.csv。
 */
esp_err_t pet_data_logger_write_state(uint32_t now_ms, const pet_behavior_result_t *result);

/**
 * @brief 如果 result->events != PET_EVENT_NONE，则写入一行事件数据到 /sdcard/pet_event.csv。
 */
esp_err_t pet_data_logger_write_event(uint32_t now_ms, const pet_behavior_result_t *result);

#ifdef __cplusplus
}
#endif