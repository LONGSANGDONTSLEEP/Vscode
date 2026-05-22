idf_component_register(
    SRCS "pet_app.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_common
        esp_http_client
        freertos
        gpio_drv
        hwinit
        pet_behavior
        pet_collar_monitor
        pet_config
        pet_time
        qmi8658a
        wifi_manager
        wifi_ota
)
