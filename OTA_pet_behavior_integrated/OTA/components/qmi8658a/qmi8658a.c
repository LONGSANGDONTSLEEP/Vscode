#include "qmi8658a.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "QMI8658A"

// Register map
#define QMI8658A_REG_WHO_AM_I       0x00
#define QMI8658A_REG_REVISION_ID    0x01
#define QMI8658A_REG_CTRL1          0x02
#define QMI8658A_REG_CTRL2          0x03
#define QMI8658A_REG_CTRL3          0x04
#define QMI8658A_REG_CTRL5          0x06
#define QMI8658A_REG_CTRL7          0x08
#define QMI8658A_REG_STATUS0        0x2E
#define QMI8658A_REG_TEMP_L         0x33
#define QMI8658A_REG_RESET          0x60

#define QMI8658A_RESET_CMD          0xB0

// CTRL1
#define QMI8658A_CTRL1_ADDR_AI      (1U << 6)  // 地址自动递增
#define QMI8658A_CTRL1_BE           (1U << 5)  // 1: Big endian, 0: Little endian

// CTRL5
#define QMI8658A_CTRL5_GLPF_EN      (1U << 4)
#define QMI8658A_CTRL5_ALPF_EN      (1U << 0)

// CTRL7
#define QMI8658A_CTRL7_GYRO_EN      (1U << 1)
#define QMI8658A_CTRL7_ACCEL_EN     (1U << 0)

struct qmi8658a_dev_t {
    i2c_master_dev_handle_t dev;
    qmi8658a_config_t cfg;
    float accel_sens_lsb_per_g;
    float gyro_sens_lsb_per_dps;
    float gyro_bias_x;
    float gyro_bias_y;
    float gyro_bias_z;
};

static float accel_sens_from_fs(qmi8658a_accel_fs_t fs)
{
    switch (fs) {
    case QMI8658A_ACCEL_FS_2G:  return 16384.0f;
    case QMI8658A_ACCEL_FS_4G:  return 8192.0f;
    case QMI8658A_ACCEL_FS_8G:  return 4096.0f;
    case QMI8658A_ACCEL_FS_16G: return 2048.0f;
    default: return 4096.0f;
    }
}

static float gyro_sens_from_fs(qmi8658a_gyro_fs_t fs)
{
    switch (fs) {
    case QMI8658A_GYRO_FS_16DPS:   return 2048.0f;
    case QMI8658A_GYRO_FS_32DPS:   return 1024.0f;
    case QMI8658A_GYRO_FS_64DPS:   return 512.0f;
    case QMI8658A_GYRO_FS_128DPS:  return 256.0f;
    case QMI8658A_GYRO_FS_256DPS:  return 128.0f;
    case QMI8658A_GYRO_FS_512DPS:  return 64.0f;
    case QMI8658A_GYRO_FS_1024DPS: return 32.0f;
    case QMI8658A_GYRO_FS_2048DPS: return 16.0f;
    default: return 64.0f;
    }
}

static esp_err_t write_reg(qmi8658a_handle_t h, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(h->dev, buf, sizeof(buf), 100);
}

static esp_err_t read_reg(qmi8658a_handle_t h, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(h->dev, &reg, 1, val, 1, 100);
}

static esp_err_t read_regs(qmi8658a_handle_t h, uint8_t start_reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(h->dev, &start_reg, 1, buf, len, 100);
}

static int16_t i16_le(uint8_t lo, uint8_t hi)
{
    return (int16_t)((uint16_t)hi << 8 | lo);
}

esp_err_t qmi8658a_create(const qmi8658a_config_t *cfg, qmi8658a_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(cfg && out_handle && cfg->bus, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    qmi8658a_handle_t h = calloc(1, sizeof(struct qmi8658a_dev_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    h->cfg = *cfg;
    if (h->cfg.i2c_addr == 0) {
        h->cfg.i2c_addr = QMI8658A_I2C_ADDR_LOW;
    }
    if (h->cfg.scl_speed_hz == 0) {
        h->cfg.scl_speed_hz = 400000;
    }

    h->accel_sens_lsb_per_g = accel_sens_from_fs(h->cfg.accel_fs);
    h->gyro_sens_lsb_per_dps = gyro_sens_from_fs(h->cfg.gyro_fs);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = h->cfg.i2c_addr,
        .scl_speed_hz = h->cfg.scl_speed_hz,
    };

    esp_err_t ret = i2c_master_bus_add_device(h->cfg.bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        uint8_t addr = h->cfg.i2c_addr;
        free(h);
        ESP_LOGE(TAG, "add i2c device 0x%02X failed: %s", addr, esp_err_to_name(ret));
        return ret;
    }

    *out_handle = h;
    ESP_LOGI(TAG, "created, addr=0x%02X", h->cfg.i2c_addr);
    return ESP_OK;
}

esp_err_t qmi8658a_delete(qmi8658a_handle_t h)
{
    if (!h) {
        return ESP_OK;
    }
    if (h->dev) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
    return ESP_OK;
}

esp_err_t qmi8658a_read_whoami(qmi8658a_handle_t h, uint8_t *whoami)
{
    ESP_RETURN_ON_FALSE(h && whoami, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    return read_reg(h, QMI8658A_REG_WHO_AM_I, whoami);
}

esp_err_t qmi8658a_probe(qmi8658a_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(qmi8658a_read_whoami(h, &who), TAG, "read WHO_AM_I failed");
    ESP_RETURN_ON_FALSE(who == QMI8658A_WHO_AM_I_VALUE, ESP_ERR_NOT_FOUND, TAG,
                        "bad WHO_AM_I: 0x%02X, expected 0x%02X", who, QMI8658A_WHO_AM_I_VALUE);

    ESP_LOGI(TAG, "probe OK, WHO_AM_I=0x%02X", who);
    return ESP_OK;
}

esp_err_t qmi8658a_init(qmi8658a_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    // 上电/软复位后等待初始化稳定
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(write_reg(h, QMI8658A_REG_RESET, QMI8658A_RESET_CMD), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(qmi8658a_probe(h), TAG, "probe failed");

    // CTRL1: 开启地址自动递增，使用 Little Endian 输出
    ESP_RETURN_ON_ERROR(write_reg(h, QMI8658A_REG_CTRL1, QMI8658A_CTRL1_ADDR_AI), TAG, "CTRL1 failed");

    // CTRL2: Accel FS + ODR
    uint8_t ctrl2 = ((uint8_t)h->cfg.accel_fs << 4) | ((uint8_t)h->cfg.accel_odr & 0x0F);
    ESP_RETURN_ON_ERROR(write_reg(h, QMI8658A_REG_CTRL2, ctrl2), TAG, "CTRL2 failed");

    // CTRL3: Gyro FS + ODR
    uint8_t ctrl3 = ((uint8_t)h->cfg.gyro_fs << 4) | ((uint8_t)h->cfg.gyro_odr & 0x0F);
    ESP_RETURN_ON_ERROR(write_reg(h, QMI8658A_REG_CTRL3, ctrl3), TAG, "CTRL3 failed");

    // CTRL5: 低通滤波。mode 默认 00，对应较低带宽
    uint8_t ctrl5 = h->cfg.enable_lpf ? (QMI8658A_CTRL5_GLPF_EN | QMI8658A_CTRL5_ALPF_EN) : 0;
    ESP_RETURN_ON_ERROR(write_reg(h, QMI8658A_REG_CTRL5, ctrl5), TAG, "CTRL5 failed");

    // CTRL7: 使能 accel + gyro
    ESP_RETURN_ON_ERROR(write_reg(h, QMI8658A_REG_CTRL7,
                                  QMI8658A_CTRL7_ACCEL_EN | QMI8658A_CTRL7_GYRO_EN),
                        TAG, "CTRL7 failed");

    // 陀螺仪启动时间较长，先给 160ms
    vTaskDelay(pdMS_TO_TICKS(160));

    ESP_LOGI(TAG, "init done, accel_sens=%.1f LSB/g, gyro_sens=%.1f LSB/dps",
             h->accel_sens_lsb_per_g, h->gyro_sens_lsb_per_dps);
    return ESP_OK;
}

esp_err_t qmi8658a_read_sample(qmi8658a_handle_t h, qmi8658a_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    uint8_t buf[14] = {0};
    ESP_RETURN_ON_ERROR(read_regs(h, QMI8658A_REG_TEMP_L, buf, sizeof(buf)), TAG, "read sample failed");

    qmi8658a_sample_t s = {0};
    s.temp_raw = i16_le(buf[0], buf[1]);
    s.ax_raw   = i16_le(buf[2], buf[3]);
    s.ay_raw   = i16_le(buf[4], buf[5]);
    s.az_raw   = i16_le(buf[6], buf[7]);
    s.gx_raw   = i16_le(buf[8], buf[9]);
    s.gy_raw   = i16_le(buf[10], buf[11]);
    s.gz_raw   = i16_le(buf[12], buf[13]);

    s.temp_c = s.temp_raw / 256.0f;
    s.ax_g = s.ax_raw / h->accel_sens_lsb_per_g;
    s.ay_g = s.ay_raw / h->accel_sens_lsb_per_g;
    s.az_g = s.az_raw / h->accel_sens_lsb_per_g;

    s.gx_dps = (s.gx_raw / h->gyro_sens_lsb_per_dps) - h->gyro_bias_x;
    s.gy_dps = (s.gy_raw / h->gyro_sens_lsb_per_dps) - h->gyro_bias_y;
    s.gz_dps = (s.gz_raw / h->gyro_sens_lsb_per_dps) - h->gyro_bias_z;

    *out = s;
    return ESP_OK;
}

esp_err_t qmi8658a_calibrate_gyro_bias(qmi8658a_handle_t h, uint32_t samples, uint32_t delay_ms)
{
    ESP_RETURN_ON_FALSE(h && samples > 0, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    float sx = 0.0f;
    float sy = 0.0f;
    float sz = 0.0f;
    uint32_t valid = 0;

    // 临时清零，避免重复校准叠加
    h->gyro_bias_x = 0.0f;
    h->gyro_bias_y = 0.0f;
    h->gyro_bias_z = 0.0f;

    for (uint32_t i = 0; i < samples; ++i) {
        qmi8658a_sample_t s;
        if (qmi8658a_read_sample(h, &s) == ESP_OK) {
            sx += s.gx_dps;
            sy += s.gy_dps;
            sz += s.gz_dps;
            valid++;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_RETURN_ON_FALSE(valid > 0, ESP_FAIL, TAG, "no valid samples");

    h->gyro_bias_x = sx / valid;
    h->gyro_bias_y = sy / valid;
    h->gyro_bias_z = sz / valid;

    ESP_LOGI(TAG, "gyro bias: %.3f, %.3f, %.3f dps",
             h->gyro_bias_x, h->gyro_bias_y, h->gyro_bias_z);

    return ESP_OK;
}

void qmi8658a_set_gyro_bias(qmi8658a_handle_t h, float bx_dps, float by_dps, float bz_dps)
{
    if (!h) {
        return;
    }
    h->gyro_bias_x = bx_dps;
    h->gyro_bias_y = by_dps;
    h->gyro_bias_z = bz_dps;
}

void qmi8658a_get_gyro_bias(qmi8658a_handle_t h, float *bx_dps, float *by_dps, float *bz_dps)
{
    if (!h) {
        return;
    }
    if (bx_dps) *bx_dps = h->gyro_bias_x;
    if (by_dps) *by_dps = h->gyro_bias_y;
    if (bz_dps) *bz_dps = h->gyro_bias_z;
}
