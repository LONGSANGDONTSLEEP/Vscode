#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_behavior.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_POWER_LED_RGB_HEX_LEN 8

typedef enum {
    PET_SYSTEM_LED_MODE_BOOT = 0,
    PET_SYSTEM_LED_MODE_ONLINE,
    PET_SYSTEM_LED_MODE_OFFLINE,
    PET_SYSTEM_LED_MODE_RECORDING,
    PET_SYSTEM_LED_MODE_OTA,
    PET_SYSTEM_LED_MODE_POWER_KEY,
} pet_system_led_mode_t;

typedef struct {
    uint8_t led_brightness;             /* 全局亮度 0~255，默认 40 */
    uint32_t system_led_period_ms;      /* 第二颗系统灯刷新周期，默认 100ms */
} pet_power_led_config_t;

typedef struct {
    pet_state_t behavior_state;

    uint8_t state_led_r;
    uint8_t state_led_g;
    uint8_t state_led_b;
    char state_led_name[28];
    char state_led_hex[PET_POWER_LED_RGB_HEX_LEN];

    pet_system_led_mode_t system_mode;
    bool network_online;
    bool recording_active;
    bool ota_active;
    bool power_key_override;
    uint32_t boot_start_ms;

    uint8_t system_led_r;
    uint8_t system_led_g;
    uint8_t system_led_b;
    char system_led_name[32];
    char system_led_hex[PET_POWER_LED_RGB_HEX_LEN];
} pet_power_led_status_t;

void pet_power_led_default_config(pet_power_led_config_t *cfg);
esp_err_t pet_power_led_start(const pet_power_led_config_t *cfg);
void pet_power_led_stop(void);

/* 第一颗 LED：当前行为状态。 */
void pet_power_led_set_behavior_state(pet_state_t state);

/* 第二颗 LED：系统状态。优先级：电源键 > OTA红闪 > 复位蓝闪5秒 > 离线/发送失败红色 > 在线绿闪。 */
void pet_power_led_set_network_online(bool online);
/* /pet HTTP 发送成功/失败。WiFi 在线但服务器不可达时，第二颗 LED 也会显示红色。 */
void pet_power_led_set_server_reachable(bool reachable);
void pet_power_led_set_recording_active(bool active);
void pet_power_led_set_ota_active(bool active);

/* 按住电源键时临时覆盖第二颗 LED 为红色渐灭，松开后恢复系统状态灯。 */
void pet_power_led_set_power_key_override(bool active, uint8_t red_brightness);

bool pet_power_led_get_status(pet_power_led_status_t *out);
const char *pet_power_led_system_mode_to_str(pet_system_led_mode_t mode);

#ifdef __cplusplus
}
#endif
