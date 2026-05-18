// Minimal Bluetooth OTA receiver using NimBLE GATT
// This component provides a very small GATT-based protocol so you can send
// a firmware image from a phone/pc (e.g. nRF Connect) to the device and
// have it written to the OTA partition. The implementation is intentionally
// small and synchronous so you can modify it to fit your needs.

#include "bluetooth_ota.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "nvs_flash.h"

/* The NimBLE-specific headers and implementation are only compiled when
 * CONFIG_BT_NIMBLE_ENABLED is set in sdkconfig. When NimBLE is not enabled
 * we provide simple stubs so the component still builds. */
#if defined(CONFIG_BT_NIMBLE_ENABLED)
#include "esp_nimble_hci.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_err.h"

/* Some IDF versions may not expose the prototype for this helper in the
 * included header; declare it here to avoid implicit-declaration errors. */
extern esp_err_t esp_nimble_hci_and_controller_init(void);
extern esp_err_t esp_nimble_hci_init(void);
#endif

static const char *TAG = "bluetooth_ota";

/* Provide a weak stub so the component links even when the real
 * esp_nimble_hci_and_controller_init symbol is not present in the SDK.
 * If the real symbol exists it will override this weak definition.
 */
#if defined(__GNUC__)
__attribute__((weak))
#endif
esp_err_t esp_nimble_hci_and_controller_init(void)
{
    ESP_LOGW(TAG, "esp_nimble_hci_and_controller_init: weak stub called (controller init not available)");
    return ESP_ERR_NOT_SUPPORTED;
}

/* Weak stub for esp_nimble_hci_init as well; if not present in SDK, we
 * simply return ESP_ERR_NOT_SUPPORTED and continue. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
esp_err_t esp_nimble_hci_init(void)
{
    ESP_LOGW(TAG, "esp_nimble_hci_init: weak stub called (HCI init not available)");
    return ESP_ERR_NOT_SUPPORTED;
}

#if defined(CONFIG_BT_NIMBLE_ENABLED)

/* OTA state (protected by the fact that NimBLE GATT access callbacks run in
 * the NimBLE host thread). For a production implementation you should add
 * proper synchronization if you call esp_ota_* from other threads. */
static esp_ota_handle_t g_ota_handle = 0;
static const esp_partition_t *g_update_partition = NULL;
static size_t g_image_size = 0;
static size_t g_written = 0;
static bool g_ota_in_progress = false;

/* Forward declarations */
static int gatt_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gatt_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);

/* Simple GATT service: primary service 0xFF00 with two writable characteristics:
 *  - 0xFF01: control (ASCII commands)
 *     supported commands (write):
 *       "START:<size>"  - prepare OTA (image size optional, 0 = unknown)
 *       "END"           - finish OTA and set boot partition (will reboot)
 *       "ABORT"         - abort current OTA
 *  - 0xFF02: data (raw binary chunks written to the OTA image)
 */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFF00),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0xFF01),
                .access_cb = gatt_control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0xFF02),
                .access_cb = gatt_data_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                0 // end of characteristics
            }
        }
    },
    {
        0 // end of services
    }
};

/* Helpers to manage OTA state */
static int ota_start(size_t image_size)
{
    if (g_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        return 0; /* already in progress, ignore */
    }

    g_update_partition = esp_ota_get_next_update_partition(NULL);
    if (!g_update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err = esp_ota_begin(g_update_partition, image_size, &g_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    g_image_size = image_size;
    g_written = 0;
    g_ota_in_progress = true;

    ESP_LOGI(TAG, "OTA started (partition=%s, size=%u)",
             g_update_partition->label ? g_update_partition->label : "unknown",
             (unsigned)g_image_size);
    return 0;
}

static int ota_finish(void)
{
    if (!g_ota_in_progress) {
        ESP_LOGW(TAG, "ota_finish called but no OTA in progress");
        return 0;
    }

    esp_err_t err = esp_ota_end(g_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    err = esp_ota_set_boot_partition(g_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(TAG, "OTA finished: %u bytes written, rebooting...", (unsigned)g_written);
    /* give the ATT response back, then reboot */
    esp_restart();
    return 0; /* not reached */
}

static void ota_abort(void)
{
    if (!g_ota_in_progress) return;
    esp_ota_abort(g_ota_handle);
    g_ota_in_progress = false;
    g_written = 0;
    g_image_size = 0;
    g_ota_handle = 0;
    g_update_partition = NULL;
    ESP_LOGI(TAG, "OTA aborted");
}

/* GATT access callbacks -------------------------------------------------- */
static int gatt_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;

    char *buf = malloc(len + 1);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;

    rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    if (rc != 0) {
        free(buf);
        return rc;
    }
    buf[len] = '\0';

    ESP_LOGI(TAG, "Control cmd: %s", buf);

    if (strncmp(buf, "START:", 6) == 0) {
        size_t image_size = (size_t)strtoul(buf + 6, NULL, 10);
        rc = ota_start(image_size);
    } else if (strncmp(buf, "END", 3) == 0) {
        rc = ota_finish();
    } else if (strncmp(buf, "ABORT", 5) == 0) {
        ota_abort();
        rc = 0;
    } else {
        /* Unknown command - ignore */
        ESP_LOGW(TAG, "Unknown control command: %s", buf);
        rc = 0;
    }

    free(buf);
    return rc;
}

static int gatt_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!g_ota_in_progress) {
        ESP_LOGW(TAG, "Received data chunk but OTA not started");
        return BLE_ATT_ERR_UNLIKELY;
    }

    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;

    uint8_t *buf = malloc(len);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;

    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    if (rc != 0) {
        free(buf);
        return rc;
    }

    esp_err_t err = esp_ota_write(g_ota_handle, buf, len);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    g_written += (size_t)len;
    if (g_image_size > 0) {
        int progress = (int)(g_written * 100 / g_image_size);
        ESP_LOGI(TAG, "OTA progress: %d%% (%u/%u)", progress, (unsigned)g_written, (unsigned)g_image_size);
    }

    return 0;
}

/* GAP event callback: restarts advertising when disconnected */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected; conn_handle=%d", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection failed; restarting advertising");
            /* connection failed; resume advertising */
            /* fall-through */
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
        /* restart advertising */
        {
            struct ble_gap_adv_params adv_params;
            memset(&adv_params, 0, sizeof(adv_params));
            adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
            adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
            uint8_t own_addr_type;
            /* Newer NimBLE API: ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type) */
            ble_hs_id_infer_auto(0, &own_addr_type);
            int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
            if (rc) ESP_LOGE(TAG, "failed to restart adv; rc=%d", rc);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertisement complete");
        break;
    default:
        break;
    }
    return 0;
}

/* Start advertising with a short name and the custom service UUID. */
static void ble_app_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    const char *name = "BT_OTA";
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed; rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    uint8_t own_addr_type;
    /* Newer NimBLE API: ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type) */
    ble_hs_id_infer_auto(0, &own_addr_type);

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising as '%s'", name);
}

/* NimBLE host callbacks */
static void ble_app_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset; reason=%d", reason);
}

static void ble_app_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced");
}

/* Host task: register GATT services and start advertising */
static void ble_host_task(void *param)
{
    int rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "GATT services registered");
    }

    ble_svc_gap_device_name_set("BT_OTA");
    ble_app_advertise();

    /* Run the NimBLE host thread loop (blocks until nimble_port_stop) */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t bluetooth_ota_init(void)
{
#if CONFIG_BT_ENABLED
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Initialize the BLE controller + HCI in a version-compatible way */
    esp_err_t init_err = esp_nimble_hci_and_controller_init();
    if (init_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Combined HCI+controller init not available; falling back to manual init");
        /* Release Classic BT memory if only BLE is used */
#ifdef CONFIG_BT_BLE_ENABLED
        (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        init_err = esp_bt_controller_init(&bt_cfg);
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(init_err));
            return init_err;
        }
        init_err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_controller_enable(BLE) failed: %s", esp_err_to_name(init_err));
            return init_err;
        }
        /* Initialize NimBLE HCI transport if available */
        (void)esp_nimble_hci_init();
    } else if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_hci_and_controller_init failed: %s", esp_err_to_name(init_err));
        return init_err;
    }

    nimble_port_init();

    ble_hs_cfg.reset_cb = ble_app_on_reset;
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    /* Start the host task */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "bluetooth_ota: initialized (NimBLE host started)");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Bluetooth not enabled in sdkconfig; bluetooth_ota disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bluetooth_ota_start_server(void)
{
#if CONFIG_BT_ENABLED
    /* Advertising already started by host task during init; calling this
     * function is a no-op but left here so main.c can call a simple API.
     * If you want explicit start/stop behaviour, you can modify this
     * implementation to control advertising state. */
    ESP_LOGI(TAG, "bluetooth_ota: start_server called (no-op)");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bluetooth_ota_stop(void)
{
#if CONFIG_BT_ENABLED
    /* Minimal stop: stop advertising. Full controller deinit is left to
     * application if needed. */
    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_stop returned rc=%d", rc);
    }
    ESP_LOGI(TAG, "bluetooth_ota: stopped advertising");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#else /* CONFIG_BT_NIMBLE_ENABLED not set */

esp_err_t bluetooth_ota_init(void)
{
    ESP_LOGW(TAG, "bluetooth_ota: NimBLE not enabled in sdkconfig -> disabled");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_ota_start_server(void)
{
    ESP_LOGW(TAG, "bluetooth_ota_start_server: NimBLE not enabled");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bluetooth_ota_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
