#include "pet_file_uploader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "pet_data_logger.h"
#include "wifi_manager.h"

#define TAG "FILE_UP"

#define URL_MAX_LEN       160
#define PATH_MAX_LEN      192
#define NAME_MAX_LEN      16
#define SESSION_MAX_LEN   128

static TaskHandle_t s_task = NULL;
static bool s_ready = false;
static volatile bool s_scan_now = false;
static uint32_t s_last_success_ms = 0;

static char s_url[URL_MAX_LEN];
static uint32_t s_scan_interval_ms = 60000;
static uint32_t s_timeout_ms = 30000;

static uint32_t uploader_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
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

static bool str_ends_with_csv(const char *name)
{
    size_t len = strlen(name);

    if (len != 11) {
        return false;
    }

    return name[7] == '.' &&
           toupper((unsigned char)name[8]) == 'C' &&
           toupper((unsigned char)name[9]) == 'S' &&
           toupper((unsigned char)name[10]) == 'V';
}

static bool parse_segment_file(const char *name, char *type_out, uint32_t *seg_out)
{
    if (!name || !type_out || !seg_out) {
        return false;
    }

    /*
     * 文件名格式：
     * S000001.CSV 状态窗口
     * E000001.CSV 事件
     * R000001.CSV 高频 raw IMU，录制测试时最重要
     */
    if (!str_ends_with_csv(name)) {
        return false;
    }

    char type = toupper((unsigned char)name[0]);
    if (type != 'S' && type != 'E' && type != 'R') {
        return false;
    }

    uint32_t seg = 0;

    for (int i = 1; i <= 6; ++i) {
        if (!isdigit((unsigned char)name[i])) {
            return false;
        }

        seg = seg * 10 + (uint32_t)(name[i] - '0');
    }

    if (seg == 0) {
        return false;
    }

    *type_out = type;
    *seg_out = seg;
    return true;
}

static bool segment_is_safe_to_upload(char type, uint32_t seg, uint32_t current_seg)
{
    /*
     * S/E 当前分段仍可能正在写，必须等 seg < current_seg。
     * R 高频 raw 文件不同：网页结束录制后 raw logger 会关闭，
     * 此时即使 R 的编号等于 current_seg，也已经是完整文件，可以立刻上传。
     */
    if (seg < current_seg) {
        return true;
    }

    if (type == 'R' && seg == current_seg && !pet_data_logger_raw_is_enabled()) {
        return true;
    }

    return false;
}

static void make_session_relative(const char *session_dir, char *out, size_t out_len)
{
    const char *prefix = "/sdcard/";
    size_t prefix_len = strlen(prefix);

    if (strncmp(session_dir, prefix, prefix_len) == 0) {
        snprintf(out, out_len, "%s", session_dir + prefix_len);
    } else {
        snprintf(out, out_len, "%s", session_dir);
    }
}

static esp_err_t post_file(const char *path, const char *file_name, const char *session_rel)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    if (st.st_size <= 0) {
        ESP_LOGW(TAG, "skip empty file: %s", path);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "open file failed: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)s_timeout_ms,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        ESP_LOGW(TAG, "http client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "text/csv");
    esp_http_client_set_header(client, "X-File-Name", file_name);
    esp_http_client_set_header(client, "X-Session", session_rel);

    esp_err_t ret = esp_http_client_open(client, (int)st.st_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        fclose(f);
        return ret;
    }

    char buf[1024];
    size_t read_len;

    while ((read_len = fread(buf, 1, sizeof(buf), f)) > 0) {
        int written = esp_http_client_write(client, buf, read_len);

        if (written < 0 || written != (int)read_len) {
            ESP_LOGW(TAG, "http write failed, written=%d, read=%u",
                     written,
                     (unsigned)read_len);
            ret = ESP_FAIL;
            break;
        }
    }

    fclose(f);

    if (ret == ESP_OK) {
        esp_http_client_fetch_headers(client);

        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG,
                     "upload OK: %s, size=%ld, status=%d",
                     file_name,
                     (long)st.st_size,
                     status);
            s_last_success_ms = uploader_now_ms();
            ret = ESP_OK;
        } else {
            ESP_LOGW(TAG, "upload failed HTTP status=%d file=%s", status, file_name);
            ret = ESP_FAIL;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return ret;
}

static esp_err_t move_to_sent(const char *session_dir, const char *path, const char *file_name)
{
    char sent_dir[PATH_MAX_LEN];
    char dst[PATH_MAX_LEN];

    int len = snprintf(sent_dir, sizeof(sent_dir), "%s/SENT", session_dir);
    if (len < 0 || len >= (int)sizeof(sent_dir)) {
        ESP_LOGE(TAG, "sent dir path too long");
        return ESP_FAIL;
    }

    if (mkdir_if_needed(sent_dir) != ESP_OK) {
        return ESP_FAIL;
    }

    len = snprintf(dst, sizeof(dst), "%s/%s", sent_dir, file_name);
    if (len < 0 || len >= (int)sizeof(dst)) {
        ESP_LOGE(TAG, "sent file path too long");
        return ESP_FAIL;
    }

    /*
     * 如果目标已经存在，先删掉，避免 rename 失败。
     */
    remove(dst);

    if (rename(path, dst) != 0) {
        ESP_LOGW(TAG,
                 "move to SENT failed: %s -> %s, errno=%d",
                 path,
                 dst,
                 errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "moved to SENT: %s", file_name);
    return ESP_OK;
}

static void scan_and_upload_once(void)
{
    const char *session_dir = pet_data_logger_get_session_dir();

    if (!session_dir || session_dir[0] == '\0') {
        return;
    }

    uint32_t current_seg = pet_data_logger_get_current_segment();
    uint32_t found_files = 0;
    uint32_t skipped_current = 0;
    uint32_t tried_files = 0;

    DIR *dir = opendir(session_dir);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed: %s, errno=%d", session_dir, errno);
        return;
    }

    char session_rel[SESSION_MAX_LEN];
    make_session_relative(session_dir, session_rel, sizeof(session_rel));

    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        char type = 0;
        uint32_t seg = 0;

        if (!parse_segment_file(ent->d_name, &type, &seg)) {
            continue;
        }

        found_files++;

        /*
         * 当前正在写的 S/E segment 不上传，避免边写边传导致 CSV 不完整。
         * 但 R 高频 raw 文件在录制结束后会关闭；关闭后即使编号等于
         * current_seg，也应该立即上传，否则网页上看不到 R000xxx.CSV。
         */
        if (!segment_is_safe_to_upload(type, seg, current_seg)) {
            skipped_current++;
            continue;
        }

        char path[PATH_MAX_LEN];

        int len = snprintf(path,
                           sizeof(path),
                           "%s/%s",
                           session_dir,
                           ent->d_name);
        if (len < 0 || len >= (int)sizeof(path)) {
            ESP_LOGW(TAG, "file path too long: %s", ent->d_name);
            continue;
        }

        tried_files++;
        esp_err_t ret = post_file(path, ent->d_name, session_rel);
        if (ret == ESP_OK) {
            move_to_sent(session_dir, path, ent->d_name);
        } else {
            /*
             * 上传失败时保留文件，下次扫描继续尝试。
             * 避免网络不好时阻塞太久。
             */
            ESP_LOGW(TAG, "upload failed, keep file: %s", ent->d_name);
            break;
        }
    }

    closedir(dir);

    if (found_files > 0 && tried_files == 0) {
        ESP_LOGI(TAG,
                 "scan: session=%s current_seg=%lu raw_enabled=%d files=%lu skipped_current=%lu",
                 session_dir,
                 (unsigned long)current_seg,
                 pet_data_logger_raw_is_enabled() ? 1 : 0,
                 (unsigned long)found_files,
                 (unsigned long)skipped_current);
    }
}

static void uploader_task(void *arg)
{
    ESP_LOGI(TAG, "file uploader task started, url=%s", s_url);

    while (1) {
        if (!wifi_manager_is_connected()) {
            /*
             * 网络断开时不要反复打开 HTTP，保留 CSV 文件，等 Wi-Fi 恢复后继续上传。
             */
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (pet_data_logger_is_ready()) {
            scan_and_upload_once();
        }

        uint32_t waited = 0;
        while (waited < s_scan_interval_ms) {
            if (s_scan_now) {
                s_scan_now = false;
                break;
            }
            uint32_t step = 250;
            uint32_t remain = s_scan_interval_ms - waited;
            if (step > remain) {
                step = remain;
            }
            vTaskDelay(pdMS_TO_TICKS(step));
            waited += step;
        }
    }
}

esp_err_t pet_file_uploader_start(const pet_file_uploader_config_t *cfg)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (!cfg || !cfg->url || cfg->url[0] == '\0') {
        ESP_LOGW(TAG, "invalid upload url");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_url, sizeof(s_url), "%s", cfg->url);

    s_scan_interval_ms = cfg->scan_interval_ms ? cfg->scan_interval_ms : 60000;
    if (s_scan_interval_ms < 1000) {
        s_scan_interval_ms = 1000;
    }
    s_timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 30000;
    s_scan_now = true;

    uint32_t stack_size = cfg->task_stack_size ? cfg->task_stack_size : 6144;
    uint32_t priority = cfg->task_priority ? cfg->task_priority : 3;

    BaseType_t ok = xTaskCreate(
        uploader_task,
        "file_upload",
        stack_size,
        NULL,
        priority,
        &s_task
    );

    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "file uploader task create failed");
        return ESP_FAIL;
    }

    s_ready = true;

    ESP_LOGI(TAG,
             "file uploader started, interval=%lu ms",
             (unsigned long)s_scan_interval_ms);

    return ESP_OK;
}

esp_err_t pet_file_uploader_stop(void)
{
    s_ready = false;
    s_scan_now = false;

    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    ESP_LOGI(TAG, "file uploader stopped");
    return ESP_OK;
}

bool pet_file_uploader_is_ready(void)
{
    return s_ready;
}
void pet_file_uploader_request_scan_now(void)
{
    /*
     * Called by recording stop / log rotate paths to wake the uploader quickly.
     * The uploader task checks this flag every 250 ms while waiting for the
     * normal scan interval, so this does not need a semaphore or queue.
     */
    s_scan_now = true;
}

uint32_t pet_file_uploader_last_success_ms(void)
{
    return s_last_success_ms;
}
