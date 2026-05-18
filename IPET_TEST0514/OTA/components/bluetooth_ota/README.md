Bluetooth OTA component (skeleton)

This component provides a minimal skeleton API so that the application can
start a Bluetooth-based OTA server with a single call from main.c. The
implementation is intentionally minimal and acts as a placeholder for a
full BLE OTA implementation (e.g., using NimBLE, Blufi, or a custom GATT
protocol to receive firmware chunks and write to the OTA partition).

Files:
- bluetooth_ota.h/c : public API and placeholder implementation

How to extend:
- Enable Bluetooth in menuconfig (CONFIG_BT_ENABLED) and add NimBLE/Bluetooth components
- Implement BLE initialization and GATT service in bluetooth_ota_init/start_server
- Handle incoming data and use esp_ota_* APIs to write to the OTA partition
- Provide progress callbacks if needed
