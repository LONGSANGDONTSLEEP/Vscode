#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t boot_id;       // 每次开机生成一个随机 ID
    uint64_t time_ms;       // 开机后的毫秒数，永远有效
    int64_t epoch_ms;       // 真实 Unix 毫秒时间戳，未校时时为 0
    bool time_valid;        // true 表示已经 NTP/BLE/其他方式校时
    char time_str[32];      // "YYYY-MM-DD HH:MM:SS"，未校时时为空字符串
} pet_time_snapshot_t;

/**
 * @brief 初始化时间模块。
 *
 * 不需要 Wi-Fi。
 * 会生成 boot_id，并设置时区。
 */
esp_err_t pet_time_init(void);

/**
 * @brief Wi-Fi 连上后，使用 NTP 同步真实时间。
 *
 * @param timeout_ms 等待同步的超时时间，例如 10000。
 */
esp_err_t pet_time_sync_ntp(uint32_t timeout_ms);

/**
 * @brief 获取当前时间快照。
 */
void pet_time_get_snapshot(pet_time_snapshot_t *out);

/**
 * @brief 当前真实时间是否有效。
 */
bool pet_time_is_valid(void);

#ifdef __cplusplus
}
#endif