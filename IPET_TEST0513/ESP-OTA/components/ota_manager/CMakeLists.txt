idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ota_wifi ota_ble nvs_flash esp_http_client esp_https_ota esp_wifi esp_event)
