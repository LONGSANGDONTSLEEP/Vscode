#include "pet_time.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "PET_TIME"

static bool s_inited = false;
static bool s_sntp_inited = false;
static bool s_time_valid = false;
static uint32_t s_boot_id = 0;

static bool system_time_looks_valid(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /*
     * 2021-01-01 00:00:00 UTC = 1609459200
     * 如果系统时间大于这个值，基本可以认为已经校时。
     */
    return tv.tv_sec > 1609459200;
}

esp_err_t pet_time_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_boot_id = esp_random();
    if (s_boot_id == 0) {
        s_boot_id = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
    }

    /*
     * 中国时区：UTC+8。
     * 如果你的测试环境不在中国，后面可以改这里。
     */
    setenv("TZ", "CST-8", 1);
    tzset();

    s_time_valid = system_time_looks_valid();
    s_inited = true;

    ESP_LOGI(TAG,
             "time init done, boot_id=%08lx, time_valid=%d",
             (unsigned long)s_boot_id,
             s_time_valid ? 1 : 0);

    return ESP_OK;
}

esp_err_t pet_time_sync_ntp(uint32_t timeout_ms)
{
    pet_time_init();

    if (!s_sntp_inited) {
        ESP_LOGI(TAG, "init SNTP");

        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

        esp_err_t ret = esp_netif_sntp_init(&config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        s_sntp_inited = true;
    }

    ESP_LOGI(TAG, "waiting for SNTP sync...");

    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));

    if (ret == ESP_OK || system_time_looks_valid()) {
        s_time_valid = true;

        pet_time_snapshot_t snap;
        pet_time_get_snapshot(&snap);

        ESP_LOGI(TAG,
                 "time synced, epoch_ms=%lld, time_str=%s",
                 (long long)snap.epoch_ms,
                 snap.time_str);

        return ESP_OK;
    }

    s_time_valid = false;

    ESP_LOGW(TAG, "SNTP sync failed: %s", esp_err_to_name(ret));
    return ret;
}

void pet_time_get_snapshot(pet_time_snapshot_t *out)
{
    if (!out) {
        return;
    }

    pet_time_init();

    memset(out, 0, sizeof(*out));

    out->boot_id = s_boot_id;
    out->time_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    out->time_valid = s_time_valid;

    if (!s_time_valid && !system_time_looks_valid()) {
        out->epoch_ms = 0;
        out->time_str[0] = '\0';
        return;
    }

    s_time_valid = true;
    out->time_valid = true;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    out->epoch_ms = ((int64_t)tv.tv_sec * 1000LL) + ((int64_t)tv.tv_usec / 1000LL);

    time_t now = tv.tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    strftime(out->time_str, sizeof(out->time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
}

bool pet_time_is_valid(void)
{
    if (s_time_valid) {
        return true;
    }

    s_time_valid = system_time_looks_valid();
    return s_time_valid;
}