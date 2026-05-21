#include "main.h"

#include "esp_err.h"
#include "esp_log.h"

#include "hwinit.h"
#include "pet_app.h"

#define TAG "MAIN"

void app_main(void)
{
    ESP_LOGI(TAG, "System boot");

    hw_init();

    esp_err_t ret = pet_app_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "application start failed: %s", esp_err_to_name(ret));
    }
}
