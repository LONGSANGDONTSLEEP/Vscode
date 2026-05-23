idf_component_register(
    SRCS "pet_file_uploader.c"
    INCLUDE_DIRS "include"
    REQUIRES pet_data_logger wifi_manager esp_http_client freertos log esp_common
)