#include "gpio_drv.h"

#include "sdkconfig.h"

#include "esp_log.h"

#include "esp_err.h"

#include "esp_intr_alloc.h"

#include "freertos/task.h"
#include "driver/ledc.h"
#include "freertos/semphr.h"

#define TAG_GPIO "GPIO_DRV"

/*
    gpio_drv_set_level(CONFIG_WS2812B_GPIO, 0);
    ESP_LOGI(TAG, "LED OFF - GPIO %d", CONFIG_WS2812B_GPIO);  // 添加日志确认
    vTaskDelay(pdMS_TO_TICKS(5000));
*/

/* ============================================================
   GPIO配置数组（使用 ESP-IDF gpio_config_t 类型）
   ============================================================ */
gpio_config_t gpio_config_array[] = {
    {CONFIG_GPIO_OUTPUT_0, GPIO_MODE_OUTPUT, 0, 0, 0}, // 示例GPIO配置
    {CONFIG_GPIO_OUTPUT_1, GPIO_MODE_OUTPUT, 0, 0, 0},
    {CONFIG_GPIO_INPUT_0, GPIO_MODE_INPUT, 0, 1, GPIO_INTR_POSEDGE}, // 输入GPIO，使用上升沿中断
    {CONFIG_GPIO_INPUT_1, GPIO_MODE_INPUT, 0, 1, GPIO_INTR_POSEDGE},
    
    // 更多GPIO配置
};

/* ============================================================
   GPIO事件回调函数数组
   ============================================================ */
#define GPIO_MAX_CALLBACKS 48
static gpio_irq_cb_t gpio_callbacks[GPIO_MAX_CALLBACKS] = {0};

/* ============================================================
   GPIO事件队列 / 回调队列
   ============================================================ */
QueueHandle_t gpio_evt_queue = NULL; // 去掉static，确保它是全局可访问的

typedef struct {
    gpio_num_t gpio;
    uint32_t level;
} gpio_cb_item_t;

static QueueHandle_t gpio_cb_queue = NULL; // 用于把事件交给回调工作任务处理，避免在事件任务中执行重负载代码

/* ============================================================
   GPIO中断服务函数（ISR）
   ============================================================ */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

/* ============================================================
   GPIO事件处理任务
   ============================================================ */
static void gpio_event_task(void *arg)
{
    uint32_t io_num;
    while (1)
    {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
        {
            uint32_t level = gpio_get_level(io_num);

            // 把回调请求放入回调队列，由专门的回调任务执行（该任务有更大的栈）
            if (gpio_cb_queue) {
                gpio_cb_item_t it = { .gpio = (gpio_num_t)io_num, .level = level };
                // 阻塞直到入队成功，确保回调不会丢失（回调任务应当及时消费）
                if (xQueueSend(gpio_cb_queue, &it, portMAX_DELAY) != pdPASS) {
                    ESP_LOGW(TAG_GPIO, "Failed to enqueue gpio callback for gpio %d", io_num);
                    // 作为最后的后备，直接调用回调以免丢事件
                    if (gpio_callbacks[io_num]) {
                        gpio_callbacks[io_num](io_num, level);
                    }
                }
            } else {
                // 如果回调队列尚未创建，则回退到直接调用
                if (gpio_callbacks[io_num]) {
                    gpio_callbacks[io_num](io_num, level);
                }
            }
        }
    }
}

static void gpio_callback_task(void *arg)
{
    gpio_cb_item_t it;
    while (1) {
        if (xQueueReceive(gpio_cb_queue, &it, portMAX_DELAY) == pdPASS) {
            if ((int)it.gpio >= 0 && it.gpio < GPIO_MAX_CALLBACKS && gpio_callbacks[it.gpio]) {
                gpio_callbacks[it.gpio](it.gpio, it.level);
            }
        }
    }
}

/* ===================== LED 控制实现 ===================== */
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL_BASE LEDC_CHANNEL_0

typedef struct {
    gpio_num_t gpio;
    uint8_t duty_percent;
    uint32_t blink_ms; /* 0 表示常亮 */
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    int channel;
    bool active_low;
} led_ctrl_t;

/* 仅支持有限数量的并发 led 控制 */
#define LED_CTRL_MAX 4
static led_ctrl_t led_ctrls[LED_CTRL_MAX] = {0};

static int find_free_led_ctrl(void)
{
    for (int i = 0; i < LED_CTRL_MAX; ++i) {
        if (led_ctrls[i].gpio == -1) return i;
    }
    return -1;
}

static int find_led_ctrl_by_gpio(gpio_num_t gpio)
{
    for (int i = 0; i < LED_CTRL_MAX; ++i) {
        if (led_ctrls[i].gpio == gpio) return i;
    }
    return -1;
}

/* ============================================================
   GPIO驱动初始化
   ============================================================ */
esp_err_t gpio_drv_init(void)
{
    /* 简化：不再通过静态数组批量配置引脚。
       在注册回调时会单独配置相应 GPIO（gpio_drv_register_callback）。
       这里只创建事件队列/任务并安装 ISR 服务。 */
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    if (gpio_evt_queue == NULL)
    {
        ESP_LOGE(TAG_GPIO, "GPIO queue create failed");
        return ESP_FAIL;
    }

    /* 创建事件处理任务 */
    xTaskCreate(gpio_event_task, "gpio_evt_task", 2048, NULL, 10, NULL);

    /* 创建回调队列与回调任务（更大的栈以避免栈溢出） */
    gpio_cb_queue = xQueueCreate(10, sizeof(gpio_cb_item_t));
    if (gpio_cb_queue == NULL) {
        ESP_LOGE(TAG_GPIO, "GPIO callback queue create failed");
        return ESP_FAIL;
    }
    xTaskCreate(gpio_callback_task, "gpio_cb_task", 4096, NULL, 10, NULL);

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1)); // 中断分配相关定义在 esp_intr_alloc.h

    /* 初始化内部 LED 控制表为未使用状态 */
    for (int i = 0; i < LED_CTRL_MAX; ++i) {
        led_ctrls[i].gpio = -1;
        led_ctrls[i].lock = NULL;
        led_ctrls[i].task = NULL;
        led_ctrls[i].channel = -1;
    }

    ESP_LOGI(TAG_GPIO, "GPIO driver init success");
    return ESP_OK;
}

/* ============================================================
   GPIO输出控制接口
   ============================================================ */
void gpio_drv_set_level(gpio_num_t gpio_num, uint32_t level)
{
    gpio_set_level(gpio_num, level);
}

esp_err_t gpio_drv_register_callback(gpio_num_t gpio, gpio_irq_cb_t cb)
{
    if (gpio < 0 || gpio >= GPIO_MAX_CALLBACKS) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_callbacks[gpio] = cb;

    /* 配置该 GPIO 为输入并使能中断（默认上拉） */
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = (1ULL << gpio);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    /* 为该 GPIO 添加 ISR 回调，arg 传入实际 gpio 编号 */
    gpio_isr_handler_add(gpio, gpio_isr_handler, (void *)gpio);
    return ESP_OK;
}


static void led_blink_task(void *arg)
{
    led_ctrl_t *c = (led_ctrl_t *)arg;
    bool ledc_attached = true; /* 在开始时，gpio_drv_led_start 已配置 LEDC 通道 */
    const uint32_t max_duty = (1 << LEDC_TIMER_13_BIT) - 1; /* 8191 */

    while (1) {
        xSemaphoreTake(c->lock, portMAX_DELAY);
        uint32_t blink = c->blink_ms;
        uint8_t duty = c->duty_percent;
        bool active_low = c->active_low;
        xSemaphoreGive(c->lock);

        if (blink == 0) {
            /* 常亮：确保 LEDC 已附加并写入占空 */
            if (!ledc_attached) {
                ledc_channel_config_t ch_conf = {
                    .gpio_num = c->gpio,
                    .speed_mode = LEDC_MODE,
                    .channel = c->channel,
                    .timer_sel = LEDC_TIMER,
                    .intr_type = LEDC_INTR_DISABLE,
                    .duty = 0,
                    .hpoint = 0
                };
                ledc_channel_config(&ch_conf);
                ledc_attached = true;
            }

            uint32_t on_val = active_low ? ((100 - duty) * max_duty) / 100 : (duty * max_duty) / 100;
            ledc_set_duty(LEDC_MODE, c->channel, on_val);
            ledc_update_duty(LEDC_MODE, c->channel);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* 闪烁：改为使用数字开/关切换以保证明显的开/关效果（而不是 PWM 亮度过渡）
           如果你需要基于亮度的闪烁（柔和渐变），我可以添加一个开关来启用 PWM 闪烁 */
        {
            int on_level = active_low ? 0 : 1;
            int off_level = 1 - on_level;

            if (ledc_attached) {
                /* 停止 LEDC 并把输出设置为 off_level */
                ledc_stop(LEDC_MODE, c->channel, off_level);
                ledc_attached = false;
            }

            /* 确保把引脚设为 GPIO 输出，再用 gpio_set_level 做严格的开/关切换 */
            gpio_set_direction(c->gpio, GPIO_MODE_OUTPUT);
            gpio_set_level(c->gpio, on_level);
            vTaskDelay(pdMS_TO_TICKS(blink/2));
            gpio_set_level(c->gpio, off_level);
            vTaskDelay(pdMS_TO_TICKS(blink/2));
            continue;
        }
    }
}

esp_err_t gpio_drv_led_start(gpio_num_t gpio, uint8_t duty_percent, uint32_t blink_ms, bool active_low)
{
    if (duty_percent > 100) return ESP_ERR_INVALID_ARG;

    int idx = find_led_ctrl_by_gpio(gpio);
    if (idx >= 0) {
        /* 已存在，更新参数 */
        led_ctrl_t *c = &led_ctrls[idx];
        xSemaphoreTake(c->lock, portMAX_DELAY);
        c->duty_percent = duty_percent;
        c->blink_ms = blink_ms;
        xSemaphoreGive(c->lock);
        return ESP_OK;
    }

    idx = find_free_led_ctrl();
    if (idx < 0) return ESP_ERR_NO_MEM;

    led_ctrl_t *c = &led_ctrls[idx];
    c->gpio = gpio;
    c->duty_percent = duty_percent;
    c->active_low = active_low;
    c->blink_ms = blink_ms;
    c->lock = xSemaphoreCreateMutex();
    c->channel = LEDC_CHANNEL_BASE + idx;

    /* 配置 LEDC 定时器/通道（简化，使用默认频率与分辨率） */
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num = gpio,
        .speed_mode = LEDC_MODE,
        .channel = c->channel,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ch_conf);

    xTaskCreate(led_blink_task, "led_blink", 2048, c, 5, &c->task);
    return ESP_OK;
}

esp_err_t gpio_drv_led_set_brightness(gpio_num_t gpio, uint8_t duty_percent)
{
    int idx = find_led_ctrl_by_gpio(gpio);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    led_ctrl_t *c = &led_ctrls[idx];
    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->duty_percent = duty_percent;
    xSemaphoreGive(c->lock);
    return ESP_OK;
}

esp_err_t gpio_drv_led_set_blink_interval(gpio_num_t gpio, uint32_t blink_ms)
{
    int idx = find_led_ctrl_by_gpio(gpio);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    led_ctrl_t *c = &led_ctrls[idx];
    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->blink_ms = blink_ms;
    xSemaphoreGive(c->lock);
    return ESP_OK;
}

esp_err_t gpio_drv_led_stop(gpio_num_t gpio)
{
    int idx = find_led_ctrl_by_gpio(gpio);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    led_ctrl_t *c = &led_ctrls[idx];
    // TODO: 删除任务并释放资源（简单实现不释放）
    c->gpio = -1;
    return ESP_OK;
}