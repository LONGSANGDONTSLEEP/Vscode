idf_component_register(
    SRCS "pet_power_led.c"
    INCLUDE_DIRS "include"
    REQUIRES led_ctrl pet_behavior driver freertos log esp_common
)
