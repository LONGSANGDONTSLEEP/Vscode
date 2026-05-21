#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 当前板子的 SDMMC 接线：
 *
 * SD2  -> GPIO6
 * SD3  -> GPIO7
 * CMD  -> GPIO15
 * CLK  -> GPIO16
 * SD0  -> GPIO18
 * SD1  -> GPIO8
 */
#define SD_CARD_DEFAULT_GPIO_D2   GPIO_NUM_6
#define SD_CARD_DEFAULT_GPIO_D3   GPIO_NUM_7
#define SD_CARD_DEFAULT_GPIO_CMD  GPIO_NUM_15
#define SD_CARD_DEFAULT_GPIO_CLK  GPIO_NUM_16
#define SD_CARD_DEFAULT_GPIO_D0   GPIO_NUM_18
#define SD_CARD_DEFAULT_GPIO_D1   GPIO_NUM_8

#define SD_CARD_DEFAULT_MOUNT_POINT "/sdcard"

typedef struct {
    gpio_num_t gpio_clk;
    gpio_num_t gpio_cmd;
    gpio_num_t gpio_d0;
    gpio_num_t gpio_d1;
    gpio_num_t gpio_d2;
    gpio_num_t gpio_d3;

    const char *mount_point;        // 默认 "/sdcard"
    int max_files;                  // 默认 5
    bool format_if_mount_failed;    // 测试阶段建议 false，避免误格式化 SD 卡
    bool use_4bit;                  // true: 4-bit, false: 1-bit
} sd_card_mgr_config_t;

/**
 * @brief 使用当前板子的默认 SDMMC 4-bit 引脚初始化 SD 卡。
 */
esp_err_t sd_card_mgr_init_default(void);

/**
 * @brief 使用指定配置初始化 SDMMC SD 卡。
 */
esp_err_t sd_card_mgr_init_sdmmc(const sd_card_mgr_config_t *cfg);

/**
 * @brief 卸载 SD 卡。
 */
esp_err_t sd_card_mgr_deinit(void);

/**
 * @brief SD 卡是否已经挂载成功。
 */
bool sd_card_mgr_is_mounted(void);

/**
 * @brief 获取挂载路径，默认 "/sdcard"。
 */
const char *sd_card_mgr_mount_point(void);

/**
 * @brief 测试写入 /sdcard/test.txt。
 */
esp_err_t sd_card_mgr_write_test_file(void);

#ifdef __cplusplus
}
#endif