#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *command_url;      /* 例如 http://192.168.1.12:8080/sd_cmd */
    const char *upload_url;       /* 复用 /upload，用于把选中的 SD 文件上传到电脑 */
    uint32_t poll_ms;             /* 轮询网页命令的间隔，建议 1000~3000ms */
    uint32_t http_timeout_ms;     /* HTTP 超时 */
    uint32_t task_stack_size;     /* 默认 8192 */
    uint32_t task_priority;       /* 默认 3 */
} pet_sd_remote_config_t;

esp_err_t pet_sd_remote_start(const pet_sd_remote_config_t *cfg);
esp_err_t pet_sd_remote_stop(void);
bool pet_sd_remote_is_ready(void);

#ifdef __cplusplus
}
#endif
