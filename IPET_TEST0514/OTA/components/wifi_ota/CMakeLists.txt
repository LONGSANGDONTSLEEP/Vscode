idf_component_register(
    SRCS "ota_update.c"         # 组件的实现文件
    INCLUDE_DIRS "."      # 头文件所在目录
    # ota_update.c 直接包含了 app_update/esp_partition/esp_system 等头文件。
    # 显式声明依赖，避免因为间接依赖变化导致找不到头文件。
    REQUIRES esp_https_ota esp_http_client app_update esp_app_format esp_partition esp_system log
)