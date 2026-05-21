#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_CONFIG_WIFI_SSID_MAX_LEN        64
#define PET_CONFIG_WIFI_PASS_MAX_LEN        64
#define PET_CONFIG_URL_MAX_LEN              160

typedef struct {
    char wifi_ssid[PET_CONFIG_WIFI_SSID_MAX_LEN];
    char wifi_pass[PET_CONFIG_WIFI_PASS_MAX_LEN];

    char pet_http_url[PET_CONFIG_URL_MAX_LEN];
    char pet_file_upload_url[PET_CONFIG_URL_MAX_LEN];

    bool enable_json_upload;
    bool enable_file_upload;

    uint32_t file_upload_scan_ms;
} pet_config_t;

/**
 * @brief 设置默认配置。
 */
void pet_config_set_defaults(pet_config_t *cfg);

/**
 * @brief 从 /sdcard/CONFIG.TXT 加载配置。
 *
 * 如果文件不存在，会自动创建一个默认模板，并继续使用默认配置。
 */
esp_err_t pet_config_load_or_create(pet_config_t *cfg);

/**
 * @brief 打印当前配置。
 */
void pet_config_print(const pet_config_t *cfg);

#ifdef __cplusplus
}
#endif