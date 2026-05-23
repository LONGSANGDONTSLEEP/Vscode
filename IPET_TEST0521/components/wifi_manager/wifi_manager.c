#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0

static bool s_inited = false;
static bool s_started = false;
static esp_netif_t *s_sta_netif = NULL;
static EventGroupHandle_t s_wifi_events = NULL;
static char s_ssid[33];
static char s_pass[65];
static uint32_t s_disconnect_count = 0;

static esp_err_t wifi_manager_init_once(void);
static void wifi_manager_copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
    }

    memcpy(dst, src, n);
    dst[n] = '\0';
}


static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_disconnect_count++;

        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }

        ESP_LOGW(TAG,
                 "WiFi disconnected, reason=%d, count=%lu, reconnecting...",
                 disc ? disc->reason : -1,
                 (unsigned long)s_disconnect_count);

        /*
         * 断线后立即重连。
         * ESP-IDF 的 esp_wifi_connect() 本身是异步的，这里不会长时间阻塞。
         */
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }

        ESP_LOGI(TAG,
                 "WiFi got IP: %s",
                 event ? ip4addr_ntoa((ip4_addr_t *)&event->ip_info.ip) : "unknown");
        return;
    }
}

static esp_err_t wifi_manager_init_once(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            ESP_LOGE(TAG, "create default WiFi STA netif failed");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI event failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register IP event failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        ESP_LOGE(TAG, "wifi event group create failed");
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set WiFi STA mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 测试/调参阶段优先保证连接稳定，先关闭省电。 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_manager_start_sta(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "empty WiFi SSID");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wifi_manager_init_once();
    if (ret != ESP_OK) {
        return ret;
    }

    const char *safe_pass = pass ? pass : "";
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(safe_pass);

    /*
     * ESP-IDF 的 wifi_config_t.sta.ssid/password 是固定长度数组。
     * 不用 snprintf，避免 GCC 14 在 -Werror=format-truncation 下把
     * “可能截断”当成编译错误；同时明确拒绝过长配置，避免静默截断。
     */
    if (ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid)) {
        ESP_LOGE(TAG, "WiFi SSID too long: %u bytes", (unsigned)ssid_len);
        return ESP_ERR_INVALID_ARG;
    }
    if (pass_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "WiFi password too long: %u bytes", (unsigned)pass_len);
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_copy_cstr(s_ssid, sizeof(s_ssid), ssid);
    wifi_manager_copy_cstr(s_pass, sizeof(s_pass), safe_pass);

    wifi_config_t wifi_config = {0};
    wifi_manager_copy_cstr((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), s_ssid);
    wifi_manager_copy_cstr((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), s_pass);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set WiFi config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_started) {
        ret = esp_wifi_start();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_started = true;
    }

    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }

    ESP_LOGI(TAG, "WiFi connect requested: ssid=%s", s_ssid);
    ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect returned: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_events) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_connect_blocking(const char *ssid, const char *pass, int timeout_ms)
{
    esp_err_t ret = wifi_manager_start_sta(ssid, pass);
    if (ret != ESP_OK) {
        return false;
    }

    bool ok = wifi_manager_wait_connected((uint32_t)timeout_ms);
    if (!ok) {
        ESP_LOGW(TAG, "WiFi connect timeout after %d ms", timeout_ms);
    }
    return ok;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_events) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_wifi_events);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_reconnect_now(void)
{
    if (!s_inited) {
        if (s_ssid[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        return wifi_manager_start_sta(s_ssid, s_pass);
    }

    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }

    ESP_LOGI(TAG, "manual WiFi reconnect requested");
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGD(TAG, "esp_wifi_disconnect returned: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect returned: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
