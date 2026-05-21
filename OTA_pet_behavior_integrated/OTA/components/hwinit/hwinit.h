#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化硬件外设：电源保持、GPIO、I2C、SD 卡、LED。
 */
void hw_init(void);

/**
 * @brief 打印当前任务栈余量。
 */
void check_stack(void);

/**
 * @brief 获取 hw_init() 创建好的 I2C 总线句柄。
 */
i2c_master_bus_handle_t hwinit_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif