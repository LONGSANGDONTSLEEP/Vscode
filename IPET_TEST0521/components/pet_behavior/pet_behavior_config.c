#include "pet_behavior_internal.h"

void pet_behavior_default_config(pet_behavior_config_t *cfg)
{
    if (!cfg)
        return;

    memset(cfg, 0, sizeof(*cfg));

    cfg->sample_rate_hz = 50.0f;
    cfg->window_ms = 1000;
    cfg->history_window_count = 7;
    cfg->min_state_hold_ms = 1800;
    cfg->terminal_report_interval_ms = 5000;

    /*
     * 默认策略：
     * - TROT/PLAY 暂时仍合并到 WALK，避免用户端误报；
     * - RUN 从 v6 开始允许作为最终状态输出；
     * - 如果后续要显示全部细分类，把 enable_fine_states 设为 true。
     */
    cfg->enable_fine_states = false;
    cfg->enable_run_state = true;
    cfg->passive_motion_as_rest = true;

    /*
     * v4 比 v3 更保守：
     * - gyro 只作为辅助，不能单独把 REST 推成 WALK；
     * - WALK 必须看到加速度本体运动，避免“拿起来轻晃/转一下”就变 WALK。
     */
    cfg->rest_acc_std_th = 0.055f;
    cfg->rest_acc_axis_std_th = 0.090f;
    cfg->rest_acc_delta_th = 0.022f;
    cfg->rest_gyro_mean_th = 22.0f;
    cfg->rest_gyro_std_th = 22.0f;
    cfg->rest_posture_std_th = 6.0f;

    /*
     * NOT_WORN 超静止阈值。
     * 2026-05-22 桌面/地面静置数据：
     * - 有的文件 acc≈1g；
     * - 有的文件 acc 固定≈6.928g；
     * 但共同点是 acc_std/range/delta 和 gyro_std/range/delta 几乎为 0。
     * 因此 v6.1 不再要求 acc_mean 接近 1g，而是用“几乎没有变化”判未佩戴。
     */
    cfg->not_worn_acc_std_th = 0.003f;
    cfg->not_worn_acc_range_th = 0.008f;
    cfg->not_worn_acc_delta_th = 0.0035f;
    cfg->not_worn_gyro_mean_th = 1.8f;
    cfg->not_worn_gyro_std_th = 0.18f;
    cfg->not_worn_gyro_range_th = 0.70f;
    cfg->not_worn_gyro_delta_th = 0.35f;
    cfg->not_worn_posture_std_th = 0.18f;

    cfg->walk_acc_std_th = 0.095f;
    cfg->walk_acc_axis_std_th = 0.145f;
    cfg->walk_acc_delta_th = 0.040f;
    cfg->walk_gyro_mean_th = 45.0f;
    cfg->walk_gyro_std_th = 32.0f;

    cfg->trot_acc_std_th = 0.180f;

    /*
     * v12 楼道走路保护版：
     * 全速跑样本显示 RUN 是连续多秒高身体运动；
     * 楼道走路样本显示 WALK 也可能有短时高 gyro/d_roll，甚至旧协议会临时给 3/4，
     * 但它的高方差/高身体冲击不连续。
     * 因此 RUN 进一步降低 gyro 权重，增加 acc_std/acc_delta 持续要求。
     */
    cfg->run_acc_std_th = 0.700f;
    cfg->run_acc_delta_th = 0.175f;
    cfg->run_gyro_mean_th = 110.0f;

    cfg->play_gyro_std_th = 135.0f;
    cfg->play_gyro_range_th = 330.0f;
    cfg->play_gyro_delta_th = 95.0f;
    cfg->play_acc_std_th = 0.38f;
    cfg->play_acc_delta_th = 0.095f;

    cfg->active_enter_votes = 3;
    cfg->rest_enter_votes = 4;
    cfg->run_enter_votes = 5;
    cfg->play_enter_votes = 3;

    cfg->impact_acc_norm_th = 3.0f;
    cfg->jump_low_g_th = 0.50f;
    cfg->jump_land_g_th = 1.85f;
    cfg->shake_gyro_th = 450.0f;
    cfg->scratch_gyro_std_min = 120.0f;
    cfg->scratch_acc_std_min = 0.10f;

    /*
     * v6.2：NOT_WORN 优先级保持不变。
     * 只要连续“几乎完全没有变化”约 20 秒，就认为是取下/放地上。
     * SLEEP 必须是“安静但仍有轻微波动”，不能抢超静止场景。
     */
    cfg->sleep_after_rest_ms = 90 * 1000;
    cfg->not_worn_after_rest_ms = 20 * 1000;
}

pet_behavior_handle_t pet_behavior_create(const pet_behavior_config_t *cfg)
{
    pet_behavior_handle_t h = calloc(1, sizeof(struct pet_behavior_t));
    if (!h)
        return NULL;

    if (cfg)
        h->cfg = *cfg;
    else
        pet_behavior_default_config(&h->cfg);

    if (h->cfg.history_window_count == 0)
        h->cfg.history_window_count = 8;
    if (h->cfg.history_window_count > PET_BEHAVIOR_HISTORY_MAX)
        h->cfg.history_window_count = PET_BEHAVIOR_HISTORY_MAX;

    pet_behavior_reset(h);
    return h;
}

void pet_behavior_delete(pet_behavior_handle_t h)
{
    free(h);
}

void pet_behavior_reset(pet_behavior_handle_t h)
{
    if (!h)
        return;

    pet_behavior_config_t cfg = h->cfg;
    memset(h, 0, sizeof(*h));
    h->cfg = cfg;

    h->state = PET_STATE_UNKNOWN;
    h->raw_state = PET_STATE_UNKNOWN;
    h->candidate = PET_STATE_UNKNOWN;
    h->pending_state = PET_STATE_UNKNOWN;

    h->last_pitch = 0.0f;
    h->last_roll = 0.0f;

    memset(&h->last_result, 0, sizeof(h->last_result));
    h->last_result.state = PET_STATE_UNKNOWN;
    h->last_result.raw_state = PET_STATE_UNKNOWN;
    h->last_result.candidate_state = PET_STATE_UNKNOWN;
    h->last_result.algo_version = PET_BEHAVIOR_ALGO_VERSION;

    reset_windows(h, 0);
    h->window_initialized = false;
}
