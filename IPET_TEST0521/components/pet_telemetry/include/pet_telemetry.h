#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "pet_behavior.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *url;              // 例如 "http://192.168.1.12:8080/pet"
    uint32_t queue_size;          // 默认 8
    uint32_t task_stack_size;     // 默认 6144
    uint32_t task_priority;       // 默认 4
    uint32_t timeout_ms;          // 默认 2000
} pet_telemetry_config_t;

/**
 * @brief 启动 HTTP 上传任务。
 */
esp_err_t pet_telemetry_start(const pet_telemetry_config_t *cfg);

/**
 * @brief 停止 HTTP 上传任务。
 */
esp_err_t pet_telemetry_stop(void);

/**
 * @brief HTTP 上传任务是否可用。
 */
bool pet_telemetry_is_ready(void);

/**
 * @brief 把当前状态放入上传队列。
 *
 * 这个函数不会直接阻塞发送 HTTP，只是把数据丢给后台任务。
 */
esp_err_t pet_telemetry_enqueue_state(uint32_t now_ms, const pet_behavior_result_t *result);

#ifdef __cplusplus
}
#endif