#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMI8658A_I2C_ADDR_HIGH      0x6A
#define QMI8658A_I2C_ADDR_LOW       0x6B
#define QMI8658A_WHO_AM_I_VALUE     0x05

typedef enum {
    QMI8658A_ACCEL_FS_2G = 0,
    QMI8658A_ACCEL_FS_4G,
    QMI8658A_ACCEL_FS_8G,
    QMI8658A_ACCEL_FS_16G,
} qmi8658a_accel_fs_t;

typedef enum {
    QMI8658A_GYRO_FS_16DPS = 0,
    QMI8658A_GYRO_FS_32DPS,
    QMI8658A_GYRO_FS_64DPS,
    QMI8658A_GYRO_FS_128DPS,
    QMI8658A_GYRO_FS_256DPS,
    QMI8658A_GYRO_FS_512DPS,
    QMI8658A_GYRO_FS_1024DPS,
    QMI8658A_GYRO_FS_2048DPS,
} qmi8658a_gyro_fs_t;

typedef enum {
    QMI8658A_ACC_ODR_1000HZ   = 0x03,
    QMI8658A_ACC_ODR_500HZ    = 0x04,
    QMI8658A_ACC_ODR_250HZ    = 0x05,
    QMI8658A_ACC_ODR_125HZ    = 0x06,
    QMI8658A_ACC_ODR_62_5HZ   = 0x07,
    QMI8658A_ACC_ODR_31_25HZ  = 0x08,

    // 低功耗 ODR 仅建议在只开加速度计时使用
    QMI8658A_ACC_ODR_LP_128HZ = 0x0C,
    QMI8658A_ACC_ODR_LP_21HZ  = 0x0D,
    QMI8658A_ACC_ODR_LP_11HZ  = 0x0E,
    QMI8658A_ACC_ODR_LP_3HZ   = 0x0F,
} qmi8658a_accel_odr_t;

typedef enum {
    QMI8658A_GYR_ODR_7174HZ = 0x00,
    QMI8658A_GYR_ODR_3587HZ = 0x01,
    QMI8658A_GYR_ODR_1793HZ = 0x02,
    QMI8658A_GYR_ODR_896HZ  = 0x03,
    QMI8658A_GYR_ODR_448HZ  = 0x04,
    QMI8658A_GYR_ODR_224HZ  = 0x05,
    QMI8658A_GYR_ODR_112HZ  = 0x06,
    QMI8658A_GYR_ODR_56HZ   = 0x07,
    QMI8658A_GYR_ODR_28HZ   = 0x08,
} qmi8658a_gyro_odr_t;

typedef struct {
    i2c_master_bus_handle_t bus;      // 外部已经创建好的 I2C master bus
    uint8_t i2c_addr;                 // 0x6A 或 0x6B；你当前硬件日志为 0x6B
    uint32_t scl_speed_hz;            // 建议 400000

    qmi8658a_accel_fs_t accel_fs;     // 宠物项圈推荐 ±8g
    qmi8658a_gyro_fs_t gyro_fs;       // 宠物项圈推荐 ±512dps
    qmi8658a_accel_odr_t accel_odr;   // 6DOF 下加速度 ODR 会同步到陀螺仪频率体系
    qmi8658a_gyro_odr_t gyro_odr;

    bool enable_lpf;
} qmi8658a_config_t;

typedef struct {
    int16_t ax_raw;
    int16_t ay_raw;
    int16_t az_raw;
    int16_t gx_raw;
    int16_t gy_raw;
    int16_t gz_raw;
    int16_t temp_raw;

    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
    float temp_c;
} qmi8658a_sample_t;

typedef struct qmi8658a_dev_t *qmi8658a_handle_t;

esp_err_t qmi8658a_create(const qmi8658a_config_t *cfg, qmi8658a_handle_t *out_handle);
esp_err_t qmi8658a_delete(qmi8658a_handle_t handle);

esp_err_t qmi8658a_probe(qmi8658a_handle_t handle);
esp_err_t qmi8658a_init(qmi8658a_handle_t handle);
esp_err_t qmi8658a_read_sample(qmi8658a_handle_t handle, qmi8658a_sample_t *out);
esp_err_t qmi8658a_read_whoami(qmi8658a_handle_t handle, uint8_t *whoami);

esp_err_t qmi8658a_calibrate_gyro_bias(qmi8658a_handle_t handle, uint32_t samples, uint32_t delay_ms);
void qmi8658a_set_gyro_bias(qmi8658a_handle_t handle, float bx_dps, float by_dps, float bz_dps);
void qmi8658a_get_gyro_bias(qmi8658a_handle_t handle, float *bx_dps, float *by_dps, float *bz_dps);

#ifdef __cplusplus
}
#endif
