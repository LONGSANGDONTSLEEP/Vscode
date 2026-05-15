#include "wifi_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <string.h>
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_mgr";

bool wifi_manager_connect_blocking(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid) return false;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password)-1);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);

    esp_wifi_start();
    esp_wifi_connect();

    int waited = 0;
    const int step = 200;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    while (waited < timeout_ms) {
        esp_netif_ip_info_t ipinfo;
        if (netif && esp_netif_get_ip_info(netif, &ipinfo) == ESP_OK) {
            if (ipinfo.ip.addr != 0) {
                /* ip4addr_ntoa expects lwip's ip4_addr_t type; esp_netif provides esp_ip4_addr_t
                   cast to ip4_addr_t to avoid incompatible pointer type warnings. */
                ESP_LOGI(TAG, "WiFi got IP: %s", ip4addr_ntoa((ip4_addr_t *)&ipinfo.ip));
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }

    ESP_LOGW(TAG, "WiFi connect timeout after %d ms", timeout_ms);
    return false;
}
