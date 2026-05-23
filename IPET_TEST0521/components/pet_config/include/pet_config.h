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

    /*
     * 远程录制命令地址。
     * 网页点击“开始录制”后，设备轮询这个 URL，进入高频采样 + raw 日志模式。
     */
    char record_command_url[PET_CONFIG_URL_MAX_LEN];

    /*
     * 远程 SD 卡管理命令地址。
     * 网页可通过它请求设备列出、上传或删除 /sdcard 文件。
     */
    char sd_command_url[PET_CONFIG_URL_MAX_LEN];

    bool enable_json_upload;
    bool enable_file_upload;

    /* 是否允许通过服务器命令触发 OTA。 */
    bool enable_remote_ota;

    /* 是否允许网页远程控制高频录制模式。 */
    bool enable_remote_record;

    /* 是否允许网页浏览、下载、删除 SD 卡文件。 */
    bool enable_remote_sd;

    uint32_t file_upload_scan_ms;

    /* /pet 当前状态上传间隔。建议 1000ms，避免网页显示“上次联系几秒前”。 */
    uint32_t json_upload_interval_ms;

    /* 远程 OTA 命令轮询间隔，建议 3000~10000ms。 */
    uint32_t ota_command_poll_ms;

    /* 远程录制命令轮询间隔，录制控制建议 1000~2000ms。 */
    uint32_t record_command_poll_ms;

    /* 远程 SD 卡管理命令轮询间隔。 */
    uint32_t sd_command_poll_ms;

    /* LED 配置。第一颗显示行为，第二颗显示系统/连接/录制/OTA 状态。 */
    uint32_t led_brightness;
    uint32_t system_led_period_ms;
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