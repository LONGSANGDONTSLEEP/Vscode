idf_component_register(
    SRCS "led_ctrl.c"
    INCLUDE_DIRS "."
    REQUIRES gpio_drv log driver
)
