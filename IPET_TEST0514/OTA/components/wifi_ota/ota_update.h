#pragma once

#include <stdbool.h>

#include "esp_err.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_UPDATE_EVENT_STARTED = 0,
    OTA_UPDATE_EVENT_PROGRESS,
    OTA_UPDATE_EVENT_SUCCESS,
    OTA_UPDATE_EVENT_FAILED,
    OTA_UPDATE_EVENT_NO_UPDATE,
} ota_update_event_t;

typedef void (*ota_update_callback_t)(ota_update_event_t event, int progress_percent, esp_err_t err, void *user_ctx);

typedef struct {
    const char *url;                 // 固件 .bin 的 HTTPS/HTTP URL
    const char *cert_pem;            // HTTPS 根证书 PEM；HTTP 可填 NULL，但生产环境强烈建议 HTTPS
    int timeout_ms;                  // HTTP 超时，0 使用默认 10000ms
    bool skip_cert_common_name_check;// 调试用；生产环境必须 false
    bool reboot_after_success;       // 成功后是否自动 esp_restart()
    ota_update_callback_t callback;  // 可选进度/结果回调
    void *user_ctx;                  // 用户上下文
} ota_update_config_t;

/**
 * @brief 同步执行 OTA。调用前请确保 Wi-Fi 已连接且 SNTP 时间已同步（HTTPS 证书校验需要）。
 */
esp_err_t ota_update_start(const ota_update_config_t *config);

/**
 * @brief 新固件首次启动后调用。开启 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 时很重要。
 *        自检成功会标记当前固件有效；自检失败会回滚并重启。
 */
esp_err_t ota_update_confirm_app(bool self_test_ok);

/**
 * @brief 打印当前固件版本、工程名、编译时间和运行分区。
 */
void ota_update_print_app_info(void);

/**
 * @brief 在后台任务中简化启动 OTA 下载并在成功后重启（便于在 main 中直接调用）
 *
 * 简单用法示例：
 *     ota_update_start_bg("https://example.com/firmware.bin", NULL);
 *
 * 注意：生产环境请传入根证书 cert_pem 并将 skip_cert_common_name_check 设为 false。
 */
void ota_update_start_bg(const char *url, const char *cert_pem);

#ifdef __cplusplus
}
#endif
