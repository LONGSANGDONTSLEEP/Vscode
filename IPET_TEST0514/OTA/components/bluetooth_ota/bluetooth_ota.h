// NOTE: this legacy header is kept for compatibility when include/ is not used.
// Prefer including the header from components/bluetooth_ota/include/bluetooth_ota.h
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
