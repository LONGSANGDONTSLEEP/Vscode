idf_component_register(
    SRCS "pet_config.c"
    INCLUDE_DIRS "include"
    REQUIRES sd_card_mgr log esp_common
)