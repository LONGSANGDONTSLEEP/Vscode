idf_component_register(
    SRCS "pet_sd_remote.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        freertos
        sd_card_mgr
        wifi_manager
)
