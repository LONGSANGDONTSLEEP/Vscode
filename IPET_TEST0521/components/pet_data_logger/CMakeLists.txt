idf_component_register(
    SRCS "pet_data_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES pet_behavior qmi8658a sd_card_mgr pet_time log esp_common
)