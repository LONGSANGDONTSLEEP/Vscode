#include "pet_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "gpio_drv.h"
#include "hwinit.h"
#include "ota_update.h"
#include "pet_behavior.h"
#include "pet_collar_monitor.h"
#include "pet_config.h"
#include "pet_time.h"
#include "qmi8658a.h"
#include "wifi_manager.h"

#define TAG "PET_APP"

#define PET_APP_OTA_BUTTON_GPIO GPIO_NUM_0
#define PET_APP_REMOTE_OTA_STACK_SIZE 6144U
#define PET_APP_REMOTE_OTA_TASK_PRIORITY 3U
#define PET_APP_OTA_HTTP_TIMEOUT_MS 3000U
#define PET_APP_OTA_URL_MAX_LEN 256U
#define PET_APP_OTA_ID_MAX_LEN 64U
#define PET_APP_RECORD_ID_MAX_LEN 64U
#define PET_APP_RECORD_HTTP_TIMEOUT_MS 1500U
#define PET_APP_REMOTE_RECORD_STACK_SIZE 4096U
#define PET_APP_REMOTE_RECORD_TASK_PRIORITY 3U

#define PET_APP_WIFI_TIMEOUT_MS 15000U
#define PET_APP_NTP_TIMEOUT_MS 10000U
#define PET_APP_HTTP_TIMEOUT_MS 2000U
#define PET_APP_HEARTBEAT_PERIOD_MS 30000U
#define PET_APP_MONITOR_SAMPLE_PERIOD_MS 20U
#define PET_APP_MONITOR_STACK_SIZE 4096U
#define PET_APP_MONITOR_TASK_PRIORITY 5U
#define PET_APP_HEARTBEAT_STACK_SIZE 3072U
#define PET_APP_HEARTBEAT_TASK_PRIORITY 2U

static pet_config_t s_app_cfg;
static TaskHandle_t s_heartbeat_task;
static TaskHandle_t s_remote_ota_task;
static TaskHandle_t s_remote_record_task;
static SemaphoreHandle_t s_ota_lock;
static volatile bool s_ota_in_progress;
static bool s_started;
static char s_last_ota_id[PET_APP_OTA_ID_MAX_LEN];
static char s_last_record_id[PET_APP_RECORD_ID_MAX_LEN];

typedef struct {
    char id[PET_APP_OTA_ID_MAX_LEN];
    char url[PET_APP_OTA_URL_MAX_LEN];
} pet_app_ota_job_t;

static void pet_app_print_stack(const char *tag)
{
    ESP_LOGI(tag,
             "stack high water mark: %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static bool pet_app_connect_wifi_for_telemetry(void)
{
    ESP_LOGI(TAG, "connecting WiFi for telemetry...");

    if (!wifi_manager_connect_blocking(s_app_cfg.wifi_ssid,
                                       s_app_cfg.wifi_pass,
                                       PET_APP_WIFI_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "WiFi connect failed, telemetry will be unavailable");
        return false;
    }

    ESP_LOGI(TAG, "WiFi connected for telemetry");

    esp_err_t ret = pet_time_sync_ntp(PET_APP_NTP_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NTP time sync success");
    } else {
        ESP_LOGW(TAG, "NTP time sync failed: %s", esp_err_to_name(ret));
    }

    return true;
}

static void copy_json_string_value(const char *json,
                                   const char *key,
                                   char *out,
                                   size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return;
    }

    out[0] = '\0';

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) {
        return;
    }

    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return;
    }

    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '\"') {
        return;
    }
    p++;

    size_t n = 0;
    while (*p && *p != '\"' && n + 1 < out_size) {
        out[n++] = *p++;
    }
    out[n] = '\0';
}

static bool json_bool_or_int_true(const char *json, const char *key)
{
    if (!json || !key) {
        return false;
    }

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }

    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }

    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    return (*p == '1') || (strncmp(p, "true", 4) == 0) || (strncmp(p, "TRUE", 4) == 0);
}

static bool json_has_key(const char *json, const char *key)
{
    if (!json || !key) {
        return false;
    }

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern) != NULL;
}

static uint32_t json_u32_value(const char *json, const char *key, uint32_t default_value)
{
    if (!json || !key) {
        return default_value;
    }

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) {
        return default_value;
    }

    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return default_value;
    }

    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) {
        return default_value;
    }

    return (uint32_t)v;
}

static esp_err_t pet_app_get_ota_command(char *id,
                                         size_t id_size,
                                         char *url,
                                         size_t url_size)
{
    if (!id || !url || id_size == 0 || url_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    id[0] = '\0';
    url[0] = '\0';

    esp_http_client_config_t config = {
        .url = s_app_cfg.ota_command_url,
        .timeout_ms = PET_APP_OTA_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "remote OTA: http client init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "remote OTA: command open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "remote OTA: command HTTP status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char body[384];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total);
        if (r < 0) {
            ESP_LOGW(TAG, "remote OTA: command read failed: %d", r);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += r;
        if (content_length > 0 && total >= content_length) {
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total <= 0) {
        return ESP_OK;
    }

    body[total] = '\0';

    if (!json_bool_or_int_true(body, "ota")) {
        return ESP_OK;
    }

    copy_json_string_value(body, "id", id, id_size);
    copy_json_string_value(body, "url", url, url_size);

    if (url[0] == '\0') {
        ESP_LOGW(TAG, "remote OTA: ota=1 but url is empty, body=%s", body);
        return ESP_OK;
    }

    if (id[0] == '\0') {
        snprintf(id, id_size, "no-id");
    }

    return ESP_OK;
}

static void pet_app_ota_job_task(void *arg)
{
    pet_app_ota_job_t *job = (pet_app_ota_job_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "OTA task running: id=%s url=%s", job->id, job->url);

    ota_update_config_t cfg = {
        .url = job->url,
        .cert_pem = NULL,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
        .reboot_after_success = true,
        .callback = NULL,
        .user_ctx = NULL,
    };

    esp_err_t ret = ota_update_start(&cfg);

    /*
     * OTA 成功时 ota_update_start() 会重启设备，一般不会走到这里。
     * 如果走到这里，说明 OTA 失败或没有自动重启，需要允许之后重新触发。
     */
    ESP_LOGW(TAG, "OTA task finished without reboot, ret=%s", esp_err_to_name(ret));

    if (s_ota_lock && xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_lock);
    } else {
        s_ota_in_progress = false;
    }

    free(job);
    vTaskDelete(NULL);
}

static void pet_app_start_ota_url(const char *url, const char *id)
{
    if (!url || url[0] == '\0') {
        return;
    }

    if (s_ota_lock && xSemaphoreTake(s_ota_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "OTA lock busy, ignore command");
        return;
    }

    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, ignore command id=%s", id ? id : "");
        if (s_ota_lock) {
            xSemaphoreGive(s_ota_lock);
        }
        return;
    }

    s_ota_in_progress = true;

    if (id && id[0] != '\0') {
        snprintf(s_last_ota_id, sizeof(s_last_ota_id), "%s", id);
    }

    if (s_ota_lock) {
        xSemaphoreGive(s_ota_lock);
    }

    pet_app_ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        ESP_LOGE(TAG, "OTA job alloc failed");
        s_ota_in_progress = false;
        return;
    }

    snprintf(job->url, sizeof(job->url), "%s", url);
    snprintf(job->id, sizeof(job->id), "%s", id ? id : "");

    ESP_LOGW(TAG, "REMOTE OTA START: id=%s url=%s", job->id, job->url);

    BaseType_t ok = xTaskCreate(pet_app_ota_job_task,
                                "ota_job",
                                8192,
                                job,
                                PET_APP_REMOTE_OTA_TASK_PRIORITY + 2,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "OTA job task create failed");
        free(job);
        s_ota_in_progress = false;
    }
}

static void pet_app_trigger_ota(void)
{
    char id[PET_APP_OTA_ID_MAX_LEN];
    char url[PET_APP_OTA_URL_MAX_LEN];

    ESP_LOGI(TAG, "manual OTA button pressed, checking command url: %s", s_app_cfg.ota_command_url);

    if (!wifi_manager_connect_blocking(s_app_cfg.wifi_ssid,
                                       s_app_cfg.wifi_pass,
                                       PET_APP_WIFI_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "WiFi not connected, abort OTA");
        return;
    }

    if (pet_app_get_ota_command(id, sizeof(id), url, sizeof(url)) == ESP_OK && url[0] != '\0') {
        pet_app_start_ota_url(url, id);
    } else {
        ESP_LOGW(TAG, "manual OTA: no OTA command available");
    }
}

static void pet_app_ota_button_cb(gpio_num_t gpio, uint32_t level)
{
    (void)gpio;

    if (level == 0) {
        ESP_LOGI(TAG, "OTA button pressed");
        pet_app_trigger_ota();
    }
}

static esp_err_t pet_app_register_ota_button(void)
{
    esp_err_t ret = gpio_drv_register_callback(PET_APP_OTA_BUTTON_GPIO,
                                               pet_app_ota_button_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA button register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OTA button registered on GPIO%d", PET_APP_OTA_BUTTON_GPIO);
    return ESP_OK;
}

static void pet_app_remote_ota_task(void *arg)
{
    (void)arg;

    char id[PET_APP_OTA_ID_MAX_LEN];
    char url[PET_APP_OTA_URL_MAX_LEN];

    uint32_t poll_ms = s_app_cfg.ota_command_poll_ms;
    if (poll_ms < 3000) {
        poll_ms = 3000;
    }

    ESP_LOGI(TAG,
             "remote OTA command polling started: url=%s, interval=%lu ms",
             s_app_cfg.ota_command_url,
             (unsigned long)poll_ms);

    while (1) {
        if (!s_ota_in_progress) {
            esp_err_t ret = pet_app_get_ota_command(id, sizeof(id), url, sizeof(url));
            if (ret == ESP_OK && url[0] != '\0') {
                if (id[0] != '\0' && strcmp(id, s_last_ota_id) == 0) {
                    ESP_LOGI(TAG, "remote OTA command already handled: id=%s", id);
                } else {
                    pet_app_start_ota_url(url, id);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

static esp_err_t pet_app_start_remote_ota(bool wifi_ok)
{
    if (!wifi_ok) {
        ESP_LOGW(TAG, "remote OTA disabled because WiFi is not connected");
        return ESP_OK;
    }

    if (!s_app_cfg.enable_remote_ota) {
        ESP_LOGI(TAG, "remote OTA disabled by config");
        return ESP_OK;
    }

    if (s_app_cfg.ota_command_url[0] == '\0') {
        ESP_LOGW(TAG, "remote OTA disabled: empty OTA_COMMAND_URL");
        return ESP_OK;
    }

    if (!s_ota_lock) {
        s_ota_lock = xSemaphoreCreateMutex();
        if (!s_ota_lock) {
            ESP_LOGE(TAG, "remote OTA mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_remote_ota_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(pet_app_remote_ota_task,
                                "remote_ota",
                                PET_APP_REMOTE_OTA_STACK_SIZE,
                                NULL,
                                PET_APP_REMOTE_OTA_TASK_PRIORITY,
                                &s_remote_ota_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "remote OTA task create failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t pet_app_get_record_command(char *id,
                                            size_t id_size,
                                            bool *enabled,
                                            uint32_t *sample_period_ms,
                                            bool *raw_log)
{
    if (!id || id_size == 0 || !enabled || !sample_period_ms || !raw_log) {
        return ESP_ERR_INVALID_ARG;
    }

    id[0] = '\0';
    *enabled = false;
    *sample_period_ms = 5;
    *raw_log = true;

    esp_http_client_config_t config = {
        .url = s_app_cfg.record_command_url,
        .timeout_ms = PET_APP_RECORD_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "record cmd: http client init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "record cmd: open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "record cmd: HTTP status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char body[384];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total);
        if (r < 0) {
            ESP_LOGW(TAG, "record cmd: read failed: %d", r);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += r;
        if (content_length > 0 && total >= content_length) {
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total <= 0) {
        return ESP_OK;
    }

    body[total] = '\0';

    if (!json_has_key(body, "record")) {
        return ESP_OK;
    }

    copy_json_string_value(body, "id", id, id_size);
    if (id[0] == '\0') {
        snprintf(id, id_size, "no-id");
    }

    *enabled = json_bool_or_int_true(body, "record");
    *sample_period_ms = json_u32_value(body, "sample_period_ms", 5);
    *raw_log = json_has_key(body, "raw") ? json_bool_or_int_true(body, "raw") : true;

    return ESP_OK;
}

static void pet_app_remote_record_task(void *arg)
{
    (void)arg;

    char id[PET_APP_RECORD_ID_MAX_LEN];
    bool enabled = false;
    bool raw_log = true;
    uint32_t sample_period_ms = 5;

    uint32_t poll_ms = s_app_cfg.record_command_poll_ms;
    if (poll_ms < 1000) {
        poll_ms = 1000;
    }

    ESP_LOGI(TAG,
             "remote record polling started: url=%s, interval=%lu ms",
             s_app_cfg.record_command_url,
             (unsigned long)poll_ms);

    while (1) {
        esp_err_t ret = pet_app_get_record_command(id,
                                                   sizeof(id),
                                                   &enabled,
                                                   &sample_period_ms,
                                                   &raw_log);
        if (ret == ESP_OK && id[0] != '\0') {
            if (strcmp(id, s_last_record_id) != 0) {
                snprintf(s_last_record_id, sizeof(s_last_record_id), "%s", id);
                ESP_LOGW(TAG,
                         "record command: id=%s enabled=%d sample_period=%lu raw=%d",
                         id,
                         enabled ? 1 : 0,
                         (unsigned long)sample_period_ms,
                         raw_log ? 1 : 0);
                pet_collar_monitor_set_recording_mode(enabled, sample_period_ms, raw_log);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

static esp_err_t pet_app_start_remote_record(bool wifi_ok)
{
    if (!wifi_ok) {
        ESP_LOGW(TAG, "remote record disabled because WiFi is not connected");
        return ESP_OK;
    }

    if (!s_app_cfg.enable_remote_record) {
        ESP_LOGI(TAG, "remote record disabled by config");
        return ESP_OK;
    }

    if (s_app_cfg.record_command_url[0] == '\0') {
        ESP_LOGW(TAG, "remote record disabled: empty RECORD_COMMAND_URL");
        return ESP_OK;
    }

    if (s_remote_record_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(pet_app_remote_record_task,
                                "remote_record",
                                PET_APP_REMOTE_RECORD_STACK_SIZE,
                                NULL,
                                PET_APP_REMOTE_RECORD_TASK_PRIORITY,
                                &s_remote_record_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "remote record task create failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t pet_app_start_monitor(bool wifi_ok)
{
    const bool enable_json = wifi_ok && s_app_cfg.enable_json_upload;
    const bool enable_file = wifi_ok && s_app_cfg.enable_file_upload;

    pet_collar_monitor_config_t cfg = {
        .bus = hwinit_get_i2c_bus(),
        .qmi8658a_addr = QMI8658A_I2C_ADDR_LOW,
        .sample_period_ms = PET_APP_MONITOR_SAMPLE_PERIOD_MS,
        .task_stack_size = PET_APP_MONITOR_STACK_SIZE,
        .task_priority = PET_APP_MONITOR_TASK_PRIORITY,
        .enable_gyro_calibration = true,

        .enable_http_upload = enable_json,
        .http_url = s_app_cfg.pet_http_url,
        .http_timeout_ms = PET_APP_HTTP_TIMEOUT_MS,

        .enable_file_upload = enable_file,
        .file_upload_url = s_app_cfg.pet_file_upload_url,
        .file_upload_scan_interval_ms = s_app_cfg.file_upload_scan_ms,
    };

    ESP_LOGI(TAG,
             "upload config: json=%d, file=%d",
             enable_json ? 1 : 0,
             enable_file ? 1 : 0);

    esp_err_t ret = pet_collar_monitor_start(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pet monitor start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "pet monitor started");
    return ESP_OK;
}

static void pet_app_heartbeat_task(void *arg)
{
    (void)arg;

    while (1) {
        pet_behavior_result_t r;
        if (pet_collar_monitor_get_last_result(&r)) {
            ESP_LOGI(TAG,
                     "alive, state=%s, stack=%u",
                     pet_state_to_str(r.state),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else {
            ESP_LOGI(TAG,
                     "alive, stack=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(PET_APP_HEARTBEAT_PERIOD_MS));
    }
}

static esp_err_t pet_app_start_heartbeat(void)
{
    if (s_heartbeat_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(pet_app_heartbeat_task,
                                "pet_app_alive",
                                PET_APP_HEARTBEAT_STACK_SIZE,
                                NULL,
                                PET_APP_HEARTBEAT_TASK_PRIORITY,
                                &s_heartbeat_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

static void pet_app_load_config(void)
{
    esp_err_t ret = pet_config_load_or_create(&s_app_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "pet config load failed: %s, use defaults",
                 esp_err_to_name(ret));
        pet_config_set_defaults(&s_app_cfg);
    }

    pet_config_print(&s_app_cfg);
}

esp_err_t pet_app_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "application start");
    pet_app_print_stack(TAG);

    pet_time_init();
    pet_app_load_config();

    bool wifi_ok = pet_app_connect_wifi_for_telemetry();

    esp_err_t monitor_ret = pet_app_start_monitor(wifi_ok);
    esp_err_t remote_ota_ret = pet_app_start_remote_ota(wifi_ok);
    esp_err_t remote_record_ret = pet_app_start_remote_record(wifi_ok);
    esp_err_t button_ret = pet_app_register_ota_button();
    esp_err_t heartbeat_ret = pet_app_start_heartbeat();

    ESP_LOGI(TAG, "Bluetooth OTA disabled");
    ESP_LOGI(TAG, "Use BOOT key for WiFi OTA");

    s_started = true;

    if (monitor_ret != ESP_OK) {
        return monitor_ret;
    }
    if (remote_ota_ret != ESP_OK) {
        return remote_ota_ret;
    }
    if (remote_record_ret != ESP_OK) {
        return remote_record_ret;
    }
    if (button_ret != ESP_OK) {
        return button_ret;
    }
    return heartbeat_ret;
}
