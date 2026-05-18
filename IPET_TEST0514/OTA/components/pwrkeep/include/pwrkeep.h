// pwrkeep.h - 电源保持与按键长按关机组件
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化电源保持，并启动按键检测任务
// - 上电后立即将 PWRKEEP(GPIO 配置项) 拉高以维持供电
// - 运行时检测按键长按，达到阈值后将 PWRKEEP 拉低以关闭电源
esp_err_t pwrkeep_init(void);

// 手动控制保持引脚（高=保持，低=释放）
void pwrkeep_set_hold(bool enable);

#ifdef __cplusplus
}
#endif
