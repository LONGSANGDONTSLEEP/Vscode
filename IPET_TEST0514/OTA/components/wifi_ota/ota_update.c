#include "ota_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "ota_update";

static void emit_cb(const ota_update_config_t *cfg,
                    ota_update_event_t event,
                    int progress,
                    esp_err_t err)
{
    if (cfg && cfg->callback) {
        cfg->callback(event, progress, err, cfg->user_ctx);
    }
}

void ota_update_print_app_info(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "project: %s", desc->project_name);
    ESP_LOGI(TAG, "version: %s", desc->version);
    ESP_LOGI(TAG, "date/time: %s %s", desc->date, desc->time);
    ESP_LOGI(TAG, "idf: %s", desc->idf_ver);
    ESP_LOGI(TAG, "running partition: %s, subtype: 0x%02x, addr: 0x%08lx",
             running ? running->label : "unknown",
             running ? running->subtype : 0,
             running ? (unsigned long)running->address : 0UL);
}

/* --- 简化接口实现：在后台任务中启动 OTA，并在完成后根据配置重启 --- */

static void ota_bg_task(void *arg)
{
    ota_update_config_t cfg = {0};
    const char **pargs = (const char **)arg;
    cfg.url = pargs[0];
    cfg.cert_pem = pargs[1];
    cfg.timeout_ms = 10000;
    cfg.skip_cert_common_name_check = true; // 简化：开发时允许，生产请改为 false
    cfg.reboot_after_success = true;
    cfg.callback = NULL;

    ota_update_start(&cfg);

    free(arg);
    vTaskDelete(NULL);
}

void ota_update_start_bg(const char *url, const char *cert_pem)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "ota_update_start_bg: empty url");
        return;
    }

    const char **pargs = malloc(sizeof(const char *) * 2);
    if (!pargs) {
        ESP_LOGE(TAG, "ota_update_start_bg: alloc failed");
        return;
    }
    pargs[0] = url;
    pargs[1] = cert_pem;

    BaseType_t ok = xTaskCreate(ota_bg_task, "ota_bg", 4096, pargs, tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ota_update_start_bg: create task failed");
        free(pargs);
    }
}

esp_err_t ota_update_confirm_app(bool self_test_ok)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        ESP_LOGW(TAG, "cannot read ota state; rollback may be disabled or partition unavailable");
        return ESP_OK;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (self_test_ok) {
            ESP_LOGI(TAG, "self-test passed; marking current app valid");
            return esp_ota_mark_app_valid_cancel_rollback();
        }

        ESP_LOGE(TAG, "self-test failed; rolling back");
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "rollback failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "app state does not require confirmation: %d", ota_state);
    return ESP_OK;
}

esp_err_t ota_update_start(const ota_update_config_t *config)
{
    if (!config || !config->url || strlen(config->url) == 0) {
        ESP_LOGE(TAG, "invalid ota config/url");
        return ESP_ERR_INVALID_ARG;
    }

    emit_cb(config, OTA_UPDATE_EVENT_STARTED, 0, ESP_OK);
    ESP_LOGI(TAG, "starting OTA from: %s", config->url);

    esp_http_client_config_t http_cfg = {
        .url = config->url,
        .cert_pem = config->cert_pem,
        .timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 10000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = config->skip_cert_common_name_check,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        emit_cb(config, OTA_UPDATE_EVENT_FAILED, 0, err);
        return err;
    }

    int last_progress = -1;
    while (1) {
        err = esp_https_ota_perform(ota_handle);

        int downloaded = esp_https_ota_get_image_len_read(ota_handle);
        int total = esp_https_ota_get_image_size(ota_handle);
        if (total > 0) {
            int progress = downloaded * 100 / total;
            if (progress != last_progress) {
                last_progress = progress;
                ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d bytes)", progress, downloaded, total);
                emit_cb(config, OTA_UPDATE_EVENT_PROGRESS, progress, ESP_OK);
            }
        }

        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        emit_cb(config, OTA_UPDATE_EVENT_FAILED, last_progress < 0 ? 0 : last_progress, err);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "complete firmware image was not received");
        esp_https_ota_abort(ota_handle);
        emit_cb(config, OTA_UPDATE_EVENT_FAILED, last_progress < 0 ? 0 : last_progress, ESP_FAIL);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        emit_cb(config, OTA_UPDATE_EVENT_FAILED, last_progress < 0 ? 0 : last_progress, err);
        return err;
    }

    ESP_LOGI(TAG, "OTA success; new firmware will boot after restart");
    emit_cb(config, OTA_UPDATE_EVENT_SUCCESS, 100, ESP_OK);

    if (config->reboot_after_success) {
        ESP_LOGI(TAG, "restarting...");
        esp_restart();
    }

    return ESP_OK;
}
