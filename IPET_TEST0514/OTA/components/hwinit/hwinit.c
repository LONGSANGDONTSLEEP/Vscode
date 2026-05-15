#include "hwinit.h"

#include "gpio_drv.h"
#include "i2c_bus.h"
#include "ota_update.h"
#include "led_ctrl.h"

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

    ESP_LOGI("HW_INIT", "GPIO init start");
    ESP_ERROR_CHECK(gpio_drv_init());
    ESP_LOGI("HW_INIT", "GPIO初始化完成");
    check_stack();

    // 简单示例：如果你在固件中希望自动连接某个 WiFi，可在这里调用 wifi_connect_sta
    // 例如：wifi_connect_sta("你的SSID", "你的密码", 15000);

    
    ESP_LOGI("HW_INIT", "I2C init start");
    i2c_bus_init(&bus, I2C_NUM_0, CONFIG_I2C_BUS_SDA_GPIO, CONFIG_I2C_BUS_SCL_GPIO, 400000);
    i2c_bus_add_device(bus, 0x68, &dev);
    ESP_LOGI("HW_INIT", "I2C初始化完成");
    check_stack();

    /* 启动 LED demo（默认使用 GPIO48） */
    led_ctrl_demo_start();




}





