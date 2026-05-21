#include "hwinit.h"

#include "gpio_drv.h"
#include "i2c_bus.h"
#include "led_ctrl.h"
#include "pwrkeep.h"
#include "sd_card_mgr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "HW_INIT"

static i2c_master_bus_handle_t s_i2c_bus;

void check_stack(void)
{
    ESP_LOGI("Stack",
             "Main task stack high water mark: %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

i2c_master_bus_handle_t hwinit_get_i2c_bus(void)
{
    return s_i2c_bus;
}

static void hwinit_init_pwrkeep(void)
{
    ESP_LOGI(TAG, "PWRKEEP init start");

    esp_err_t ret = pwrkeep_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWRKEEP init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "PWRKEEP init success");
    }
}

static void hwinit_init_gpio(void)
{
    ESP_LOGI(TAG, "GPIO init start");

    esp_err_t ret = gpio_drv_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "GPIO init success");
    }
}

static void hwinit_init_i2c(void)
{
    ESP_LOGI(TAG, "I2C init start");

    /*
     * 当前项目 I2C：
     * SDA = GPIO13
     * SCL = GPIO12
     * speed = 400kHz
     */
    esp_err_t ret = i2c_bus_init(&s_i2c_bus, I2C_NUM_0, 13, 12, 400000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "I2C init success");
    }
}

static void hwinit_init_sd_card(void)
{
    ESP_LOGI(TAG, "SD card init start");

    /*
     * SD 卡初始化失败不应该导致系统崩溃。
     * 没插卡时，IMU、OTA、按键仍然应该继续工作。
     */
    esp_err_t ret = sd_card_mgr_init_default();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SD card init success");

    /*
     * 当前阶段先写一个 test.txt，确认 SD 卡能写文件。
     * 后面接 pet_state.csv 后，可以把这句删掉。
     */
    ret = sd_card_mgr_write_test_file();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card test write failed: %s", esp_err_to_name(ret));
    }
}

static void hwinit_init_led(void)
{
    ESP_LOGI(TAG, "LED init start");

    /*
     * 你当前项目里这个函数会初始化 WS2812 并启动 demo。
     */
    led_ctrl_demo_start();

    ESP_LOGI(TAG, "LED init done");
}

void hw_init(void)
{
    ESP_LOGI(TAG, "Hardware init start");
    check_stack();

    hwinit_init_pwrkeep();
    check_stack();

    hwinit_init_gpio();
    check_stack();

    hwinit_init_i2c();
    check_stack();

    hwinit_init_sd_card();
    check_stack();

    hwinit_init_led();
    check_stack();

    ESP_LOGI(TAG, "Hardware init done");
}