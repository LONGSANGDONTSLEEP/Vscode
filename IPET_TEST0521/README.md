# OTA Pet Collar Monitor

这是一个基于 ESP-IDF 的宠物项圈行为识别项目，包含硬件初始化、QMI8658A IMU 采样、宠物行为识别、SD 卡日志、HTTP 实时上传、CSV 文件上传和 BOOT 按键 Wi-Fi OTA。

## 项目结构

```text
main/
  main.c                         # 系统入口，只负责硬件初始化和启动应用层

components/
  hwinit/                        # 电源保持、GPIO、I2C、SD 卡、LED 等硬件初始化
  pet_app/                       # 应用启动层：配置、Wi-Fi、NTP、OTA 按键、监测任务、心跳日志
  qmi8658a/                      # QMI8658A IMU 驱动
  pet_behavior/                  # 行为识别算法
  pet_collar_monitor/            # 行为监测业务任务
  pet_config/                    # SD 卡 CONFIG.TXT 配置读写
  pet_data_logger/               # SD 卡状态/事件 CSV 日志
  pet_telemetry/                 # HTTP JSON 实时上传
  pet_file_uploader/             # CSV 文件上传
  pet_time/                      # 本地时间与 NTP 同步
  wifi_manager/                  # Wi-Fi 连接管理
  wifi_ota/                      # Wi-Fi OTA
```

## 使用方法

1. 安装并进入 ESP-IDF 环境。
2. 在项目根目录执行：

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py flash monitor
```

3. SD 卡可放置 `CONFIG.TXT` 修改网络和上传开关。首次启动且 SD 卡可用时，项目会自动生成默认模板。

```ini
WIFI_SSID=TYZX
WIFI_PASS=ty20260101
PET_HTTP_URL=http://192.168.1.12:8080/pet
PET_FILE_UPLOAD_URL=http://192.168.1.12:8080/upload
ENABLE_JSON_UPLOAD=1
ENABLE_FILE_UPLOAD=1
FILE_UPLOAD_SCAN_MS=60000
```

4. BOOT 键触发 Wi-Fi OTA，默认固件地址在 `components/pet_app/pet_app.c`：

```c
#define PET_APP_OTA_URL "http://192.168.1.12:8070/OTA.bin"
```

本地测试可在固件文件目录启动 HTTP 服务：

```bash
python -m http.server 8070
```

## 行为识别调参

算法阈值集中在：

```text
components/pet_behavior/pet_behavior.c
pet_behavior_default_config()
```

建议先采集静止、手持轻晃、佩戴走路、跑动、抓挠、甩头等日志，再根据 `acc_std`、`gyro_std` 分布调整阈值。

## 本次修改记录

- 新增 `components/pet_app/`，把 Wi-Fi 连接、NTP、配置加载、OTA 按键注册、宠物监测启动和心跳日志从 `main.c` 移出。
- 精简 `main/main.c`，现在只保留 `hw_init()` 和 `pet_app_start()` 两步。
- 调整 `main/CMakeLists.txt`，让入口只依赖 `hwinit`、`pet_app` 和 ESP 基础组件。
- 新增 `components/pet_app/CMakeLists.txt` 和 `pet_app.h`，补齐新组件依赖。
- 保留原有硬件初始化职责在 `hwinit`：电源保持、GPIO、I2C、SD 卡、LED 仍统一在 `hw_init()` 完成。
- 添加本 README，说明项目结构、编译运行方式、SD 卡配置和本次重构内容。
