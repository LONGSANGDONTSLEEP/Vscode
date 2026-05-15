idf_component_register(
    SRCS "hwinit.c"
    INCLUDE_DIRS "."
    # 这里把对 ota 组件的依赖名改为目录名 wifi_ota（components/wifi_ota）
    REQUIRES driver log freertos gpio_drv i2c_bus wifi_ota led_ctrl
)
