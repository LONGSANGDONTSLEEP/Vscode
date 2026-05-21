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
    uint32_t timeout_ms;              // 默认 5000
} pet_file_uploader_config_t;

esp_err_t pet_file_uploader_start(const pet_file_uploader_config_t *cfg);
esp_err_t pet_file_uploader_stop(void);
bool pet_file_uploader_is_ready(void);

#ifdef __cplusplus
}
#endif