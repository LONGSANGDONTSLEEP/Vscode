#include "pet_power_led.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "led_ctrl.h"

#define TAG "PET_LED"

#define PET_LED_STATE_IDX  0
#define PET_LED_SYSTEM_IDX 1

#define DEFAULT_BRIGHTNESS 40U
#define DEFAULT_SYSTEM_PERIOD_MS 100U

static pet_power_led_config_t s_cfg;
static pet_power_led_status_t s_status;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_started;
static bool s_wifi_connected;
static bool s_server_reachable;
static uint32_t s_phase;

static uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint8_t scale(uint8_t v)
{
    uint16_t out = ((uint16_t)v * (uint16_t)s_cfg.led_brightness) / 255U;
    return (uint8_t)(out > 255U ? 255U : out);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    const char *in = src ? src : "";
    size_t n = strlen(in);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, in, n);
    dst[n] = '\0';
}

static void rgb_to_hex(uint8_t r, uint8_t g, uint8_t b, char out[PET_POWER_LED_RGB_HEX_LEN])
{
    snprintf(out, PET_POWER_LED_RGB_HEX_LEN, "#%02X%02X%02X", r, g, b);
}

const char *pet_power_led_system_mode_to_str(pet_system_led_mode_t mode)
{
    switch (mode) {
    case PET_SYSTEM_LED_MODE_BOOT:      return "BOOT";
    case PET_SYSTEM_LED_MODE_ONLINE:    return "ONLINE";
    case PET_SYSTEM_LED_MODE_OFFLINE:   return "OFFLINE";
    case PET_SYSTEM_LED_MODE_RECORDING: return "RECORDING";
    case PET_SYSTEM_LED_MODE_OTA:       return "OTA";
    case PET_SYSTEM_LED_MODE_POWER_KEY: return "POWER_KEY";
    default:                            return "UNKNOWN";
    }
}

static void set_state_status_rgb_locked(const char *name, uint8_t r, uint8_t g, uint8_t b)
{
    s_status.state_led_r = r;
    s_status.state_led_g = g;
    s_status.state_led_b = b;
    copy_text(s_status.state_led_name, sizeof(s_status.state_led_name), name ? name : "state");
    rgb_to_hex(r, g, b, s_status.state_led_hex);
}

static void set_system_status_rgb_locked(pet_system_led_mode_t mode,
                                         const char *name,
                                         uint8_t r,
                                         uint8_t g,
                                         uint8_t b)
{
    s_status.system_mode = mode;
    s_status.system_led_r = r;
    s_status.system_led_g = g;
    s_status.system_led_b = b;
    copy_text(s_status.system_led_name, sizeof(s_status.system_led_name), name ? name : "system");
    rgb_to_hex(r, g, b, s_status.system_led_hex);
}

static void calc_state_led_locked(const char **name, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /*
     * 第一颗 LED：只表达宠物行为状态。
     * 颜色按产品定义固定：
     * REST=绿，WALK=蓝，RUN=橙，SLEEP=粉，NOT_WORN=红闪，PLAY=蓝/橙闪。
     */
    *name = "UNKNOWN/未知-灭";
    *r = 0;
    *g = 0;
    *b = 0;

    switch (s_status.behavior_state) {
    case PET_STATE_REST:
        *name = "REST/休息-绿";
        *r = 0;
        *g = 255;
        *b = 0;
        break;

    case PET_STATE_WALK:
    case PET_STATE_TROT:
        *name = "WALK/走动-蓝";
        *r = 0;
        *g = 80;
        *b = 255;
        break;

    case PET_STATE_RUN:
        *name = "RUN/跑步-橙";
        *r = 255;
        *g = 100;
        *b = 0;
        break;

    case PET_STATE_SLEEP:
        *name = "SLEEP/睡觉-粉";
        *r = 255;
        *g = 45;
        *b = 170;
        break;

    case PET_STATE_NOT_WORN: {
        bool on = ((s_phase / 5U) % 2U) == 0U; /* 约 500ms 红闪 */
        *name = "NOT_WORN/未佩戴-红闪";
        *r = on ? 255 : 0;
        *g = 0;
        *b = 0;
        break;
    }

    case PET_STATE_PLAY: {
        bool blue = ((s_phase / 4U) % 2U) == 0U; /* 约 400ms 蓝/橙交替 */
        *name = "PLAY/玩耍-蓝橙闪";
        if (blue) {
            *r = 0;
            *g = 80;
            *b = 255;
        } else {
            *r = 255;
            *g = 100;
            *b = 0;
        }
        break;
    }

    case PET_STATE_PASSIVE_MOTION:
        *name = "PASSIVE/被动-青";
        *r = 0;
        *g = 220;
        *b = 255;
        break;

    case PET_STATE_ABNORMAL_INACTIVE:
        *name = "ABNORMAL/异常-红";
        *r = 255;
        *g = 0;
        *b = 0;
        break;

    case PET_STATE_UNKNOWN:
    default:
        break;
    }
}

static void apply_state_led_locked(void)
{
    const char *name = "UNKNOWN/未知-灭";
    uint8_t r = 0, g = 0, b = 0;

    calc_state_led_locked(&name, &r, &g, &b);
    set_state_status_rgb_locked(name, r, g, b);
    led_ctrl_set_led_color(PET_LED_STATE_IDX, scale(r), scale(g), scale(b));
}

static void calc_system_led_locked(uint8_t *r, uint8_t *g, uint8_t *b,
                                   pet_system_led_mode_t *mode,
                                   const char **name)
{
    /*
     * 第二颗 LED：只表达系统状态，尽量简单。
     * 优先级：电源键覆盖 > OTA 下载 > 复位/启动 5 秒蓝闪 > 网络/发送失败红色 > 在线绿闪。
     */
    uint32_t elapsed_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_status.boot_start_ms > 0 && elapsed_ms >= s_status.boot_start_ms) {
        elapsed_ms -= s_status.boot_start_ms;
    } else {
        elapsed_ms = 0;
    }

    if (s_status.power_key_override) {
        *mode = PET_SYSTEM_LED_MODE_POWER_KEY;
        *name = "POWER_KEY/电源键-红渐灭";
        *r = s_status.system_led_r;
        *g = 0;
        *b = 0;
        return;
    }

    if (s_status.ota_active) {
        bool on = ((s_phase / 3U) % 2U) == 0U; /* 约 300ms 红闪：正在下载/OTA */
        *mode = PET_SYSTEM_LED_MODE_OTA;
        *name = "OTA/下载-红闪";
        *r = on ? 255 : 0;
        *g = 0;
        *b = 0;
        return;
    }

    if (elapsed_ms < 5000U) {
        bool on = ((s_phase / 3U) % 2U) == 0U; /* 复位/启动后蓝闪 5 秒 */
        *mode = PET_SYSTEM_LED_MODE_BOOT;
        *name = "BOOT/复位-蓝闪5秒";
        *r = 0;
        *g = 0;
        *b = on ? 255 : 0;
        return;
    }

    if (!s_status.network_online) {
        *mode = PET_SYSTEM_LED_MODE_OFFLINE;
        *name = "OFFLINE/离线或发送失败-红";
        *r = 255;
        *g = 0;
        *b = 0;
        return;
    }

    /* 在线正常：每 2 秒闪一次绿色，表示设备和服务器通信正常。 */
    bool on = (elapsed_ms % 2000U) < 220U;
    *mode = PET_SYSTEM_LED_MODE_ONLINE;
    *name = "ONLINE/正常连接-绿闪";
    *r = 0;
    *g = on ? 255 : 0;
    *b = 0;
}

static void apply_system_led_locked(void)
{
    uint8_t r = 0, g = 0, b = 0;
    pet_system_led_mode_t mode = PET_SYSTEM_LED_MODE_BOOT;
    const char *name = "BOOT/启动";

    calc_system_led_locked(&r, &g, &b, &mode, &name);
    set_system_status_rgb_locked(mode, name, r, g, b);
    led_ctrl_set_led_color(PET_LED_SYSTEM_IDX, scale(r), scale(g), scale(b));
}

static void system_led_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_phase++;
            apply_state_led_locked();
            apply_system_led_locked();
            xSemaphoreGive(s_lock);
        }

        uint32_t delay_ms = s_cfg.system_led_period_ms ? s_cfg.system_led_period_ms : DEFAULT_SYSTEM_PERIOD_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void pet_power_led_default_config(pet_power_led_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->led_brightness = DEFAULT_BRIGHTNESS;
    cfg->system_led_period_ms = DEFAULT_SYSTEM_PERIOD_MS;
}

esp_err_t pet_power_led_start(const pet_power_led_config_t *cfg)
{
    if (s_started) {
        return ESP_OK;
    }

    if (cfg) {
        s_cfg = *cfg;
    } else {
        pet_power_led_default_config(&s_cfg);
    }

    if (s_cfg.led_brightness == 0) {
        s_cfg.led_brightness = DEFAULT_BRIGHTNESS;
    }
    if (s_cfg.system_led_period_ms == 0) {
        s_cfg.system_led_period_ms = DEFAULT_SYSTEM_PERIOD_MS;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.behavior_state = PET_STATE_UNKNOWN;
    s_status.system_mode = PET_SYSTEM_LED_MODE_BOOT;
    s_status.boot_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_wifi_connected = false;
    s_server_reachable = false;
    s_status.network_online = false;
    copy_text(s_status.state_led_name, sizeof(s_status.state_led_name), "UNKNOWN/未知");
    copy_text(s_status.system_led_name, sizeof(s_status.system_led_name), "BOOT/启动");
    rgb_to_hex(0, 0, 0, s_status.state_led_hex);
    rgb_to_hex(0, 0, 0, s_status.system_led_hex);

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    led_ctrl_init(GPIO_NUM_48);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        apply_state_led_locked();
        apply_system_led_locked();
        xSemaphoreGive(s_lock);
    }

    BaseType_t ok = xTaskCreate(system_led_task,
                                "pet_sys_led",
                                2048,
                                NULL,
                                3,
                                &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "system led task create failed");
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "behavior/system LED started");
    return ESP_OK;
}

void pet_power_led_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_started = false;
}

void pet_power_led_set_behavior_state(pet_state_t state)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (s_status.behavior_state != state) {
            s_status.behavior_state = state;
            apply_state_led_locked();
        }
        xSemaphoreGive(s_lock);
    }
}


static void update_effective_network_locked(void)
{
    /* 第二颗 LED 的“在线”必须同时满足：WiFi 已连接 + /pet 最近发送成功。 */
    s_status.network_online = s_wifi_connected && s_server_reachable;
}

void pet_power_led_set_network_online(bool online)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        s_wifi_connected = online;
        if (!online) {
            s_server_reachable = false;
        }
        update_effective_network_locked();
        apply_system_led_locked();
        xSemaphoreGive(s_lock);
    }
}

void pet_power_led_set_server_reachable(bool reachable)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        s_server_reachable = reachable;
        update_effective_network_locked();
        apply_system_led_locked();
        xSemaphoreGive(s_lock);
    }
}

void pet_power_led_set_recording_active(bool active)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        s_status.recording_active = active;
        apply_system_led_locked();
        xSemaphoreGive(s_lock);
    }
}

void pet_power_led_set_ota_active(bool active)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        s_status.ota_active = active;
        apply_system_led_locked();
        xSemaphoreGive(s_lock);
    }
}

void pet_power_led_set_power_key_override(bool active, uint8_t red_brightness)
{
    if (!s_lock) {
        if (active) {
            led_ctrl_set_led_color(PET_LED_SYSTEM_IDX, red_brightness, 0, 0);
        }
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status.power_key_override = active;
        if (active) {
            s_status.system_led_r = red_brightness;
            s_status.system_led_g = 0;
            s_status.system_led_b = 0;
            set_system_status_rgb_locked(PET_SYSTEM_LED_MODE_POWER_KEY,
                                         "POWER_KEY/电源键",
                                         red_brightness,
                                         0,
                                         0);
            led_ctrl_set_led_color(PET_LED_SYSTEM_IDX, red_brightness, 0, 0);
        } else {
            apply_system_led_locked();
        }
        xSemaphoreGive(s_lock);
    }
}

bool pet_power_led_get_status(pet_power_led_status_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30)) == pdTRUE) {
        *out = s_status;
        ok = true;
        xSemaphoreGive(s_lock);
    }
    return ok;
}
