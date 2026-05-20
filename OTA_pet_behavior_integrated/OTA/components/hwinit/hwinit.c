#include "hwinit.h"

#include "gpio_drv.h"
#include "i2c_bus.h"
#include "led_ctrl.h"
#include "pwrkeep.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "HW_INIT"

static i2c_master_bus_handle_t s_i2c_bus;

void check_stack(void)
{
    ESP_LOGI("Stack", "Main task stack high water mark: %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

i2c_master_bus_handle_t hwinit_get_i2c_bus(void)
{
    return s_i2c_bus;
}

void hw_init(void)
{
    ESP_LOGI(TAG, "Hardware init start");
    check_stack();

    // 先启动电源保持，避免后续初始化耗时导致自保持掉电。
    ESP_LOGI(TAG, "PWRKEEP init start");
    esp_err_t pk_err = pwrkeep_init();
    if (pk_err != ESP_OK) {
        ESP_LOGE(TAG, "pwrkeep_init failed: %s", esp_err_to_name(pk_err));
    } else {
        ESP_LOGI(TAG, "PWRKEEP 初始化完成");
    }

    ESP_LOGI(TAG, "GPIO init start");
    ESP_ERROR_CHECK(gpio_drv_init());
    ESP_LOGI(TAG, "GPIO 初始化完成");
    check_stack();

    ESP_LOGI(TAG, "I2C init start");
    // QMI8658A 当前硬件接线：SDA=IO13, SCL=IO12。
    ESP_ERROR_CHECK(i2c_bus_init(&s_i2c_bus, I2C_NUM_0, 13, 12, 400000));
    ESP_LOGI(TAG, "I2C 初始化完成");
    check_stack();

    // 启动 WS2812 demo（默认 GPIO48）
    led_ctrl_demo_start();

    ESP_LOGI(TAG, "Hardware init done");
}
