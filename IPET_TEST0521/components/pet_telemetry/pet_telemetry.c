#include "pet_telemetry.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pet_time.h"

#define TAG "PET_HTTP"

#define PET_TELEMETRY_URL_MAX_LEN   160
#define PET_TELEMETRY_JSON_MAX_LEN  512

/*
 * 上传频率控制：
 *
 * REST / SLEEP：
 *     30 秒上传一次，省电省流量
 *
 * WALK / TROT / RUN / PLAY / PASSIVE_MOTION：
 *     5 秒上传一次，方便远程观察
 *
 * EVENT：
 *     最快 1 秒上传一次，避免 SHAKE 连续刷屏
 */
#define UPLOAD_INTERVAL_REST_MS     1000
#define UPLOAD_INTERVAL_ACTIVE_MS   1000
#define UPLOAD_INTERVAL_EVENT_MS    1000
#define UPLOAD_INTERVAL_UNKNOWN_MS  1000

/*
 * HTTP 失败退避：
 *
 * 第 1 次失败：5 秒后再试
 * 第 2 次失败：10 秒后再试
 * 第 3 次失败：20 秒后再试
 * 第 4 次失败：40 秒后再试
 * 后续最多：60 秒后再试
 */
#define BACKOFF_BASE_MS             1000
#define BACKOFF_MAX_MS              10000

typedef struct {
    uint32_t now_ms;
    pet_behavior_result_t result;
} pet_telemetry_msg_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static bool s_ready = false;

static char s_url[PET_TELEMETRY_URL_MAX_LEN];
static uint32_t s_timeout_ms = 2000;

static uint32_t s_consecutive_failures = 0;
static uint64_t s_next_attempt_ms = 0;

static uint64_t s_last_state_enqueue_ms = 0;
static uint64_t s_last_event_enqueue_ms = 0;

static uint64_t now_ms64(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t upload_interval_by_state(pet_state_t state)
{
    switch (state) {
    case PET_STATE_REST:
    case PET_STATE_SLEEP:
    case PET_STATE_NOT_WORN:
        return UPLOAD_INTERVAL_REST_MS;

    case PET_STATE_WALK:
    case PET_STATE_TROT:
    case PET_STATE_RUN:
    case PET_STATE_PLAY:
    case PET_STATE_PASSIVE_MOTION:
        return UPLOAD_INTERVAL_ACTIVE_MS;

    case PET_STATE_UNKNOWN:
    case PET_STATE_ABNORMAL_INACTIVE:
    default:
        return UPLOAD_INTERVAL_UNKNOWN_MS;
    }
}

static uint32_t calc_backoff_ms(uint32_t failures)
{
    if (failures == 0) {
        return 0;
    }

    uint32_t delay = BACKOFF_BASE_MS;

    /*
     * failures:
     * 1 -> 5s
     * 2 -> 10s
     * 3 -> 20s
     * 4 -> 40s
     * >=5 -> 60s cap
     */
    for (uint32_t i = 1; i < failures; ++i) {
        if (delay >= BACKOFF_MAX_MS / 2) {
            delay = BACKOFF_MAX_MS;
            break;
        }
        delay *= 2;
    }

    if (delay > BACKOFF_MAX_MS) {
        delay = BACKOFF_MAX_MS;
    }

    return delay;
}

static bool should_enqueue(uint64_t now_ms, const pet_behavior_result_t *result)
{
    if (!result) {
        return false;
    }

    /*
     * 当前网络处于退避期，不入队。
     * SD 卡仍然会保存完整数据，所以这里丢弃 HTTP 实时上传没关系。
     */
    if (s_next_attempt_ms != 0 && now_ms < s_next_attempt_ms) {
        return false;
    }

    /*
     * 有事件时，优先上传。
     * 但是也限制最小间隔，避免 SHAKE 连续触发时刷屏。
     */
    if (result->events != PET_EVENT_NONE) {
        if ((now_ms - s_last_event_enqueue_ms) >= UPLOAD_INTERVAL_EVENT_MS) {
            s_last_event_enqueue_ms = now_ms;
            return true;
        }

        return false;
    }

    /*
     * 没事件时，根据当前状态控制上传频率。
     */
    uint32_t interval = upload_interval_by_state(result->state);

    if ((now_ms - s_last_state_enqueue_ms) >= interval) {
        s_last_state_enqueue_ms = now_ms;
        return true;
    }

    return false;
}

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
            ESP_LOGI(TAG, "POST OK, status=%d, len=%d, json=%s", status, len, json);
            ret = ESP_OK;
        } else {
            ESP_LOGW(TAG, "POST HTTP status failed, status=%d, len=%d", status, len);
            ret = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "POST error: %s", esp_err_to_name(ret));
    }

    esp_http_client_cleanup(client);
    return ret;
}

static void update_backoff_after_post(esp_err_t ret)
{
    uint64_t now = now_ms64();

    if (ret == ESP_OK) {
        if (s_consecutive_failures > 0) {
            ESP_LOGI(TAG, "network recovered, reset backoff");
        }

        s_consecutive_failures = 0;
        s_next_attempt_ms = 0;
        return;
    }

    s_consecutive_failures++;

    uint32_t delay_ms = calc_backoff_ms(s_consecutive_failures);
    s_next_attempt_ms = now + delay_ms;

    ESP_LOGW(TAG,
             "POST failed, failures=%lu, retry after %lu ms",
             (unsigned long)s_consecutive_failures,
             (unsigned long)delay_ms);
}

static void make_json(uint32_t now_ms,
                      const pet_behavior_result_t *r,
                      char *buf,
                      size_t buf_len)
{
    (void)now_ms;

    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    /*
     * 实时终端/HTTP 只发送稳定后的当前状态。
     * candidate/raw_state/acc/gyro 等详细调试字段仍写入 SD CSV，避免终端刷屏。
     */
    snprintf(buf,
             buf_len,
             "{"
             "\"boot_id\":\"%08lx\","
             "\"time_ms\":%llu,"
             "\"epoch_ms\":%lld,"
             "\"time_valid\":%d,"
             "\"time_str\":\"%s\","
             "\"state\":\"%s\","
             "\"state_duration_ms\":%lu"
             "}",
             (unsigned long)ts.boot_id,
             (unsigned long long)ts.time_ms,
             (long long)ts.epoch_ms,
             ts.time_valid ? 1 : 0,
             ts.time_str,
             pet_state_to_str(r->state),
             (unsigned long)r->state_duration_ms);
}

static void telemetry_task(void *arg)
{
    pet_telemetry_msg_t msg;
    pet_telemetry_msg_t newer;
    char json[PET_TELEMETRY_JSON_MAX_LEN];

    ESP_LOGI(TAG, "telemetry task started, url=%s", s_url);

    while (1) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
            /*
             * 如果队列里堆了多条，只保留最新一条。
             * HTTP 是实时观察，不是历史补传；完整历史由 SD 卡负责。
             */
            while (xQueueReceive(s_queue, &newer, 0) == pdTRUE) {
                msg = newer;
            }

            uint64_t now = now_ms64();

            if (s_next_attempt_ms != 0 && now < s_next_attempt_ms) {
                continue;
            }

            make_json(msg.now_ms, &msg.result, json, sizeof(json));

            esp_err_t ret = post_json(json);
            update_backoff_after_post(ret);
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

    uint32_t queue_size = cfg->queue_size ? cfg->queue_size : 16;
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

    s_consecutive_failures = 0;
    s_next_attempt_ms = 0;
    s_last_state_enqueue_ms = 0;
    s_last_event_enqueue_ms = 0;

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

    s_consecutive_failures = 0;
    s_next_attempt_ms = 0;
    s_last_state_enqueue_ms = 0;
    s_last_event_enqueue_ms = 0;

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

    uint64_t t = now_ms64();

    if (!should_enqueue(t, result)) {
        return ESP_OK;
    }

    pet_telemetry_msg_t msg = {
        .now_ms = now_ms,
        .result = *result,
    };

    /*
     * 不阻塞主监测任务。
     * 队列满了就丢掉最旧的一条，保留最新状态。
     */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        pet_telemetry_msg_t old_msg;

        if (xQueueReceive(s_queue, &old_msg, 0) == pdTRUE) {
            if (xQueueSend(s_queue, &msg, 0) == pdTRUE) {
                ESP_LOGW(TAG, "telemetry queue full, drop oldest state");
                return ESP_OK;
            }
        }

        ESP_LOGW(TAG, "telemetry queue full, drop current state");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}