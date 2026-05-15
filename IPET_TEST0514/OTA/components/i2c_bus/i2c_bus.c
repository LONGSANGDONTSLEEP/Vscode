#include "i2c_bus.h"

#include "esp_log.h"

static const char *TAG = "i2c_bus";

/**
 * @brief 初始化 I2C 总线
 */
esp_err_t i2c_bus_init(i2c_master_bus_handle_t *bus_handle, 
                       i2c_port_t dev_num, 
                       int sda_io, 
                       int scl_io, 
                       uint32_t freq_hz)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = dev_num,
        .sda_io_num = sda_io,
        .scl_io_num = scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    // 创建 I2C 主总线
    esp_err_t ret = i2c_new_master_bus(&bus_config, bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2C 总线初始化成功");
    return ESP_OK;
}

/**
 * @brief 删除 I2C 总线
 */
esp_err_t i2c_bus_deinit(i2c_master_bus_handle_t bus_handle)
{
    // 删除 I2C 主总线
    esp_err_t ret = i2c_del_master_bus(bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线删除失败: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2C 总线删除成功");
    return ESP_OK;
}

/**
 * @brief 向 I2C 总线添加设备
 */
esp_err_t i2c_bus_add_device(i2c_master_bus_handle_t bus_handle, 
                             uint8_t dev_addr, 
                             i2c_master_dev_handle_t *dev_handle)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, // 7位地址
        .device_address = dev_addr,
        .scl_speed_hz = 400000,                // 默认400kHz
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加 I2C 设备失败: 0x%02X, err=%d", dev_addr, ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2C 设备添加成功: 0x%02X", dev_addr);
    return ESP_OK;
}

/**
 * @brief 从 I2C 总线删除设备
 */
esp_err_t i2c_bus_remove_device(i2c_master_dev_handle_t dev_handle)
{
    esp_err_t ret = i2c_master_bus_rm_device(dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "删除 I2C 设备失败, err=%d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2C 设备删除成功");
    return ESP_OK;
}