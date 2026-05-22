#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "qmi8658a.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_BEHAVIOR_ALGO_VERSION "v6.2.0-fast-not-worn"
#define PET_BEHAVIOR_HISTORY_MAX 8

typedef enum {
    PET_STATE_UNKNOWN = 0,
    PET_STATE_NOT_WORN,        // 未佩戴 / 摘下
    PET_STATE_SLEEP,           // 睡眠 / 深度休息
    PET_STATE_REST,            // 清醒静止 / 安静
    PET_STATE_WALK,            // 慢走 / 普通活动。v3 默认把 TROT/RUN/PLAY 显示合并到 WALK
    PET_STATE_TROT,            // 快走 / 小跑。默认仅作为 raw_state 记录
    PET_STATE_RUN,             // 奔跑。默认仅作为 raw_state 记录
    PET_STATE_PLAY,            // 玩耍 / 剧烈不规则活动。默认仅作为 raw_state 记录
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
    float sample_rate_hz;           // 推荐 25Hz 或 50Hz

    /*
     * v3 核心节奏：
     * 1) 高频 sample 只累计统计量；
     * 2) 每 window_ms 生成一个短窗口候选状态；
     * 3) 再结合最近 history_window_count 个短窗口做最终判断。
     */
    uint32_t window_ms;             // 推荐 1000ms
    uint8_t history_window_count;   // 推荐 5；最大 PET_BEHAVIOR_HISTORY_MAX
    uint32_t min_state_hold_ms;     // 兜底防抖，推荐 2000~3000ms
    uint32_t terminal_report_interval_ms; // 终端/HTTP 慢速上报间隔，推荐 5000ms

    /*
     * 产品显示策略：
     * false: 默认只稳定显示 REST / WALK / SLEEP / NOT_WORN，减少 WALK 误显示 PLAY/RUN。
     * true : 允许最终 state 显示 TROT / RUN / PLAY，适合后期有更多人工标签后再打开。
     */
    bool enable_fine_states;      // true: 最终 state 允许显示 TROT/RUN/PLAY
    bool enable_run_state;        // v6: true 时即使 enable_fine_states=false，也允许最终 state 显示 RUN
    bool passive_motion_as_rest;    // true 时 PASSIVE_MOTION 显示成 REST，但 raw_state 仍保留

    // 静止/运动阈值。单位：g 或 dps。
    float rest_acc_std_th;
    float rest_acc_axis_std_th;
    float rest_acc_delta_th;
    float rest_gyro_mean_th;
    float rest_gyro_std_th;
    float rest_posture_std_th;

    /*
     * 未佩戴/桌面静置阈值。
     * 这组阈值比 REST 更严格：必须几乎完全没有加速度、角速度和姿态波动，
     * 并且持续一段时间后才判 NOT_WORN。
     * 如果只是长时间安静但仍有轻微微动，则更像 SLEEP。
     */
    /* acc_mean 不再强制接近 1g：有些静置文件会出现固定 6.928g，但只要波动极低仍应判未佩戴。 */
    float not_worn_acc_std_th;
    float not_worn_acc_range_th;
    float not_worn_acc_delta_th;
    float not_worn_gyro_mean_th;
    float not_worn_gyro_std_th;
    float not_worn_gyro_range_th;
    float not_worn_gyro_delta_th;
    float not_worn_posture_std_th;

    float walk_acc_std_th;
    float walk_acc_axis_std_th;
    float walk_acc_delta_th;
    float walk_gyro_mean_th;
    float walk_gyro_std_th;

    float trot_acc_std_th;
    float run_acc_std_th;
    float run_acc_delta_th;
    float run_gyro_mean_th;

    /* PLAY 一定要更严格：它代表不规则、高旋转、高 burst，而不是“动一下很大”。 */
    float play_gyro_std_th;
    float play_gyro_range_th;
    float play_gyro_delta_th;
    float play_acc_std_th;
    float play_acc_delta_th;

    // 多窗口投票。按 1 秒窗口时，3 votes 约等于最近 5 秒里至少 3 秒满足。
    uint8_t active_enter_votes;     // 从 REST 进入 WALK 至少需要多少个 active 窗口
    uint8_t rest_enter_votes;       // 从 WALK 回 REST 至少需要多少个 rest 窗口
    uint8_t run_enter_votes;
    uint8_t play_enter_votes;

    // 事件阈值
    float impact_acc_norm_th;
    float jump_low_g_th;
    float jump_land_g_th;
    float shake_gyro_th;
    float scratch_gyro_std_min;
    float scratch_acc_std_min;

    // 长时间判断
    // not_worn_after_rest_ms 基于“超静止”计时；sleep_after_rest_ms 基于普通 REST 计时。
    // v6.2 默认更快：放下后约 20 秒超静止即可进入 NOT_WORN。
    uint32_t sleep_after_rest_ms;
    uint32_t not_worn_after_rest_ms;
} pet_behavior_config_t;

typedef struct {
    pet_state_t state;              // 产品最终输出状态。默认细分类会合并到 WALK
    pet_state_t candidate_state;    // 当前 1 秒短窗口候选状态
    pet_state_t raw_state;          // 多秒综合后的内部状态，保留 RUN/PLAY/TROT 方便后续调参
    uint32_t events;                // pet_event_t bitmask

    float acc_norm_g;
    float gyro_norm_dps;
    float pitch_deg;
    float roll_deg;

    float acc_norm_mean;
    float acc_norm_std;
    float gyro_norm_mean;
    float gyro_norm_std;

    // v3 调试特征：建议全部写进 CSV，后续持续调参时非常关键。
    float acc_axis_std;
    float gyro_axis_std;
    float acc_delta_mean;
    float gyro_delta_mean;
    float acc_range;
    float gyro_range;
    float posture_std;

    float activity_score;           // 0~100，整体运动强度
    float rest_score;               // 0~100，静止可信度
    float rhythm_score;             // 0~100，节律/稳定运动倾向。当前为轻量启发式，不是频谱算法
    float irregular_score;          // 0~100，不规则/乱动倾向
    float burst_score;              // 0~100，短时冲击/大幅旋转倾向
    float run_score;                // 0~100，跑步倾向。v6 用于调 WALK/RUN 边界
    float play_score;               // 0~100，玩耍倾向。后续用于 PLAY 识别
    float confidence;               // 0~100，最终状态置信度

    uint8_t history_count;
    uint8_t vote_rest;
    uint8_t vote_walk;
    uint8_t vote_trot;
    uint8_t vote_run;
    uint8_t vote_play;
    uint8_t vote_passive;

    uint32_t state_duration_ms;
    uint32_t rest_like_duration_ms;
    uint32_t sample_count;

    const char *algo_version;
} pet_behavior_result_t;

typedef struct pet_behavior_t *pet_behavior_handle_t;

void pet_behavior_default_config(pet_behavior_config_t *cfg);
pet_behavior_handle_t pet_behavior_create(const pet_behavior_config_t *cfg);
void pet_behavior_delete(pet_behavior_handle_t h);
void pet_behavior_reset(pet_behavior_handle_t h);

// 返回 true 表示本次产生了窗口状态更新或事件，适合打印日志/上报。
bool pet_behavior_update(pet_behavior_handle_t h,
                         const qmi8658a_sample_t *sample,
                         uint32_t now_ms,
                         pet_behavior_result_t *out);

const char *pet_state_to_str(pet_state_t state);
void pet_events_to_str(uint32_t events, char *buf, uint32_t buf_len);

#ifdef __cplusplus
}
#endif
