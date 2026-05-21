idf_component_register(
    SRCS "sd_card_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_common
)