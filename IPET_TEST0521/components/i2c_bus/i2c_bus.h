#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdint.h>

#include "esp_err.h"

#include "driver/i2c_master.h"




/*示例用法：
i2c_master_bus_handle_t bus;
i2c_master_dev_handle_t dev;
// 初始化总线 SDA=21, SCL=22, 400kHz
i2c_bus_init(&bus, I2C_NUM_0, 21, 22, 400000);
// 添加设备 0x68
i2c_bus_add_device(bus, 0x68, &dev);
// 使用完删除设备
i2c_bus_remove_device(dev);
// 删除总线
i2c_bus_deinit(bus);
*/


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2C 总线
 * @param bus_handle 输出：I2C 总线句柄
 * @param dev_num I2C 端口号，通常 I2C_NUM_0 或 I2C_NUM_1
 * @param sda_io SDA 引脚号
 * @param scl_io SCL 引脚号
 * @param freq_hz I2C 时钟频率
 * @return esp_err_t 错误码
 */
esp_err_t i2c_bus_init(i2c_master_bus_handle_t *bus_handle, 
                       i2c_port_t dev_num, 
                       int sda_io, 
                       int scl_io, 
                       uint32_t freq_hz);

/**
 * @brief 删除 I2C 总线
 * @param bus_handle I2C 总线句柄
 * @return esp_err_t 错误码
 */
esp_err_t i2c_bus_deinit(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 向 I2C 总线添加设备
 * @param bus_handle I2C 总线句柄
 * @param dev_addr 设备 I2C 地址
 * @param dev_handle 输出：I2C 设备句柄
 * @return esp_err_t 错误码
 */
esp_err_t i2c_bus_add_device(i2c_master_bus_handle_t bus_handle, 
                             uint8_t dev_addr, 
                             i2c_master_dev_handle_t *dev_handle);

/**
 * @brief 从 I2C 总线删除设备
 * @param dev_handle 设备句柄
 * @return esp_err_t 错误码
 */
esp_err_t i2c_bus_remove_device(i2c_master_dev_handle_t dev_handle);

#ifdef __cplusplus
}
#endif

#endif // I2C_BUS_H