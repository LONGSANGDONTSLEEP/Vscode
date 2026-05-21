#include "sd_card_mgr.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define TAG "SD_CARD"

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static char s_mount_point[32] = SD_CARD_DEFAULT_MOUNT_POINT;

esp_err_t sd_card_mgr_init_default(void)
{
    sd_card_mgr_config_t cfg = {
        .gpio_clk = SD_CARD_DEFAULT_GPIO_CLK,
        .gpio_cmd = SD_CARD_DEFAULT_GPIO_CMD,
        .gpio_d0  = SD_CARD_DEFAULT_GPIO_D0,
        .gpio_d1  = SD_CARD_DEFAULT_GPIO_D1,
        .gpio_d2  = SD_CARD_DEFAULT_GPIO_D2,
        .gpio_d3  = SD_CARD_DEFAULT_GPIO_D3,

        .mount_point = SD_CARD_DEFAULT_MOUNT_POINT,
        .max_files = 5,

        /*
         * 第一版先不要自动格式化。
         * 如果 SD 卡格式不对，先让它报错，避免误删卡里的东西。
         */
        .format_if_mount_failed = false,

        /*
         * 你的硬件 D0/D1/D2/D3 都接了，所以默认 4-bit。
         * 如果挂载失败，后面可以临时改成 false，用 1-bit 排查。
         */
        .use_4bit = true,
    };

    return sd_card_mgr_init_sdmmc(&cfg);
}

esp_err_t sd_card_mgr_init_sdmmc(const sd_card_mgr_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    const char *mount_point = cfg->mount_point ? cfg->mount_point : SD_CARD_DEFAULT_MOUNT_POINT;
    snprintf(s_mount_point, sizeof(s_mount_point), "%s", mount_point);

    ESP_LOGI(TAG, "Initializing SD card in SDMMC mode");
    ESP_LOGI(TAG, "CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d width=%d-bit",
             cfg->gpio_clk,
             cfg->gpio_cmd,
             cfg->gpio_d0,
             cfg->gpio_d1,
             cfg->gpio_d2,
             cfg->gpio_d3,
             cfg->use_4bit ? 4 : 1);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = cfg->format_if_mount_failed,
        .max_files = cfg->max_files > 0 ? cfg->max_files : 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    /*
     * 先用默认频率，稳定优先。
     * 后面确认 SD 卡稳定后，再考虑提高频率。
     */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.clk = cfg->gpio_clk;
    slot_config.cmd = cfg->gpio_cmd;
    slot_config.d0  = cfg->gpio_d0;

    if (cfg->use_4bit) {
        slot_config.width = 4;
        slot_config.d1 = cfg->gpio_d1;
        slot_config.d2 = cfg->gpio_d2;
        slot_config.d3 = cfg->gpio_d3;
    } else {
        /*
         * 1-bit 模式只使用 CLK / CMD / D0。
         * 如果 4-bit 挂载失败，可以先用 1-bit 排查硬件。
         */
        slot_config.width = 1;
        slot_config.d1 = GPIO_NUM_NC;
        slot_config.d2 = GPIO_NUM_NC;
        slot_config.d3 = GPIO_NUM_NC;
    }

#ifdef SDMMC_SLOT_FLAG_INTERNAL_PULLUP
    /*
     * 内部上拉只能用于调试，正式硬件最好有外部上拉。
     */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        s_mount_point,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));

        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Filesystem mount failed. "
                          "Check FAT32 format, or set format_if_mount_failed=true only if you want to format.");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "SD card timeout. Check wiring, pull-ups, power supply, and card insertion.");
        } else {
            ESP_LOGE(TAG, "SD card init failed. Check GPIO mapping and SD card format.");
        }

        return ret;
    }

    s_mounted = true;

    ESP_LOGI(TAG, "SD card mounted at %s", s_mount_point);
    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

esp_err_t sd_card_mgr_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card unmount failed: %s", esp_err_to_name(ret));
    }

    s_card = NULL;
    s_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
    return ret;
}

bool sd_card_mgr_is_mounted(void)
{
    return s_mounted;
}

const char *sd_card_mgr_mount_point(void)
{
    return s_mount_point;
}

esp_err_t sd_card_mgr_write_test_file(void)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "SD card not mounted");

    char path[64];
    snprintf(path, sizeof(path), "%s/test.txt", s_mount_point);

    ESP_LOGI(TAG, "Writing test file: %s", path);

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s, errno=%d", path, errno);
        return ESP_FAIL;
    }

    fprintf(f, "hello sd card, pet collar test\n");
    fclose(f);

    ESP_LOGI(TAG, "Test file written");
    return ESP_OK;
}