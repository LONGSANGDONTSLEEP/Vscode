idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES hwinit wifi_manager wifi_ota gpio_drv pet_collar_monitor pet_time pet_config
)