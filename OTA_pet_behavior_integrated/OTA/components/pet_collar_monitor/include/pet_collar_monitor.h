#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "pet_behavior.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t qmi8658a_addr;          // 默认 0x6B
    uint32_t sample_period_ms;      // 默认 20ms，即 50Hz
    uint32_t task_stack_size;       // 默认 4096
    uint32_t task_priority;         // 默认 5
    bool enable_gyro_calibration;   // 默认 true
} pet_collar_monitor_config_t;

esp_err_t pet_collar_monitor_start(const pet_collar_monitor_config_t *cfg);
esp_err_t pet_collar_monitor_stop(void);
bool pet_collar_monitor_get_last_result(pet_behavior_result_t *out);

#ifdef __cplusplus
}
#endif
