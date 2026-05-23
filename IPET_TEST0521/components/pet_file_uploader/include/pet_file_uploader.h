#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *url;                  // 例如 "http://192.168.1.12:8080/upload"
    uint32_t scan_interval_ms;        // 默认 60000
    uint32_t task_stack_size;         // 默认 6144
    uint32_t task_priority;           // 默认 3
    uint32_t timeout_ms;              // 默认 30000，raw CSV 可能较大
} pet_file_uploader_config_t;

esp_err_t pet_file_uploader_start(const pet_file_uploader_config_t *cfg);
esp_err_t pet_file_uploader_stop(void);
bool pet_file_uploader_is_ready(void);

/*
 * 请求上传器尽快扫描一次。
 * 用于录制结束后立即上传刚关闭的 R/S/E CSV，避免等下一轮定时扫描。
 */
void pet_file_uploader_request_scan_now(void);

/* 最近一次上传成功时间，0 表示从未成功。 */
uint32_t pet_file_uploader_last_success_ms(void);

#ifdef __cplusplus
}
#endif