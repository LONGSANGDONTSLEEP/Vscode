idf_component_register(
    SRCS "bluetooth_ota.c"
    INCLUDE_DIRS "include"
    # This component uses NimBLE (esp_nimble) APIs when CONFIG_BT_NIMBLE_ENABLED is set.
    # Add explicit REQUIRES to ensure the build system exposes the correct include paths
    # and links the bluetooth stack when enabled.
    # Use 'bt' as the generic bluetooth dependency to maximize compatibility
    # across different ESP-IDF versions. If your SDK exposes a component named
    # 'esp_nimble' you can add it here, but many installations already provide
    # the required headers/libs via 'bt'.
    REQUIRES log nvs_flash esp_http_client app_update esp_app_format esp_partition esp_system bt
    # Conditional dependency: most esp-idf versions provide "bt" or "esp_nimble" components.
    # Keep the list minimal; if you see missing headers during build, add 'bt' or 'esp_nimble'
    # to this list depending on your SDK configuration.
)
