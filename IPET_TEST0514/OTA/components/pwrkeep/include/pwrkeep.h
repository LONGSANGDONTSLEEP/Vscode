// pwrkeep.h

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电源保持模块
 *
 * 功能：
 * 1. 拉高 PWR_KEEP 保持供电
 * 2. 启动按键检测任务
 * 3. 支持长按自动关机
 */
esp_err_t pwrkeep_init(void);

/**
 * @brief 手动控制电源保持
 *
 * @param enable
 * true  = 保持供电
 * false = 断电
 */
void pwrkeep_set_hold(bool enable);

#ifdef __cplusplus
}
#endif