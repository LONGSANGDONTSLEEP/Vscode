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
 * 分段间隔。
 *
 * 调试阶段可以改成 1 分钟：
 * #define LOG_ROTATE_INTERVAL_MS (1 * 60 * 1000ULL)
 *
 * 正常测试建议 3~5 分钟。
 */
#define LOG_ROTATE_INTERVAL_MS (3 * 60 * 1000ULL)

static bool s_ready = false;

static char s_session_dir[160];
static char s_state_path[192];
static char s_event_path[192];

static uint32_t s_segment_no = 1;
static uint64_t s_segment_start_ms = 0;

static uint64_t logger_now_ms(void)
{
    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);
    return ts.time_ms;
}

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
        ESP_LOGE(TAG, "open header file failed: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    fprintf(f, "%s\n", header);
    fclose(f);

    ESP_LOGI(TAG, "CSV header written: %s", path);
    return ESP_OK;
}

static esp_err_t build_session_dir(void)
{
    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    const char *mount = sd_card_mgr_mount_point();

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
            return ESP_OK;
        }

        ESP_LOGW(TAG, "time_str parse failed: %s", ts.time_str);
    }

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

    return ESP_OK;
}

static esp_err_t build_segment_paths(uint32_t segment_no)
{
    char state_name[16];
    char event_name[16];

    /*
     * 8.3 文件名：
     * S000001.CSV
     * E000001.CSV
     */
    snprintf(state_name, sizeof(state_name), "S%06lu.CSV", (unsigned long)segment_no);
    snprintf(event_name, sizeof(event_name), "E%06lu.CSV", (unsigned long)segment_no);

    int len = snprintf(s_state_path,
                       sizeof(s_state_path),
                       "%s/%s",
                       s_session_dir,
                       state_name);
    if (len < 0 || len >= (int)sizeof(s_state_path)) {
        ESP_LOGE(TAG, "state path too long");
        return ESP_FAIL;
    }

    len = snprintf(s_event_path,
                   sizeof(s_event_path),
                   "%s/%s",
                   s_session_dir,
                   event_name);
    if (len < 0 || len >= (int)sizeof(s_event_path)) {
        ESP_LOGE(TAG, "event path too long");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "state log path: %s", s_state_path);
    ESP_LOGI(TAG, "event log path: %s", s_event_path);

    return ESP_OK;
}

static esp_err_t ensure_current_headers(void)
{
    esp_err_t ret;

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

    return ESP_OK;
}

static esp_err_t rotate_if_needed(void)
{
    uint64_t now = logger_now_ms();

    if (s_segment_start_ms == 0) {
        s_segment_start_ms = now;
        return ESP_OK;
    }

    if ((now - s_segment_start_ms) < LOG_ROTATE_INTERVAL_MS) {
        return ESP_OK;
    }

    s_segment_no++;
    s_segment_start_ms = now;

    ESP_LOGI(TAG, "rotate log segment: %lu", (unsigned long)s_segment_no);

    esp_err_t ret = build_segment_paths(s_segment_no);
    if (ret != ESP_OK) {
        return ret;
    }

    return ensure_current_headers();
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

    esp_err_t ret = build_session_dir();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "session dir: %s", s_session_dir);

    s_segment_no = 1;
    s_segment_start_ms = logger_now_ms();

    ret = build_segment_paths(s_segment_no);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_current_headers();
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

const char *pet_data_logger_get_session_dir(void)
{
    return s_session_dir;
}

uint32_t pet_data_logger_get_current_segment(void)
{
    return s_segment_no;
}

esp_err_t pet_data_logger_force_rotate(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_segment_no++;
    s_segment_start_ms = logger_now_ms();

    ESP_LOGI(TAG, "force rotate log segment: %lu", (unsigned long)s_segment_no);

    esp_err_t ret = build_segment_paths(s_segment_no);
    if (ret != ESP_OK) {
        return ret;
    }

    return ensure_current_headers();
}

esp_err_t pet_data_logger_write_state(uint32_t now_ms, const pet_behavior_result_t *result)
{
    (void)now_ms;

    if (!s_ready || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = rotate_if_needed();
    if (ret != ESP_OK) {
        return ret;
    }

    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    char event_buf[96];
    pet_events_to_str(result->events, event_buf, sizeof(event_buf));

    FILE *f = fopen(s_state_path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "open state log failed: %s, errno=%d", s_state_path, errno);
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

    esp_err_t ret = rotate_if_needed();
    if (ret != ESP_OK) {
        return ret;
    }

    pet_time_snapshot_t ts;
    pet_time_get_snapshot(&ts);

    char event_buf[96];
    pet_events_to_str(result->events, event_buf, sizeof(event_buf));

    FILE *f = fopen(s_event_path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "open event log failed: %s, errno=%d", s_event_path, errno);
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