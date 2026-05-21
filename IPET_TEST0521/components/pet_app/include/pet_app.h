#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动项目应用层。
 *
 * 前置条件：调用前必须已经完成 hw_init()，因为本模块会使用
 * hwinit_get_i2c_bus()、GPIO 驱动、SD 卡配置文件等硬件资源。
 */
esp_err_t pet_app_start(void);

#ifdef __cplusplus
}
#endif
