# components/gpio_drv/CMakeLists.txt

idf_component_register(SRCS "gpio_drv.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver log freertos)
