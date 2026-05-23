idf_component_register(
    SRCS "pet_telemetry.c"
    INCLUDE_DIRS "include"
    REQUIRES pet_behavior pet_time pet_power_led sd_card_mgr esp_http_client freertos log esp_common
)