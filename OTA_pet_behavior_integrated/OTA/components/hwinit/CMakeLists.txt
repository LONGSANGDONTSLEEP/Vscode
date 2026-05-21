idf_component_register(
    SRCS "hwinit.c"
    INCLUDE_DIRS "."
    REQUIRES gpio_drv i2c_bus led_ctrl pwrkeep sd_card_mgr
)