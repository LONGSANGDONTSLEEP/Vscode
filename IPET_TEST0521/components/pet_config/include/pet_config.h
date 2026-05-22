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

    /*
     * 远程 OTA 命令地址。
     * 设备会定时 GET 这个 URL。服务器返回 {"ota":1,"url":"..."} 时，
     * 设备开始下载 url 指向的固件 .bin。
     */
    char ota_command_url[PET_CONFIG_URL_MAX_LEN];

    bool enable_json_upload;
    bool enable_file_upload;

    /* 是否允许通过服务器命令触发 OTA。 */
    bool enable_remote_ota;

    uint32_t file_upload_scan_ms;

    /* 远程 OTA 命令轮询间隔，建议 3000~10000ms。 */
    uint32_t ota_command_poll_ms;
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