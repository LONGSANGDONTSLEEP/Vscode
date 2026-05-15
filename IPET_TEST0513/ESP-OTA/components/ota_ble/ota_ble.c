#include "ota_ble.h"
#include "esp_log.h"

static const char* TAG = "ota_ble";

void ota_ble_init(void)
{
    ESP_LOGI(TAG, "BLE OTA init (placeholder)");
    // NOTE: Full BLE DFU/OTA implementation requires integrating
    // ESP-IDF BLE GATT server and a DFU protocol. This file provides
    // a stub that you can extend using ESP-IDF examples such as
    // "esp_ble_ota" or custom GATT characteristics for receiving firmware.
}

void ota_ble_start(void)
{
    ESP_LOGW(TAG, "BLE OTA start called - not fully implemented in this template");
}
