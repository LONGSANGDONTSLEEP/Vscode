# 宠物项圈行为识别接入说明

本次整理后，项目把 IMU 驱动、行为算法、业务启动逻辑拆成三层，避免 `main.c` 里堆大量传感器代码。

## 目录结构

```text
components/
  qmi8658a/              # QMI8658A 传感器驱动层
    include/qmi8658a.h
    qmi8658a.c

  pet_behavior/          # 行为识别算法层
    include/pet_behavior.h
    pet_behavior.c

  pet_collar_monitor/    # 项圈业务封装层
    include/pet_collar_monitor.h
    pet_collar_monitor.c

  hwinit/                # 硬件初始化层，提供 hwinit_get_i2c_bus()

  pet_app/               # 应用启动层：配置、Wi-Fi、NTP、OTA 按键、监测任务、心跳日志

main/
  main.c                 # 只负责系统入口：hw_init() + pet_app_start()
```

## 数据流

```text
QMI8658A 原始数据
    ↓ qmi8658a_read_sample()
ax/ay/az(g), gx/gy/gz(dps), temp
    ↓ pet_behavior_update()
状态 + 事件
    ↓ pet_collar_monitor task
日志 / 后续 BLE、Wi-Fi、云端上报
```

## 已接入的状态

```c
PET_STATE_UNKNOWN
PET_STATE_NOT_WORN
PET_STATE_SLEEP
PET_STATE_REST
PET_STATE_WALK
PET_STATE_TROT
PET_STATE_RUN
PET_STATE_PLAY
PET_STATE_PASSIVE_MOTION
PET_STATE_ABNORMAL_INACTIVE
```

## 已接入的事件

```c
PET_EVENT_SHAKE       // 甩头 / 抖毛
PET_EVENT_SCRATCH     // 抓挠
PET_EVENT_JUMP        // 跳跃
PET_EVENT_IMPACT      // 撞击
PET_EVENT_ROLL_OVER   // 翻滚
```

## 默认配置

`components/pet_app/pet_app.c` 中启动参数：

```c
pet_collar_monitor_config_t cfg = {
    .bus = hwinit_get_i2c_bus(),
    .qmi8658a_addr = QMI8658A_I2C_ADDR_LOW, // 0x6B
    .sample_period_ms = 20,                 // 50Hz
    .task_stack_size = 4096,
    .task_priority = 5,
    .enable_gyro_calibration = true,
};
```

QMI8658A 配置在 `pet_collar_monitor.c`：

```c
.accel_fs = QMI8658A_ACCEL_FS_8G,
.gyro_fs = QMI8658A_GYRO_FS_512DPS,
.accel_odr = QMI8658A_ACC_ODR_500HZ,
.gyro_odr = QMI8658A_GYR_ODR_448HZ,
.enable_lpf = true,
```

## 调参位置

第一版阈值在：

```text
components/pet_behavior/pet_behavior.c
pet_behavior_default_config()
```

重点关注这些参数：

```c
rest_acc_std_th
rest_gyro_mean_th
walk_acc_std_th
trot_acc_std_th
run_acc_std_th
play_gyro_std_th
impact_acc_norm_th
shake_gyro_th
scratch_gyro_std_min
scratch_acc_std_min
```

## 日志格式

行为任务会输出类似：

```text
I PET_MON: state=REST event=NONE acc=0.999 gyro=0.82 pitch=-4.1 roll=10.2 acc_std=0.006 gyro_std=0.40
```

字段含义：

- `state`：当前主状态
- `event`：本次检测到的事件，可叠加
- `acc`：加速度模长，单位 g
- `gyro`：角速度模长，单位 dps
- `pitch/roll`：姿态角，辅助调试
- `acc_std/gyro_std`：1 秒窗口内的运动强度特征

## 下一步建议

1. 先让项圈分别采集：桌面静止、手持轻晃、戴在狗/猫身上走路、跑动、抓挠/甩头样本。
2. 把日志里的 `acc_std`、`gyro_std` 分布记录下来。
3. 根据真实数据微调 `pet_behavior_default_config()` 的阈值。
4. 后续可把 `pet_collar_monitor_get_last_result()` 的结果接到 BLE/Wi-Fi 上报。
