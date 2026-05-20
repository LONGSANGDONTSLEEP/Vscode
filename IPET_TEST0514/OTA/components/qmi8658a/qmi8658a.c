#include "qmi8658a.h"
#include <string.h>
#include "esp_log.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "QMI8658A";

static i2c_master_dev_handle_t s_dev = NULL;

// Helper: parse little-endian 16-bit (low byte first)
static inline int16_t le16(uint8_t lo, uint8_t hi)
{
    return (int16_t)((uint16_t)hi << 8 | lo);
}

// QMI8658A I2C address (user provided)
#define QMI8658A_ADDR 0x6B

// Registers (per datasheet)
#define QMI8658A_REG_WHO_AM_I    0x00
#define QMI8658A_REG_CTRL1       0x02
#define QMI8658A_REG_CTRL2       0x03
#define QMI8658A_REG_CTRL3       0x04
#define QMI8658A_REG_CTRL5       0x06
#define QMI8658A_REG_CTRL7       0x08
#define QMI8658A_REG_TEMP_L      0x33
#define QMI8658A_REG_TEMP_H      0x34
#define QMI8658A_REG_AX_L        0x35
#define QMI8658A_REG_GZ_H        0x40
#define QMI8658A_REG_RESET       0x60

// Initialization values chosen (can be adjusted):
#define QMI8658A_RESET_CMD      0xB0
#define QMI8658A_CTRL1_ADDR_AI  (1 << 6) // address auto-increment, little-endian (BE=0)
// CTRL2: accel FS = ±8g (aFS=010 -> <<4 = 0x02<<4 = 0x20) and aODR choose 500Hz (0x04)
#define QMI8658A_CTRL2_VALUE    ((0x02 << 4) | 0x04) // ±8g, 500Hz
// CTRL3: gyro FS = ±512 dps (0x05<<4) and gODR = 448Hz (0x04)
#define QMI8658A_CTRL3_VALUE    ((0x05 << 4) | 0x04) // ±512 dps, 448Hz
// CTRL5: enable low-pass filters for both accel & gyro
#define QMI8658A_CTRL5_VALUE    ( (1<<4) | (1<<0) ) // gLPF_EN | aLPF_EN
// CTRL7: enable accel + gyro
#define QMI8658A_CTRL7_VALUE    ( (1<<1) | (1<<0) ) // gEN | aEN

// Scale LSB per unit for chosen FS
#define QMI8658A_ACCEL_LSB_PER_G 4096.0f  // ±8g -> 4096 LSB/g (16-bit)
#define QMI8658A_GYRO_LSB_PER_DPS 64.0f   // ±512 dps -> 64 LSB/dps

static void qmi_read_task(void *arg)
{
    float accel[3];
    float gyro[3];
    float temp;
    while (1) {
        if (qmi8658a_read_all(accel, gyro, &temp) == ESP_OK) {
            ESP_LOGI(TAG, "Accel: %.3f, %.3f, %.3f g", accel[0], accel[1], accel[2]);
            ESP_LOGI(TAG, "Gyro: %.3f, %.3f, %.3f dps", gyro[0], gyro[1], gyro[2]);
        } else {
            ESP_LOGW(TAG, "qmi8658a read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 简单的寄存器读写封装（使用 esp-driver-i2c 提供的接口）
static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    /* 使用 esp_driver_i2c 提供的接口直接调用，参数按头文件要求传入超时时间（ms） */
    uint8_t tmp[2] = { reg, val };
    return i2c_master_transmit(s_dev, (const uint8_t *)tmp, sizeof(tmp), 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    /* 先写寄存器地址再读数据，超时时间传入 ms */
    return i2c_master_transmit_receive(s_dev, (const uint8_t *)&reg, 1, (uint8_t *)buf, len, 100);
}

esp_err_t qmi8658a_init(i2c_master_bus_handle_t bus, int sda_io, int scl_io)
{
    esp_err_t ret;
    // 添加设备到总线
    ret = i2c_bus_add_device(bus, QMI8658A_ADDR, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "device handle s_dev=%p", s_dev);

    // 尝试探测设备：读取寄存器 0x00 以确认设备响应
    uint8_t probe_val = 0;
    esp_err_t probe_ret = read_regs(0x00, &probe_val, 1);
    if (probe_ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C probe failed for addr 0x%02X: %s", QMI8658A_ADDR, esp_err_to_name(probe_ret));
        // 如果探测失败，移除设备句柄以清理状态
        i2c_bus_remove_device(s_dev);
        s_dev = NULL;
        return probe_ret;
    } else {
        ESP_LOGI(TAG, "I2C probe OK for addr 0x%02X, reg0=0x%02X", QMI8658A_ADDR, probe_val);
    }

    // 复位并初始化传感器寄存器
    // Soft reset bank select to bank0 if needed (register 0x7F used in some examples)
    write_reg(0x7F, 0x00); // bank0
    vTaskDelay(pdMS_TO_TICKS(5));

    // 软复位（使用 datasheet 建议的 RESET 命令）
    write_reg(QMI8658A_REG_RESET, QMI8658A_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));

    // CTRL1: 地址自动递增, Little-Endian
    write_reg(QMI8658A_REG_CTRL1, QMI8658A_CTRL1_ADDR_AI);

    // CTRL2: 加速度配置（由宏定义）
    write_reg(QMI8658A_REG_CTRL2, QMI8658A_CTRL2_VALUE);

    // CTRL3: 陀螺配置（由宏定义）
    write_reg(QMI8658A_REG_CTRL3, QMI8658A_CTRL3_VALUE);

    // CTRL5: 低通滤波器使能
    write_reg(QMI8658A_REG_CTRL5, QMI8658A_CTRL5_VALUE);

    // CTRL7: 使能加速度与陀螺
    write_reg(QMI8658A_REG_CTRL7, QMI8658A_CTRL7_VALUE);

    // 给传感器留出启动时间（Gyro 启动典型需要 ~150ms）
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "qmi8658a init done");
    return ESP_OK;
}

esp_err_t qmi8658a_read_all(float *accel_g, float *gyro_dps, float *temp_c)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[14];
    // 按数据手册，从 TEMP_L (0x33) 连续读 14 字节：TEMP_L/H, AX_L/H, AY_L/H, AZ_L/H, GX_L/H, GY_L/H, GZ_L/H
    esp_err_t ret = read_regs(QMI8658A_REG_TEMP_L, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    ESP_LOGD(TAG, "raw data:");
    for (int i = 0; i < sizeof(buf); i++) {
        if (i % 8 == 0) ESP_LOGD(TAG, "");
        ESP_LOGD(TAG, " %02X", buf[i]);
    }

    // 解析为 little-endian 16-bit 值：低字节在前（使用文件作用域 le16 helper）
    int16_t raw_temp = le16(buf[0], buf[1]);
    int16_t raw_ax   = le16(buf[2], buf[3]);
    int16_t raw_ay   = le16(buf[4], buf[5]);
    int16_t raw_az   = le16(buf[6], buf[7]);
    int16_t raw_gx   = le16(buf[8], buf[9]);
    int16_t raw_gy   = le16(buf[10], buf[11]);
    int16_t raw_gz   = le16(buf[12], buf[13]);

    if (temp_c) {
        // 根据手册：T = TEMP_H + (TEMP_L / 256)
        *temp_c = raw_temp / 256.0f;
    }

    if (accel_g) {
        accel_g[0] = raw_ax / QMI8658A_ACCEL_LSB_PER_G;
        accel_g[1] = raw_ay / QMI8658A_ACCEL_LSB_PER_G;
        accel_g[2] = raw_az / QMI8658A_ACCEL_LSB_PER_G;
    }
    if (gyro_dps) {
        gyro_dps[0] = raw_gx / QMI8658A_GYRO_LSB_PER_DPS;
        gyro_dps[1] = raw_gy / QMI8658A_GYRO_LSB_PER_DPS;
        gyro_dps[2] = raw_gz / QMI8658A_GYRO_LSB_PER_DPS;
    }

    return ESP_OK;
}

esp_err_t qmi8658a_start(i2c_master_bus_handle_t bus, int sda_io, int scl_io)
{
    esp_err_t ret = qmi8658a_init(bus, sda_io, scl_io);
    if (ret != ESP_OK) return ret;

    BaseType_t r = xTaskCreatePinnedToCore(
        qmi_read_task,
        "qmi_read",
        2048,
        NULL,
        5,
        NULL,
        tskNO_AFFINITY);

    if (r != pdPASS) {
        ESP_LOGW(TAG, "failed to create qmi_read task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t qmi8658a_i2c_scan(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "Starting I2C scan on bus");
    for (uint8_t addr = 1; addr < 0x7F; addr++) {
        i2c_master_dev_handle_t tmpdev;
        esp_err_t r = i2c_bus_add_device(bus, addr, &tmpdev);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "Found device at 0x%02X", addr);
            i2c_bus_remove_device(tmpdev);
        }
    }
    ESP_LOGI(TAG, "I2C scan done");
    return ESP_OK;
}
