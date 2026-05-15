#include "ota_wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_err.h"
#include "nvs_flash.h"
#if defined(__has_include)
#  if __has_include("esp_ota_ops.h")
#    include "esp_ota_ops.h"
#  else
/* Provide minimal fallback declarations when esp_ota_ops.h is not available
   This avoids build failures on toolchains/projects where the header
   isn't exposed as a component. These declarations match the esp-idf
   ota APIs used in this file. */
typedef void* esp_ota_handle_t;
extern esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
extern esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *src, size_t size);
extern esp_err_t esp_ota_end(esp_ota_handle_t handle);
extern esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
extern const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t *partition);
#ifndef OTA_SIZE_UNKNOWN
#define OTA_SIZE_UNKNOWN ((size_t)-1)
#endif
#  endif
#else
#  include "esp_ota_ops.h"
#endif
#include "esp_partition.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ota_wifi_ap";

// AP credentials
#ifndef AP_SSID
#define AP_SSID "ESP32_OTA_AP"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "esp32pass"
#endif

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    // This handler receives the uploaded firmware file via multipart/form-data
    // For simplicity, we read the body into a temporary file and then call esp_https_ota API is not usable here.
    // Instead, use esp_ota_begin/esp_ota_write/esp_ota_end sequence.

    char buf[128];
    int ret, remaining = req->content_len;

    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No partition available for OTA");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No partition");
        return ESP_FAIL;
    }

    if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int to_read = (remaining > (int)sizeof(buf)) ? sizeof(buf) : remaining;
        if ((ret = httpd_req_recv(req, buf, to_read)) <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv failed");
            esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        esp_err_t err = esp_ota_write(ota_handle, (const void*)buf, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        remaining -= ret;
    }

    if (esp_ota_end(ota_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK, rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t upload_uri = {
    .uri = "/upload",
    .method = HTTP_POST,
    .handler = upload_post_handler,
    .user_ctx = NULL
};

// Built-in upload page (inlined to avoid CMake embedding issues)
static const char upload_html[] =
"<!doctype html>\n"
"<html>\n"
"  <head>\n"
"    <meta charset=\"utf-8\">\n"
"    <title>ESP OTA Upload</title>\n"
"    <style>\n"
"      body{font-family:Arial,Helvetica,sans-serif;margin:20px}\n"
"      #progress{width:100%;background:#eee;border:1px solid #ccc;height:18px;margin-top:10px}\n"
"      #bar{height:100%;width:0;background:#4caf50}\n"
"      #status{margin-top:10px}\n"
"    </style>\n"
"  </head>\n"
"  <body>\n"
"    <h3>ESP32 OTA Upload</h3>\n"
"    <form id=\"uploadForm\" method=\"POST\" action=\"/upload\" enctype=\"multipart/form-data\">\n"
"      <input type=\"file\" id=\"file\" name=\"firmware\" required>\n"
"      <input type=\"submit\" value=\"Upload\">\n"
"    </form>\n"
"    <div id=\"status\"></div>\n"
"    <div id=\"progress\" style=\"display:none\"><div id=\"bar\"></div></div>\n"
"\n"
"    <script>\n"
"      const form = document.getElementById('uploadForm');\n"
"      const status = document.getElementById('status');\n"
"      const progress = document.getElementById('progress');\n"
"      const bar = document.getElementById('bar');\n"
"\n"
"      form.addEventListener('submit', function(e){\n"
"        e.preventDefault();\n"
"        const fileInput = document.getElementById('file');\n"
"        if(!fileInput.files.length){ status.textContent='请选择固件文件'; return; }\n"
"        const file = fileInput.files[0];\n"
"        const fd = new FormData();\n"
"        fd.append('firmware', file);\n"
"\n"
"        const xhr = new XMLHttpRequest();\n"
"        xhr.open('POST', '/upload', true);\n"
"\n"
"        xhr.onload = function(){\n"
"          if(xhr.status === 200){\n"
"            status.textContent = '上传成功，设备将重启。页面将在3秒后返回根页面。';\n"
"            setTimeout(function(){ window.location.href = '/'; }, 3000);\n"
"          } else {\n"
"            status.textContent = '上传失败: ' + xhr.status + ' ' + xhr.responseText;\n"
"          }\n"
"        };\n"
"        xhr.onerror = function(){ status.textContent = '上传时网络错误'; };\n"
"        xhr.upload.onprogress = function(e){\n"
"          if(e.lengthComputable){\n"
"            progress.style.display = 'block';\n"
"            const pct = Math.round((e.loaded / e.total) * 100);\n"
"            bar.style.width = pct + '%';\n"
"          }\n"
"        };\n"
"\n"
"        status.textContent = '正在上传...';\n"
"        xhr.send(fd);\n"
"      });\n"
"    </script>\n"
"  </body>\n"
"</html>\n";

// Handler to serve the inlined upload page
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, upload_html, strlen(upload_html));
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &upload_uri);
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);
    }
    return server;
}

void ota_wifi_ap_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi AP for OTA: SSID=%s", AP_SSID);
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(AP_SSID);
    wifi_config.ap.max_connection = 4;
    strncpy((char*)wifi_config.ap.password, AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);
    esp_wifi_start();

    start_webserver();
}
