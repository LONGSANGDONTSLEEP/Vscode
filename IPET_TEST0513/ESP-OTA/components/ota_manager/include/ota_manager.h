#pragma once

#include <stdbool.h>

typedef enum {
    OTA_METHOD_WIFI,
    OTA_METHOD_WIFI_AP,
    OTA_METHOD_BLE,
} ota_method_t;

void ota_manager_init(void);
void ota_manager_start(ota_method_t method, const char* url);
