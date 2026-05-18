#include "led_ctrl.h"
#include "gpio_drv.h"
#include "esp_log.h"

static const char *TAG = "led_ctrl";

void led_ctrl_demo_start(void)
{
    // 使用 GPIO48，100% 亮度，每1000ms闪烁（确保闪烁为明显开/关切换）
    // GPIO48 为低电平点亮（active_low）
    gpio_drv_led_start(GPIO_NUM_48, 100, 5000, true);
    ESP_LOGI(TAG, "LED demo started on GPIO48: 100%% brightness, 1s blink");
}
