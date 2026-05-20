#include "hwinit.h"

#include "gpio_drv.h"
#include "i2c_bus.h"
#include "ota_update.h"
#include "led_ctrl.h"
#include "pwrkeep.h"

#include "esp_err.h"
#include "esp_log.h"

#include "driver/i2c_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Wi‑Fi 连接移至 components/wifi_manager */

/* 日志TAG */
//static const char *TAG = "HW_INIT";

i2c_master_bus_handle_t bus;
i2c_master_dev_handle_t dev;



void check_stack(void)
{
    ESP_LOGI("Stack", "Main task stack high water mark: %d", uxTaskGetStackHighWaterMark(NULL));
}


/* 初始化所有外设 */
void hw_init(void)
{
    ESP_LOGI("HW_INIT", "Hardware init start");
    check_stack();

    /* 先尽早启动电源保持任务，确保在上电后尽快拉高保持引脚以维持供电。
       把 pwrkeep_init 放在 gpio_drv_init 之前，避免 gpio_drv 可能的引脚重配置干扰保持引脚。 */
    ESP_LOGI("HW_INIT", "PWRKEEP init start");
    esp_err_t pk_err = pwrkeep_init();
    if (pk_err != ESP_OK) {
        ESP_LOGE("HW_INIT", "pwrkeep_init failed: %s", esp_err_to_name(pk_err));
    } else {
        ESP_LOGI("HW_INIT", "PWRKEEP 初始化完成");
    }

    ESP_LOGI("HW_INIT", "GPIO init start");
    ESP_ERROR_CHECK(gpio_drv_init());
    ESP_LOGI("HW_INIT", "GPIO初始化完成");
    check_stack();

    // 简单示例：如果你在固件中希望自动连接某个 WiFi，可在这里调用 wifi_connect_sta
    // 例如：wifi_connect_sta("你的SSID", "你的密码", 15000);

    
    ESP_LOGI("HW_INIT", "I2C init start");
    // 使用明确的引脚（SDA=IO13, SCL=IO12），避免 CONFIG 配置不一致导致总线与硬件接线不符
    i2c_bus_init(&bus, I2C_NUM_0, 13, 12, 400000);
    i2c_bus_add_device(bus, 0x68, &dev);
    ESP_LOGI("HW_INIT", "I2C初始化完成");
    check_stack();

    /* 启动 LED demo（默认使用 GPIO48） */
    led_ctrl_demo_start();
}
