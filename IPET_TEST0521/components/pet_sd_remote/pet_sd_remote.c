#include "pet_sd_remote.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_card_mgr.h"
#include "wifi_manager.h"

#define TAG "SD_REMOTE"

#define SD_REMOTE_URL_MAX_LEN        192
#define SD_REMOTE_ID_MAX_LEN         72
#define SD_REMOTE_CMD_MAX_LEN        16
#define SD_REMOTE_PATH_MAX_LEN       256
#define SD_REMOTE_BODY_MAX_LEN       16384
#define SD_REMOTE_CMD_BODY_MAX_LEN   4096
#define SD_REMOTE_MAX_SELECTED       12
#define SD_REMOTE_MAX_LIST_FILES     80
#define SD_REMOTE_MAX_DEPTH          1

static TaskHandle_t s_task;
static bool s_ready;
static char s_command_url[SD_REMOTE_URL_MAX_LEN];
static char s_upload_url[SD_REMOTE_URL_MAX_LEN];
static uint32_t s_poll_ms = 2000;
static uint32_t s_timeout_ms = 6000;

static void json_escape_append(char *out, size_t out_len, size_t *pos, const char *s)
{
    if (!out || !pos || *pos >= out_len) {
        return;
    }
    if (!s) {
        s = "";
    }
    for (const char *p = s; *p && *pos + 2 < out_len; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[(*pos)++] = '\\';
            out[(*pos)++] = (char)c;
        } else if (c == '\n') {
            if (*pos + 2 < out_len) {
                out[(*pos)++] = '\\';
                out[(*pos)++] = 'n';
            }
        } else if (c == '\r') {
            if (*pos + 2 < out_len) {
                out[(*pos)++] = '\\';
                out[(*pos)++] = 'r';
            }
        } else if (c >= 32) {
            out[(*pos)++] = (char)c;
        }
    }
    out[*pos] = '\0';
}

static void append_text(char *out, size_t out_len, size_t *pos, const char *fmt, ...)
{
    if (!out || !pos || *pos >= out_len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, out_len - *pos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= out_len - *pos) {
        *pos = out_len - 1;
        out[*pos] = '\0';
    } else {
        *pos += (size_t)n;
    }
}

static bool has_key(const char *json, const char *key)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return json && strstr(json, pattern) != NULL;
}

static bool json_bool_or_int_true(const char *json, const char *key)
{
    if (!json || !key) {
        return false;
    }
    char pattern[48];
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
    return *p == '1' || strncmp(p, "true", 4) == 0 || strncmp(p, "TRUE", 4) == 0;
}

static void copy_json_string_value(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    char pattern[48];
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
    if (*p != '"') {
        return;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
}

static int parse_json_string_array(const char *json,
                                   const char *key,
                                   char out[][SD_REMOTE_PATH_MAX_LEN],
                                   int max_items)
{
    if (!json || !key || !out || max_items <= 0) {
        return 0;
    }
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pattern), '[');
    if (!p) {
        return 0;
    }
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_items) {
        while (*p && *p != '"' && *p != ']') {
            p++;
        }
        if (*p == ']') {
            break;
        }
        p++;
        size_t n = 0;
        while (*p && *p != '"' && n + 1 < SD_REMOTE_PATH_MAX_LEN) {
            if (*p == '\\' && p[1]) {
                p++;
            }
            out[count][n++] = *p++;
        }
        out[count][n] = '\0';
        count++;
        while (*p && *p != ',' && *p != ']') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return count;
}

static esp_err_t http_get_body(const char *url, char *body, size_t body_size)
{
    if (!url || !body || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    body[0] = '\0';
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)s_timeout_ms,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "GET status=%d url=%s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int total = 0;
    while (total < (int)body_size - 1) {
        int r = esp_http_client_read(client, body + total, body_size - 1 - total);
        if (r < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        total += r;
        if (content_length > 0 && total >= content_length) {
            break;
        }
    }
    body[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t post_json(const char *url, const char *json)
{
    if (!url || !json || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int len = (int)strlen(json);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)s_timeout_ms,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t ret = esp_http_client_open(client, len);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }
    int written = esp_http_client_write(client, json, len);
    if (written != len) {
        ret = ESP_FAIL;
    } else {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ret = (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "POST json status=%d", status);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static bool contains_parent_ref(const char *path)
{
    if (!path) {
        return true;
    }
    return strstr(path, "..") != NULL;
}

static bool resolve_sd_path(const char *input, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    const char *root = sd_card_mgr_mount_point();
    if (!root || root[0] == '\0') {
        root = "/sdcard";
    }
    if (!input || input[0] == '\0' || strcmp(input, "/") == 0) {
        snprintf(out, out_len, "%s", root);
        return true;
    }
    if (contains_parent_ref(input)) {
        return false;
    }
    if (strncmp(input, root, strlen(root)) == 0) {
        snprintf(out, out_len, "%s", input);
    } else if (input[0] == '/') {
        snprintf(out, out_len, "%s%s", root, input);
    } else {
        snprintf(out, out_len, "%s/%s", root, input);
    }
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[len - 1] = '\0';
        len--;
    }
    return strncmp(out, root, strlen(root)) == 0;
}

static const char *base_name(const char *path)
{
    if (!path) {
        return "";
    }
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static const char *file_kind_from_name(const char *name)
{
    if (!name) {
        return "file";
    }
    size_t len = strlen(name);
    if (len >= 11 && name[len - 4] == '.') {
        char c0 = (char)toupper((unsigned char)name[len - 11]);
        if (c0 == 'R') {
            return "raw";
        }
        if (c0 == 'S') {
            return "state";
        }
        if (c0 == 'E') {
            return "event";
        }
    }
    if (strcasecmp(name, "CONFIG.TXT") == 0) {
        return "config";
    }
    return "file";
}

static void append_file_entry(char *json,
                              size_t json_len,
                              size_t *pos,
                              const char *path,
                              const char *name,
                              bool is_dir,
                              long size,
                              long mtime,
                              bool *first)
{
    if (!*first) {
        append_text(json, json_len, pos, ",");
    }
    *first = false;
    append_text(json, json_len, pos, "{\"path\":\"");
    json_escape_append(json, json_len, pos, path);
    append_text(json, json_len, pos, "\",\"name\":\"");
    json_escape_append(json, json_len, pos, name);
    append_text(json,
                json_len,
                pos,
                "\",\"is_dir\":%d,\"size\":%ld,\"mtime\":%ld,\"kind\":\"%s\"}",
                is_dir ? 1 : 0,
                size,
                mtime,
                is_dir ? "dir" : file_kind_from_name(name));
}

typedef struct {
    char *json;
    size_t json_len;
    size_t *pos;
    int count;
    bool truncated;
    bool first;
} list_ctx_t;

static void list_dir_one_level(const char *dir_path, list_ctx_t *ctx)
{
    if (!dir_path || !ctx || ctx->count >= SD_REMOTE_MAX_LIST_FILES) {
        if (ctx) {
            ctx->truncated = true;
        }
        return;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed: %s errno=%d", dir_path, errno);
        return;
    }

    const TickType_t start_tick = xTaskGetTickCount();
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (ctx->count >= SD_REMOTE_MAX_LIST_FILES || *(ctx->pos) + 512 >= ctx->json_len) {
            ctx->truncated = true;
            break;
        }
        /*
         * Keep list operation intentionally lightweight.
         * The first SD browser version recursively walked the whole /sdcard tree;
         * on SDMMC cards with many D/T/session folders that can occupy the card
         * long enough to collide with logger/uploader access and trigger timeout.
         * Now we only list the current directory. The web UI lets the user enter
         * a directory to browse deeper.
         */
        char child[SD_REMOTE_PATH_MAX_LEN];
        int n = snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name);
        if (n < 0 || n >= (int)sizeof(child)) {
            ctx->truncated = true;
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        bool is_dir = S_ISDIR(st.st_mode);
        append_file_entry(ctx->json,
                          ctx->json_len,
                          ctx->pos,
                          child,
                          ent->d_name,
                          is_dir,
                          is_dir ? 0L : (long)st.st_size,
                          (long)st.st_mtime,
                          &ctx->first);
        ctx->count++;

        if ((ctx->count % 12) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if ((xTaskGetTickCount() - start_tick) > pdMS_TO_TICKS(600)) {
            ctx->truncated = true;
            break;
        }
    }
    closedir(dir);
}

static void send_simple_result(const char *result_url,
                               const char *id,
                               const char *cmd,
                               bool ok,
                               const char *message,
                               int affected)
{
    char body[768];
    snprintf(body,
             sizeof(body),
             "{\"ok\":%s,\"id\":\"%s\",\"cmd\":\"%s\",\"message\":\"%s\",\"affected\":%d}",
             ok ? "true" : "false",
             id ? id : "",
             cmd ? cmd : "",
             message ? message : "",
             affected);
    post_json(result_url, body);
}

static void handle_list_command(const char *result_url, const char *id, const char *path_in)
{
    char path[SD_REMOTE_PATH_MAX_LEN];
    if (!resolve_sd_path(path_in, path, sizeof(path))) {
        send_simple_result(result_url, id, "list", false, "invalid path", 0);
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_simple_result(result_url, id, "list", false, "directory not found", 0);
        return;
    }

    char *body = calloc(1, SD_REMOTE_BODY_MAX_LEN);
    if (!body) {
        send_simple_result(result_url, id, "list", false, "no memory", 0);
        return;
    }
    size_t pos = 0;
    append_text(body, SD_REMOTE_BODY_MAX_LEN, &pos, "{\"ok\":true,\"id\":\"");
    json_escape_append(body, SD_REMOTE_BODY_MAX_LEN, &pos, id);
    append_text(body, SD_REMOTE_BODY_MAX_LEN, &pos, "\",\"cmd\":\"list\",\"path\":\"");
    json_escape_append(body, SD_REMOTE_BODY_MAX_LEN, &pos, path);
    append_text(body, SD_REMOTE_BODY_MAX_LEN, &pos, "\",\"files\":[");

    list_ctx_t ctx = {
        .json = body,
        .json_len = SD_REMOTE_BODY_MAX_LEN,
        .pos = &pos,
        .count = 0,
        .truncated = false,
        .first = true,
    };
    list_dir_one_level(path, &ctx);
    append_text(body,
                SD_REMOTE_BODY_MAX_LEN,
                &pos,
                "],\"count\":%d,\"truncated\":%d}",
                ctx.count,
                ctx.truncated ? 1 : 0);
    ESP_LOGI(TAG, "sd list one-level: %s files=%d truncated=%d", path, ctx.count, ctx.truncated ? 1 : 0);
    post_json(result_url, body);
    free(body);
}

static bool is_protected_path(const char *path)
{
    if (!path) {
        return true;
    }
    if (strcmp(path, "/sdcard") == 0 || strcmp(path, "/sdcard/") == 0) {
        return true;
    }
    const char *name = base_name(path);
    if (strcasecmp(name, "CONFIG.TXT") == 0) {
        return true;
    }
    return false;
}

static void make_upload_session_for_path(const char *path, char *out, size_t out_len)
{
    /*
     * Build the X-Session value used by the PC web server.
     *
     * Do not use snprintf(out, out_len, "sdcard/%s", tmp) here: ESP-IDF
     * builds with -Werror=format-truncation and GCC can correctly see that a
     * 255 byte relative path may not fit into a 256 byte destination once the
     * "sdcard/" prefix is added. Manual bounded copying keeps the upload
     * session safe and avoids the build warning/error.
     */
    if (!out || out_len == 0) {
        return;
    }

    const char *prefix = "/sdcard/";
    const char *rel = path ? path : "";
    if (strncmp(rel, prefix, strlen(prefix)) == 0) {
        rel += strlen(prefix);
    }

    char tmp[SD_REMOTE_PATH_MAX_LEN];
    size_t rel_len = strnlen(rel, sizeof(tmp) - 1);
    memcpy(tmp, rel, rel_len);
    tmp[rel_len] = '\0';

    char *slash = strrchr(tmp, '/');
    if (slash) {
        *slash = '\0';
    } else {
        tmp[0] = '\0';
    }

    const char *base = "sdcard";
    size_t pos = 0;
    out[0] = '\0';

    while (base[pos] != '\0' && pos + 1 < out_len) {
        out[pos] = base[pos];
        pos++;
    }
    out[pos] = '\0';

    if (tmp[0] == '\0' || pos + 1 >= out_len) {
        return;
    }

    out[pos++] = '/';
    out[pos] = '\0';

    size_t i = 0;
    while (tmp[i] != '\0' && pos + 1 < out_len) {
        out[pos++] = tmp[i++];
    }
    out[pos] = '\0';
}

static esp_err_t post_file_to_upload(const char *upload_url, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size <= 0) {
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    esp_http_client_config_t config = {
        .url = upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        return ESP_FAIL;
    }
    char session[SD_REMOTE_PATH_MAX_LEN];
    make_upload_session_for_path(path, session, sizeof(session));
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-File-Name", base_name(path));
    esp_http_client_set_header(client, "X-Session", session);
    esp_http_client_set_header(client, "X-SD-Path", path);
    esp_err_t ret = esp_http_client_open(client, (int)st.st_size);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        fclose(f);
        return ret;
    }
    char buf[1024];
    size_t read_len;
    while ((read_len = fread(buf, 1, sizeof(buf), f)) > 0) {
        int written = esp_http_client_write(client, buf, read_len);
        if (written != (int)read_len) {
            ret = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    if (ret == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ret = (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static void handle_upload_command(const char *result_url,
                                  const char *id,
                                  const char *upload_url,
                                  char files[][SD_REMOTE_PATH_MAX_LEN],
                                  int file_count)
{
    int ok_count = 0;
    int fail_count = 0;
    char real_path[SD_REMOTE_PATH_MAX_LEN];
    for (int i = 0; i < file_count; ++i) {
        if (!resolve_sd_path(files[i], real_path, sizeof(real_path)) || is_protected_path(real_path)) {
            fail_count++;
            continue;
        }
        esp_err_t ret = post_file_to_upload(upload_url && upload_url[0] ? upload_url : s_upload_url, real_path);
        if (ret == ESP_OK) {
            ok_count++;
            ESP_LOGI(TAG, "sd upload OK: %s", real_path);
        } else {
            fail_count++;
            ESP_LOGW(TAG, "sd upload failed: %s ret=%s", real_path, esp_err_to_name(ret));
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "uploaded=%d failed=%d", ok_count, fail_count);
    send_simple_result(result_url, id, "upload", fail_count == 0, msg, ok_count);
}

static void handle_delete_command(const char *result_url,
                                  const char *id,
                                  char files[][SD_REMOTE_PATH_MAX_LEN],
                                  int file_count)
{
    int ok_count = 0;
    int fail_count = 0;
    char real_path[SD_REMOTE_PATH_MAX_LEN];
    for (int i = 0; i < file_count; ++i) {
        if (!resolve_sd_path(files[i], real_path, sizeof(real_path)) || is_protected_path(real_path)) {
            fail_count++;
            continue;
        }
        struct stat st;
        if (stat(real_path, &st) != 0) {
            fail_count++;
            continue;
        }
        int rc = -1;
        if (S_ISDIR(st.st_mode)) {
            /* 安全起见，只允许删除空目录；不做递归删除。 */
            rc = rmdir(real_path);
        } else {
            rc = remove(real_path);
        }
        if (rc == 0) {
            ok_count++;
            ESP_LOGW(TAG, "sd delete OK: %s", real_path);
        } else {
            fail_count++;
            ESP_LOGW(TAG, "sd delete failed: %s errno=%d", real_path, errno);
        }
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "deleted=%d failed=%d", ok_count, fail_count);
    send_simple_result(result_url, id, "delete", fail_count == 0, msg, ok_count);
}

static void execute_command(const char *body)
{
    if (!body || !json_bool_or_int_true(body, "sd")) {
        return;
    }
    char id[SD_REMOTE_ID_MAX_LEN];
    char cmd[SD_REMOTE_CMD_MAX_LEN];
    char path[SD_REMOTE_PATH_MAX_LEN];
    char result_url[SD_REMOTE_URL_MAX_LEN];
    char upload_url[SD_REMOTE_URL_MAX_LEN];
    char files[SD_REMOTE_MAX_SELECTED][SD_REMOTE_PATH_MAX_LEN];

    copy_json_string_value(body, "id", id, sizeof(id));
    copy_json_string_value(body, "cmd", cmd, sizeof(cmd));
    copy_json_string_value(body, "path", path, sizeof(path));
    copy_json_string_value(body, "result_url", result_url, sizeof(result_url));
    copy_json_string_value(body, "upload_url", upload_url, sizeof(upload_url));
    if (result_url[0] == '\0') {
        ESP_LOGW(TAG, "sd command missing result_url");
        return;
    }

    if (!sd_card_mgr_is_mounted()) {
        ESP_LOGW(TAG, "sd command ignored: SD card not mounted");
        send_simple_result(result_url, id, cmd[0] ? cmd : "sd", false, "sd card not mounted", 0);
        return;
    }

    if (upload_url[0] == '\0') {
        snprintf(upload_url, sizeof(upload_url), "%s", s_upload_url);
    }

    int file_count = parse_json_string_array(body, "files", files, SD_REMOTE_MAX_SELECTED);
    ESP_LOGI(TAG, "sd command: id=%s cmd=%s path=%s files=%d", id, cmd, path, file_count);

    if (strcmp(cmd, "list") == 0) {
        handle_list_command(result_url, id, path[0] ? path : "/sdcard");
    } else if (strcmp(cmd, "upload") == 0) {
        handle_upload_command(result_url, id, upload_url, files, file_count);
    } else if (strcmp(cmd, "delete") == 0) {
        handle_delete_command(result_url, id, files, file_count);
    } else {
        send_simple_result(result_url, id, cmd, false, "unknown command", 0);
    }
}

static void sd_remote_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SD remote polling started: %s", s_command_url);
    char *body = malloc(SD_REMOTE_CMD_BODY_MAX_LEN);
    if (!body) {
        ESP_LOGE(TAG, "no memory for command body");
        vTaskDelete(NULL);
        return;
    }
    while (s_ready) {
        if (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(s_poll_ms));
            continue;
        }
        if (http_get_body(s_command_url, body, SD_REMOTE_CMD_BODY_MAX_LEN) == ESP_OK) {
            execute_command(body);
        }
        vTaskDelay(pdMS_TO_TICKS(s_poll_ms));
    }
    free(body);
    vTaskDelete(NULL);
}

esp_err_t pet_sd_remote_start(const pet_sd_remote_config_t *cfg)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!cfg || !cfg->command_url || cfg->command_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_command_url, sizeof(s_command_url), "%s", cfg->command_url);
    snprintf(s_upload_url, sizeof(s_upload_url), "%s", cfg->upload_url ? cfg->upload_url : "");
    s_poll_ms = cfg->poll_ms ? cfg->poll_ms : 2000;
    if (s_poll_ms < 500) {
        s_poll_ms = 500;
    }
    s_timeout_ms = cfg->http_timeout_ms ? cfg->http_timeout_ms : 6000;
    uint32_t stack_size = cfg->task_stack_size ? cfg->task_stack_size : 8192;
    uint32_t priority = cfg->task_priority ? cfg->task_priority : 3;
    s_ready = true;
    BaseType_t ok = xTaskCreate(sd_remote_task, "sd_remote", stack_size, NULL, priority, &s_task);
    if (ok != pdPASS) {
        s_ready = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SD remote started, poll=%lu ms", (unsigned long)s_poll_ms);
    return ESP_OK;
}

esp_err_t pet_sd_remote_stop(void)
{
    s_ready = false;
    if (s_task) {
        TaskHandle_t task = s_task;
        s_task = NULL;
        vTaskDelete(task);
    }
    return ESP_OK;
}

bool pet_sd_remote_is_ready(void)
{
    return s_ready;
}
