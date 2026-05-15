#include "ota_manager.h"
#include "esp_log.h"
#include "ota_wifi.h"
#include "ota_ble.h"
#include "ota_wifi.h"

static const char* TAG = "ota_manager";

void ota_manager_init(void)
{
    ESP_LOGI(TAG, "OTA manager init");
    ota_wifi_init();
    ota_ble_init();
}

void ota_manager_start(ota_method_t method, const char* url)
{
    ESP_LOGI(TAG, "OTA start method=%d url=%s", method, url ? url : "(null)");
    switch (method) {
    case OTA_METHOD_WIFI:
        ota_wifi_start(url);
        break;
    case OTA_METHOD_WIFI_AP:
        // start AP-based upload webserver
        ota_wifi_ap_start();
        break;
    case OTA_METHOD_BLE:
        ota_ble_start();
        break;
    default:
        ESP_LOGW(TAG, "Unknown OTA method");
        break;
    }
}
