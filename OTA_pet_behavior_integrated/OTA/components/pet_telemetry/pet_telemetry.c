#include "pet_telemetry.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pet_time.h"

#define TAG "PET_HTTP"

#define PET_TELEMETRY_URL_MAX_LEN   160
#define PET_TELEMETRY_JSON_MAX_LEN  512

typedef struct {
    uint32_t now_ms;
    pet_behavior_result_t result;
} pet_telemetry_msg_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static bool s_ready = false;
static char s_url[PET_TELEMETRY_URL_MAX_LEN];
static uint32_t s_timeout_ms = 2000;

static esp_err_t post_json(const char *json)
{
    esp_http_client_config_t config = {
        .url = s_url,
        .timeout_ms = (int)s_timeout_ms,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "http client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t ret = esp_http_client_perform(client);

    if (ret == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int len = esp_http_client_get_content_length(client);

        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "POST OK, status=%d, len=%d", status, len);
        } else {
            ESP_LOGW(TAG, "POST failed, status=%d, len=%d", status, len);
            ret = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "POST error: %s", esp_err_to_name(ret));
    }

    esp_http_client_cleanup(client);
    return ret;
}

static void make_json(uint32_t now_ms,
                      const pet_behavior_result_t *r,
                      char *buf,
                      size_t buf_len)
{
    (void)now_ms;

    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    char event_buf[96];
    pet_events_to_str(r->events, event_buf, sizeof(event_buf));

    snprintf(buf,
             buf_len,
             "{"
             "\"boot_id\":\"%08lx\","
             "\"time_ms\":%llu,"
             "\"epoch_ms\":%lld,"
             "\"time_valid\":%d,"
             "\"time_str\":\"%s\","
             "\"state\":\"%s\","
             "\"candidate\":\"%s\","
             "\"event\":\"%s\","
             "\"acc\":%.3f,"
             "\"gyro\":%.2f,"
             "\"acc_std\":%.3f,"
             "\"gyro_std\":%.2f,"
             "\"pitch\":%.1f,"
             "\"roll\":%.1f,"
             "\"state_duration_ms\":%lu,"
             "\"rest_like_ms\":%lu"
             "}",
             (unsigned long)ts.boot_id,
             (unsigned long long)ts.time_ms,
             (long long)ts.epoch_ms,
             ts.time_valid ? 1 : 0,
             ts.time_str,
             pet_state_to_str(r->state),
             pet_state_to_str(r->candidate_state),
             event_buf,
             r->acc_norm_g,
             r->gyro_norm_dps,
             r->acc_norm_std,
             r->gyro_norm_std,
             r->pitch_deg,
             r->roll_deg,
             (unsigned long)r->state_duration_ms,
             (unsigned long)r->rest_like_duration_ms);
}

static void telemetry_task(void *arg)
{
    pet_telemetry_msg_t msg;
    char json[PET_TELEMETRY_JSON_MAX_LEN];

    ESP_LOGI(TAG, "telemetry task started, url=%s", s_url);

    while (1) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
            make_json(msg.now_ms, &msg.result, json, sizeof(json));
            post_json(json);
        }
    }
}

esp_err_t pet_telemetry_start(const pet_telemetry_config_t *cfg)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (cfg == NULL || cfg->url == NULL || cfg->url[0] == '\0') {
        ESP_LOGW(TAG, "invalid telemetry url");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_url, sizeof(s_url), "%s", cfg->url);

    uint32_t queue_size = cfg->queue_size ? cfg->queue_size : 8;
    uint32_t stack_size = cfg->task_stack_size ? cfg->task_stack_size : 6144;
    uint32_t priority = cfg->task_priority ? cfg->task_priority : 4;
    s_timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 2000;

    s_queue = xQueueCreate(queue_size, sizeof(pet_telemetry_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(
        telemetry_task,
        "pet_http",
        stack_size,
        NULL,
        priority,
        &s_task
    );

    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }

    s_ready = true;

    ESP_LOGI(TAG, "telemetry started");
    return ESP_OK;
}

esp_err_t pet_telemetry_stop(void)
{
    s_ready = false;

    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    ESP_LOGI(TAG, "telemetry stopped");
    return ESP_OK;
}

bool pet_telemetry_is_ready(void)
{
    return s_ready;
}

esp_err_t pet_telemetry_enqueue_state(uint32_t now_ms, const pet_behavior_result_t *result)
{
    if (!s_ready || s_queue == NULL || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    pet_telemetry_msg_t msg = {
        .now_ms = now_ms,
        .result = *result,
    };

    /*
     * 不阻塞主监测任务。
     * 队列满了就丢弃本次上传，SD 卡仍然有完整记录。
     */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "telemetry queue full, drop one state");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}