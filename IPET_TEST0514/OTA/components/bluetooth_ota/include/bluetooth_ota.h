// bluetooth_ota.h
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


esp_err_t bluetooth_ota_init(void);
esp_err_t bluetooth_ota_start_server(void);
esp_err_t bluetooth_ota_stop(void);

#ifdef __cplusplus
}
#endif
/**
 * Simple Bluetooth OTA component API (skeleton).
 *
 * Usage:
 *   - Call bluetooth_ota_init() once at startup.
 *   - Call bluetooth_ota_start_server() to start a background BLE service that will accept
 *     firmware data and write to the OTA partition. (Current implementation is a placeholder.)
 *   - Call bluetooth_ota_stop() to stop the server.
 *
 * Note: The current implementation is a placeholder that returns ESP_ERR_NOT_SUPPORTED
 *       when CONFIG_BT_ENABLED is not set in sdkconfig. It is intended as a starting point
 *       so that the application can call a single simple API from main.c and the
 *       heavy lifting is kept inside this component.
 */
