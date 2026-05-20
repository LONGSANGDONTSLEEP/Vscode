#ifndef QMI8658A_H
#define QMI8658A_H

#include <stdint.h>
#include "esp_err.h"
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t qmi8658a_init(i2c_master_bus_handle_t bus, int sda_io, int scl_io);
esp_err_t qmi8658a_read_all(float *accel_g, float *gyro_dps, float *temp_c);
// 初始化并启动周期读取任务（内部会添加设备并创建任务）
esp_err_t qmi8658a_start(i2c_master_bus_handle_t bus, int sda_io, int scl_io);

// 调试：在组件里提供一个 I2C 扫描函数，方便定位设备是否存在
esp_err_t qmi8658a_i2c_scan(i2c_master_bus_handle_t bus);

#ifdef __cplusplus
}
#endif

#endif // QMI8658A_H
