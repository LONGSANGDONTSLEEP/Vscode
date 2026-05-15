// main.c now delegates OTA to components
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ota_manager.h"

// OTA URL - replace with your firmware image location or set via NVS
#ifndef OTA_URL
#define OTA_URL "https://example.com/firmware.bin"
#endif

static const char* TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing OTA manager");
    ota_manager_init();

    // Start OTA in AP upload mode so you can connect with phone/computer and upload firmware via browser
    ota_manager_start(OTA_METHOD_WIFI_AP, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
