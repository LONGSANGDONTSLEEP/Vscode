#include "pet_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sd_card_mgr.h"

#define TAG "PET_CFG"

#define CONFIG_FILE_NAME "CONFIG.TXT"

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    snprintf(dst, dst_size, "%s", src ? src : "");
}

static char *trim(char *s)
{
    if (!s) {
        return s;
    }

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

static bool str_eq_ignore_case(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }

    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static bool parse_bool_value(const char *value, bool default_value)
{
    if (!value) {
        return default_value;
    }

    if (str_eq_ignore_case(value, "1") ||
        str_eq_ignore_case(value, "true") ||
        str_eq_ignore_case(value, "yes") ||
        str_eq_ignore_case(value, "on")) {
        return true;
    }

    if (str_eq_ignore_case(value, "0") ||
        str_eq_ignore_case(value, "false") ||
        str_eq_ignore_case(value, "no") ||
        str_eq_ignore_case(value, "off")) {
        return false;
    }

    return default_value;
}

static uint32_t parse_u32_value(const char *value, uint32_t default_value)
{
    if (!value || value[0] == '\0') {
        return default_value;
    }

    char *end = NULL;
    unsigned long v = strtoul(value, &end, 10);

    if (end == value) {
        return default_value;
    }

    return (uint32_t)v;
}

void pet_config_set_defaults(pet_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    copy_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "TYZX");
    copy_str(cfg->wifi_pass, sizeof(cfg->wifi_pass), "ty20260101");

    copy_str(cfg->pet_http_url,
             sizeof(cfg->pet_http_url),
             "http://192.168.1.12:8080/pet");

    copy_str(cfg->pet_file_upload_url,
             sizeof(cfg->pet_file_upload_url),
             "http://192.168.1.12:8080/upload");

    cfg->enable_json_upload = true;
    cfg->enable_file_upload = true;
    cfg->file_upload_scan_ms = 60000;
}

static esp_err_t write_default_config_file(const char *path, const pet_config_t *cfg)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "create config failed: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    fprintf(f, "# Pet collar config\n");
    fprintf(f, "# Edit this file on SD card, then reboot device.\n");
    fprintf(f, "\n");

    fprintf(f, "WIFI_SSID=%s\n", cfg->wifi_ssid);
    fprintf(f, "WIFI_PASS=%s\n", cfg->wifi_pass);
    fprintf(f, "\n");

    fprintf(f, "PET_HTTP_URL=%s\n", cfg->pet_http_url);
    fprintf(f, "PET_FILE_UPLOAD_URL=%s\n", cfg->pet_file_upload_url);
    fprintf(f, "\n");

    fprintf(f, "ENABLE_JSON_UPLOAD=%d\n", cfg->enable_json_upload ? 1 : 0);
    fprintf(f, "ENABLE_FILE_UPLOAD=%d\n", cfg->enable_file_upload ? 1 : 0);
    fprintf(f, "FILE_UPLOAD_SCAN_MS=%lu\n", (unsigned long)cfg->file_upload_scan_ms);

    fclose(f);

    ESP_LOGI(TAG, "default config created: %s", path);
    return ESP_OK;
}

static void apply_key_value(pet_config_t *cfg, const char *key, const char *value)
{
    if (!cfg || !key || !value) {
        return;
    }

    if (str_eq_ignore_case(key, "WIFI_SSID")) {
        copy_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), value);
    } else if (str_eq_ignore_case(key, "WIFI_PASS")) {
        copy_str(cfg->wifi_pass, sizeof(cfg->wifi_pass), value);
    } else if (str_eq_ignore_case(key, "PET_HTTP_URL")) {
        copy_str(cfg->pet_http_url, sizeof(cfg->pet_http_url), value);
    } else if (str_eq_ignore_case(key, "PET_FILE_UPLOAD_URL")) {
        copy_str(cfg->pet_file_upload_url, sizeof(cfg->pet_file_upload_url), value);
    } else if (str_eq_ignore_case(key, "ENABLE_JSON_UPLOAD")) {
        cfg->enable_json_upload = parse_bool_value(value, cfg->enable_json_upload);
    } else if (str_eq_ignore_case(key, "ENABLE_FILE_UPLOAD")) {
        cfg->enable_file_upload = parse_bool_value(value, cfg->enable_file_upload);
    } else if (str_eq_ignore_case(key, "FILE_UPLOAD_SCAN_MS")) {
        cfg->file_upload_scan_ms = parse_u32_value(value, cfg->file_upload_scan_ms);
    } else {
        ESP_LOGW(TAG, "unknown config key: %s", key);
    }
}

esp_err_t pet_config_load_or_create(pet_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    pet_config_set_defaults(cfg);

    if (!sd_card_mgr_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, use default config");
        return ESP_OK;
    }

    char path[96];
    int len = snprintf(path,
                       sizeof(path),
                       "%s/%s",
                       sd_card_mgr_mount_point(),
                       CONFIG_FILE_NAME);
    if (len < 0 || len >= (int)sizeof(path)) {
        ESP_LOGE(TAG, "config path too long");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "config not found, create default: %s", path);
        return write_default_config_file(path, cfg);
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);

        if (p[0] == '\0') {
            continue;
        }

        if (p[0] == '#') {
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) {
            ESP_LOGW(TAG, "invalid config line: %s", p);
            continue;
        }

        *eq = '\0';

        char *key = trim(p);
        char *value = trim(eq + 1);

        /*
         * 去掉 Windows CRLF 里的 \r。
         */
        size_t vlen = strlen(value);
        if (vlen > 0 && value[vlen - 1] == '\r') {
            value[vlen - 1] = '\0';
        }

        apply_key_value(cfg, key, value);
    }

    fclose(f);

    ESP_LOGI(TAG, "config loaded: %s", path);
    return ESP_OK;
}

void pet_config_print(const pet_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    ESP_LOGI(TAG, "wifi_ssid=%s", cfg->wifi_ssid);
    ESP_LOGI(TAG, "wifi_pass=%s", cfg->wifi_pass);
    ESP_LOGI(TAG, "pet_http_url=%s", cfg->pet_http_url);
    ESP_LOGI(TAG, "pet_file_upload_url=%s", cfg->pet_file_upload_url);
    ESP_LOGI(TAG, "enable_json_upload=%d", cfg->enable_json_upload ? 1 : 0);
    ESP_LOGI(TAG, "enable_file_upload=%d", cfg->enable_file_upload ? 1 : 0);
    ESP_LOGI(TAG, "file_upload_scan_ms=%lu", (unsigned long)cfg->file_upload_scan_ms);
}