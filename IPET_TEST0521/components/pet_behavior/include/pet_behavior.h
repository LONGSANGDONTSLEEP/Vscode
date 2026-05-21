#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "qmi8658a.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PET_STATE_UNKNOWN = 0,
    PET_STATE_NOT_WORN,        // 未佩戴 / 摘下
    PET_STATE_SLEEP,           // 睡眠 / 深度休息
    PET_STATE_REST,            // 清醒静止 / 安静
    PET_STATE_WALK,            // 慢走
    PET_STATE_TROT,            // 快走 / 小跑
    PET_STATE_RUN,             // 奔跑
    PET_STATE_PLAY,            // 玩耍 / 剧烈活动
    PET_STATE_PASSIVE_MOTION,  // 被抱起 / 乘车 / 非自主运动
    PET_STATE_ABNORMAL_INACTIVE
} pet_state_t;

typedef enum {
    PET_EVENT_NONE      = 0,
    PET_EVENT_SHAKE     = 1 << 0,  // 甩头 / 抖毛
    PET_EVENT_SCRATCH   = 1 << 1,  // 抓挠
    PET_EVENT_JUMP      = 1 << 2,  // 跳跃
    PET_EVENT_IMPACT    = 1 << 3,  // 撞击
    PET_EVENT_ROLL_OVER = 1 << 4,  // 翻滚
} pet_event_t;

typedef struct {
    float sample_rate_hz;           // 第一版推荐 50Hz

    uint32_t window_ms;             // 每个窗口计算一次状态，推荐 1000ms
    uint32_t min_state_hold_ms;     // 状态防抖时间，推荐 3000ms

    // 静止/运动阈值
    float rest_acc_std_th;          // acc_norm 标准差低于此值认为接近静止
    float rest_gyro_mean_th;        // gyro_norm 均值低于此值认为接近静止
    float walk_acc_std_th;
    float trot_acc_std_th;
    float run_acc_std_th;
    float play_gyro_std_th;

    // 事件阈值
    float impact_acc_norm_th;
    float jump_low_g_th;
    float jump_land_g_th;
    float shake_gyro_th;
    float scratch_gyro_std_min;
    float scratch_acc_std_min;

    // 长时间判断
    uint32_t sleep_after_rest_ms;
    uint32_t not_worn_after_rest_ms;
} pet_behavior_config_t;

typedef struct {
    pet_state_t state;
    pet_state_t candidate_state;
    uint32_t events;                // pet_event_t bitmask

    float acc_norm_g;
    float gyro_norm_dps;
    float pitch_deg;
    float roll_deg;

    float acc_norm_mean;
    float acc_norm_std;
    float gyro_norm_mean;
    float gyro_norm_std;

    uint32_t state_duration_ms;
    uint32_t rest_like_duration_ms;
    uint32_t sample_count;
} pet_behavior_result_t;

typedef struct pet_behavior_t *pet_behavior_handle_t;

void pet_behavior_default_config(pet_behavior_config_t *cfg);
pet_behavior_handle_t pet_behavior_create(const pet_behavior_config_t *cfg);
void pet_behavior_delete(pet_behavior_handle_t h);
void pet_behavior_reset(pet_behavior_handle_t h);

// 返回 true 表示本次产生了窗口状态更新或事件，适合打印日志/上报
bool pet_behavior_update(pet_behavior_handle_t h,
                         const qmi8658a_sample_t *sample,
                         uint32_t now_ms,
                         pet_behavior_result_t *out);

const char *pet_state_to_str(pet_state_t state);
void pet_events_to_str(uint32_t events, char *buf, uint32_t buf_len);

#ifdef __cplusplus
}
#endif
