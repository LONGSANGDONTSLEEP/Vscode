idf_component_register(
    SRCS "pwrkeep.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos log pet_power_led
)
