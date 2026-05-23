idf_component_register(
    SRCS "pet_collar_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES qmi8658a pet_behavior pet_data_logger pet_telemetry pet_file_uploader pet_power_led driver esp_timer freertos log esp_common
)