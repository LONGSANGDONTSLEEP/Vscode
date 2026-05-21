idf_component_register(
    SRCS "pet_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_netif esp_common
)