# ESP32-S3 可复用 OTA 模块（ESP-IDF / VSCode）

这个模块基于 ESP-IDF 原生 `esp_https_ota` 封装，适合在 VSCode + ESP-IDF 插件项目中复用。

## 文件

```text
main/
  ota_update.c
  ota_update.h
  main.c
partitions.csv
```

## 1. 分区表

OTA 必须有 `otadata`、`ota_0`、`ota_1`。示例：

```csv
# Name,   Type, SubType, Offset,   Size, Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
ota_0,    app,  ota_0,   0x20000,  1800K,
ota_1,    app,  ota_1,           1800K,
spiffs,   data, spiffs,          0x70000,
```

在 `menuconfig` 中设置：

```text
Partition Table -> Custom partition table CSV -> partitions.csv
Bootloader config -> Enable app rollback support（建议开启）
```

不同 Flash 大小请调整 `ota_0/ota_1/spiffs` 大小。

## 2. CMakeLists.txt

如果放在 `main/`：

```cmake
idf_component_register(
    SRCS "main.c" "ota_update.c"
    INCLUDE_DIRS "."
)
```

如果做成组件：

```text
components/ota_update/ota_update.c
components/ota_update/include/ota_update.h
components/ota_update/CMakeLists.txt
```

组件 CMakeLists：

```cmake
idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client
)
```

## 3. main.c 使用示例

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ota_update.h"

extern const char server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const char server_cert_pem_end[] asm("_binary_server_cert_pem_end");

static const char *TAG = "app";

static void ota_cb(ota_update_event_t event, int progress, esp_err_t err, void *user_ctx)
{
    switch (event) {
    case OTA_UPDATE_EVENT_STARTED:
        ESP_LOGI(TAG, "OTA started");
        break;
    case OTA_UPDATE_EVENT_PROGRESS:
        ESP_LOGI(TAG, "OTA progress: %d%%", progress);
        break;
    case OTA_UPDATE_EVENT_SUCCESS:
        ESP_LOGI(TAG, "OTA success");
        break;
    case OTA_UPDATE_EVENT_FAILED:
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ota_update_print_app_info();

    // 开启 rollback 后，新固件首次启动必须确认。
    // 这里可以替换成你的真实自检：Wi-Fi、传感器、NVS、核心任务等。
    ota_update_confirm_app(true);

    // 先完成 Wi-Fi 连接和 SNTP 时间同步，再执行 OTA。
    // 示例：延迟 10 秒后拉取新固件。
    vTaskDelay(pdMS_TO_TICKS(10000));

    ota_update_config_t cfg = {
        .url = "https://example.com/firmware/esp32s3_app.bin",
        .cert_pem = server_cert_pem_start,
        .timeout_ms = 15000,
        .skip_cert_common_name_check = false,
        .reboot_after_success = true,
        .callback = ota_cb,
        .user_ctx = NULL,
    };

    esp_err_t err = ota_update_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(err));
    }
}
```

## 4. 添加 HTTPS 证书

把服务器根证书保存为：

```text
main/certs/server_cert.pem
```

在 `main/CMakeLists.txt` 加入：

```cmake
target_add_binary_data(${COMPONENT_TARGET} "certs/server_cert.pem" TEXT)
```

调试阶段可以使用 HTTP 或临时关闭证书 CN 检查，但生产环境必须使用 HTTPS + 正确证书。

## 5. 编译固件并上传到服务器

VSCode ESP-IDF Terminal：

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

编译后的 OTA 固件通常在：

```text
build/你的工程名.bin
```

上传到服务器，例如：

```text
https://example.com/firmware/esp32s3_app.bin
```

设备访问这个 URL，就会下载并切换到新的 OTA 分区。

## 6. 本地快速测试 HTTP 服务器

仅用于局域网测试：

```bash
cd build
python3 -m http.server 8070
```

然后 URL 类似：

```text
http://你的电脑IP:8070/你的工程名.bin
```

如果使用 HTTP，`.cert_pem = NULL`。

## 7. 生产建议

- 使用 HTTPS，不要跳过证书校验。
- 开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。
- 新固件启动后先做自检，再调用 `ota_update_confirm_app(true)`。
- OTA 前检查电量、网络质量、固件版本号。
- 服务器端最好提供 `version.json`，设备先比较版本，再决定是否下载 `.bin`。

示例 `version.json`：

```json
{
  "version": "1.0.3",
  "url": "https://example.com/firmware/esp32s3_app_v1.0.3.bin",
  "sha256": "可选"
}
```
