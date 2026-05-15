idf_component_register(SRCS "ota_wifi.c" "ota_ap.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash esp_http_client esp_https_ota esp_wifi esp_event esp_netif esp_http_server esp_partition)

# Note: upload page is inlined into ota_ap.c to avoid embedding issues across ESP-IDF versions.











