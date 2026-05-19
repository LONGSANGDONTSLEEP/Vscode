#include "main.h"

#include "hwinit.h"
#include "ota_update.h"
#include "gpio_drv.h"
#include "pwrkeep.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_ctrl.h"
//==============================
// WiFi 配置
//==============================
#define MY_WIFI_SSID "TYZX"
#define MY_WIFI_PASS "ty20260101"
//==============================
// OTA 配置
//==============================

#define OTA_BUTTON_GPIO GPIO_NUM_0

#define OTA_URL "http://192.168.1.12:8070/OTA.bin"
//==============================
// 日志 TAG
//==============================

static const char*TAG="SYS";
//==============================
// OTA 启动
//==============================

static void trigger_ota(void){
    ESP_LOGI(TAG,
             "Trigger OTA:%s",
             OTA_URL);

/*确保在启动 OTA 前 Wi-Fi 网络栈已准备并已连接（防止在未初始化 tcpip stack 时调用 getaddrinfo 导致 panic）*/
    if(!wifi_manager_connect_blocking(MY_WIFI_SSID,MY_WIFI_PASS,15000))    {
        ESP_LOGW(TAG,"WiFi not connected,abort OTA");
        return;
    }

    ota_update_start_bg(OTA_URL,NULL);
}
//==============================
// OTA 按键回调
//==============================

static void ota_button_cb(gpio_num_t gpio,uint32_t level){
// 按下触发
    if(level==0)    {

        ESP_LOGI(TAG,
                 "OTA button pressed");

        trigger_ota();
    }
}
//==============================
// app_main
//==============================

void app_main(void){
    ESP_LOGI(TAG,
             "System boot");
//==================================================
// 初始化电源保持
//==================================================
    if(pwrkeep_init() !=ESP_OK)    {

        ESP_LOGE(TAG,
                 "pwrkeep init failed");

        return;
    }

    ESP_LOGI(TAG,
             "pwrkeep init success");
//==================================================
// 硬件初始化
//==================================================

    hw_init();

    ESP_LOGI(TAG,
             "hardware init done");
//==================================================
// WiFi 提示
//==================================================

    ESP_LOGI(TAG,
             "Bluetooth OTA disabled");

    ESP_LOGI(TAG,
             "Use BOOT key for WiFi OTA");
//==================================================
// 注册 OTA 按键
//==================================================

    esp_err_t ret;

    ret=gpio_drv_register_callback(
        OTA_BUTTON_GPIO,
        ota_button_cb);

    if(ret !=ESP_OK)    {

        ESP_LOGW(TAG,
                 "OTA button register failed");
    }
    else
    {

        ESP_LOGI(TAG,
                 "OTA button registered on GPIO%d",
                 OTA_BUTTON_GPIO);
    }
//==================================================
// 主循环
//==================================================
    while(1)    {
        ESP_LOGI(TAG,
                 "Main alive");
        ESP_LOGI(TAG,
                 "Stack remain:%d",
                 uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}