idf_component_register(
    SRCS "led_ctrl.c"
    INCLUDE_DIRS "."
    REQUIRES log driver freertos esp_common
)
