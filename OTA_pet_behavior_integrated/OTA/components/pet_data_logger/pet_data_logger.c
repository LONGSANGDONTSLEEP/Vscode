#include "pet_data_logger.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "sd_card_mgr.h"
#include "pet_time.h"

#define TAG "PET_LOG"

/*
 * 继续使用 8.3 短文件名，避免 FATFS 未开启长文件名时 fopen errno=22。
 */
#define PET_STATE_FILE_NAME "STATE.CSV"
#define PET_EVENT_FILE_NAME "EVENT.CSV"

static bool s_ready = false;
static char s_session_dir[160];
static char s_state_path[192];
static char s_event_path[192];

static esp_err_t mkdir_if_needed(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return ESP_OK;
    }

    if (mkdir(path, 0775) == 0) {
        ESP_LOGI(TAG, "mkdir: %s", path);
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "mkdir failed: %s, errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t ensure_csv_header(const char *path, const char *header)
{
    struct stat st;
    bool need_header = false;

    if (stat(path, &st) != 0) {
        need_header = true;
    } else if (st.st_size == 0) {
        need_header = true;
    }

    if (!need_header) {
        return ESP_OK;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG,
                 "open header file failed: %s, errno=%d",
                 path,
                 errno);
        return ESP_FAIL;
    }

    fprintf(f, "%s\n", header);
    fclose(f);

    ESP_LOGI(TAG, "CSV header written: %s", path);
    return ESP_OK;
}

static esp_err_t build_session_paths(void)
{
    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    const char *mount = sd_card_mgr_mount_point();

    /*
     * 如果 NTP 已经成功：
     *
     * /sdcard/D260521/T121130/77AFEBE2/STATE.CSV
     *
     * 如果还没有真实时间：
     *
     * /sdcard/NOTIME/77AFEBE2/STATE.CSV
     */

    if (ts.time_valid && ts.time_str[0] != '\0') {
        int year = 0;
        int mon = 0;
        int day = 0;
        int hour = 0;
        int min = 0;
        int sec = 0;

        int n = sscanf(ts.time_str,
                       "%d-%d-%d %d:%d:%d",
                       &year,
                       &mon,
                       &day,
                       &hour,
                       &min,
                       &sec);

        if (n == 6) {
            char date_dir[96];
            char time_dir[128];
            char boot_dir[160];

            /*
             * 每个目录名本身仍然是 8.3 短文件名：
             *
             * D260521
             * T121130
             * 77AFEBE2
             *
             * 但是完整路径会越来越长，所以缓冲区必须大一点。
             */
            int len = snprintf(date_dir,
                               sizeof(date_dir),
                               "%s/D%02d%02d%02d",
                               mount,
                               year % 100,
                               mon,
                               day);
            if (len < 0 || len >= (int)sizeof(date_dir)) {
                ESP_LOGE(TAG, "date_dir path too long");
                return ESP_FAIL;
            }

            len = snprintf(time_dir,
                           sizeof(time_dir),
                           "%s/T%02d%02d%02d",
                           date_dir,
                           hour,
                           min,
                           sec);
            if (len < 0 || len >= (int)sizeof(time_dir)) {
                ESP_LOGE(TAG, "time_dir path too long");
                return ESP_FAIL;
            }

            len = snprintf(boot_dir,
                           sizeof(boot_dir),
                           "%s/%08lx",
                           time_dir,
                           (unsigned long)ts.boot_id);
            if (len < 0 || len >= (int)sizeof(boot_dir)) {
                ESP_LOGE(TAG, "boot_dir path too long");
                return ESP_FAIL;
            }

            if (mkdir_if_needed(date_dir) != ESP_OK) {
                return ESP_FAIL;
            }

            if (mkdir_if_needed(time_dir) != ESP_OK) {
                return ESP_FAIL;
            }

            if (mkdir_if_needed(boot_dir) != ESP_OK) {
                return ESP_FAIL;
            }

            snprintf(s_session_dir, sizeof(s_session_dir), "%s", boot_dir);
        } else {
            ESP_LOGW(TAG, "time_str parse failed: %s", ts.time_str);
            goto no_time_path;
        }
    } else {
no_time_path:
        {
            char notime_dir[96];
            char boot_dir[160];

            int len = snprintf(notime_dir,
                               sizeof(notime_dir),
                               "%s/NOTIME",
                               mount);
            if (len < 0 || len >= (int)sizeof(notime_dir)) {
                ESP_LOGE(TAG, "notime_dir path too long");
                return ESP_FAIL;
            }

            len = snprintf(boot_dir,
                           sizeof(boot_dir),
                           "%s/%08lx",
                           notime_dir,
                           (unsigned long)ts.boot_id);
            if (len < 0 || len >= (int)sizeof(boot_dir)) {
                ESP_LOGE(TAG, "boot_dir path too long");
                return ESP_FAIL;
            }

            if (mkdir_if_needed(notime_dir) != ESP_OK) {
                return ESP_FAIL;
            }

            if (mkdir_if_needed(boot_dir) != ESP_OK) {
                return ESP_FAIL;
            }

            snprintf(s_session_dir, sizeof(s_session_dir), "%s", boot_dir);
        }
    }

    int len = snprintf(s_state_path,
                       sizeof(s_state_path),
                       "%s/%s",
                       s_session_dir,
                       PET_STATE_FILE_NAME);
    if (len < 0 || len >= (int)sizeof(s_state_path)) {
        ESP_LOGE(TAG, "state log path too long");
        return ESP_FAIL;
    }

    len = snprintf(s_event_path,
                   sizeof(s_event_path),
                   "%s/%s",
                   s_session_dir,
                   PET_EVENT_FILE_NAME);
    if (len < 0 || len >= (int)sizeof(s_event_path)) {
        ESP_LOGE(TAG, "event log path too long");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "session dir: %s", s_session_dir);
    ESP_LOGI(TAG, "state log path: %s", s_state_path);
    ESP_LOGI(TAG, "event log path: %s", s_event_path);

    return ESP_OK;
}

esp_err_t pet_data_logger_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (!sd_card_mgr_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, pet data logger disabled");
        return ESP_ERR_INVALID_STATE;
    }

    pet_time_init();

    esp_err_t ret = build_session_paths();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_csv_header(
        s_state_path,
        "boot_id,time_ms,epoch_ms,time_valid,time_str,state,candidate,event,acc,gyro,acc_std,gyro_std,pitch,roll,state_duration_ms,rest_like_ms"
    );
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_csv_header(
        s_event_path,
        "boot_id,time_ms,epoch_ms,time_valid,time_str,event,state,candidate,acc,gyro,acc_std,gyro_std,pitch,roll,state_duration_ms,rest_like_ms"
    );
    if (ret != ESP_OK) {
        return ret;
    }

    s_ready = true;

    ESP_LOGI(TAG, "pet data logger ready");
    return ESP_OK;
}

bool pet_data_logger_is_ready(void)
{
    return s_ready;
}

esp_err_t pet_data_logger_write_state(uint32_t now_ms, const pet_behavior_result_t *result)
{
    (void)now_ms;

    if (!s_ready || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    char event_buf[96];
    pet_events_to_str(result->events, event_buf, sizeof(event_buf));

    FILE *f = fopen(s_state_path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG,
                 "open state log failed: %s, errno=%d",
                 s_state_path,
                 errno);
        return ESP_FAIL;
    }

    fprintf(f,
            "%08lx,%llu,%lld,%d,\"%s\",%s,%s,%s,%.3f,%.2f,%.3f,%.2f,%.1f,%.1f,%lu,%lu\n",
            (unsigned long)ts.boot_id,
            (unsigned long long)ts.time_ms,
            (long long)ts.epoch_ms,
            ts.time_valid ? 1 : 0,
            ts.time_str,
            pet_state_to_str(result->state),
            pet_state_to_str(result->candidate_state),
            event_buf,
            result->acc_norm_g,
            result->gyro_norm_dps,
            result->acc_norm_std,
            result->gyro_norm_std,
            result->pitch_deg,
            result->roll_deg,
            (unsigned long)result->state_duration_ms,
            (unsigned long)result->rest_like_duration_ms);

    fclose(f);
    return ESP_OK;
}

esp_err_t pet_data_logger_write_event(uint32_t now_ms, const pet_behavior_result_t *result)
{
    (void)now_ms;

    if (!s_ready || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (result->events == PET_EVENT_NONE) {
        return ESP_OK;
    }

    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    char event_buf[96];
    pet_events_to_str(result->events, event_buf, sizeof(event_buf));

    FILE *f = fopen(s_event_path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG,
                 "open event log failed: %s, errno=%d",
                 s_event_path,
                 errno);
        return ESP_FAIL;
    }

    fprintf(f,
            "%08lx,%llu,%lld,%d,\"%s\",%s,%s,%s,%.3f,%.2f,%.3f,%.2f,%.1f,%.1f,%lu,%lu\n",
            (unsigned long)ts.boot_id,
            (unsigned long long)ts.time_ms,
            (long long)ts.epoch_ms,
            ts.time_valid ? 1 : 0,
            ts.time_str,
            event_buf,
            pet_state_to_str(result->state),
            pet_state_to_str(result->candidate_state),
            result->acc_norm_g,
            result->gyro_norm_dps,
            result->acc_norm_std,
            result->gyro_norm_std,
            result->pitch_deg,
            result->roll_deg,
            (unsigned long)result->state_duration_ms,
            (unsigned long)result->rest_like_duration_ms);

    fclose(f);
    return ESP_OK;
}