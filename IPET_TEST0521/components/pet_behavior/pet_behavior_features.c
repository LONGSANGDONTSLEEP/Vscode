#include "pet_behavior_internal.h"

bool is_ultra_static_window(const behavior_window_t *w, const pet_behavior_config_t *c)
{
    /*
     * 取下放桌面时的典型数据：
     * acc_std <= 0.001g, acc_range <= 0.003g, gyro_mean < 0.9dps,
     * gyro_std <= 0.06dps, gyro_range <= 0.31dps。
     * 这里留出少量余量，但仍明显低于佩戴睡觉时应出现的微动。
     */
    bool acc_near_1g = fabsf(w->acc_mean - 1.0f) < 0.05f;
    return acc_near_1g &&
           w->acc_std <= c->not_worn_acc_std_th &&
           w->acc_range <= c->not_worn_acc_range_th &&
           w->acc_delta_mean <= c->not_worn_acc_delta_th &&
           w->gyro_mean <= c->not_worn_gyro_mean_th &&
           w->gyro_std <= c->not_worn_gyro_std_th &&
           w->gyro_range <= c->not_worn_gyro_range_th &&
           w->gyro_delta_mean <= c->not_worn_gyro_delta_th &&
           w->posture_std <= c->not_worn_posture_std_th;
}

void reset_windows(pet_behavior_handle_t h, uint32_t now_ms)
{
    stat_reset(&h->acc_norm_win);
    stat_reset(&h->gyro_norm_win);
    stat_reset(&h->ax_win);
    stat_reset(&h->ay_win);
    stat_reset(&h->az_win);
    stat_reset(&h->gx_win);
    stat_reset(&h->gy_win);
    stat_reset(&h->gz_win);
    stat_reset(&h->acc_delta_win);
    stat_reset(&h->gyro_delta_win);
    stat_reset(&h->pitch_win);
    stat_reset(&h->roll_win);
    h->win_start_ms = now_ms;
    h->window_initialized = true;
}

behavior_window_t build_window_feature(pet_behavior_handle_t h, uint32_t now_ms)
{
    behavior_window_t w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    w.end_ms = now_ms;

    float ax_std = stat_std(&h->ax_win);
    float ay_std = stat_std(&h->ay_win);
    float az_std = stat_std(&h->az_win);
    float gx_std = stat_std(&h->gx_win);
    float gy_std = stat_std(&h->gy_win);
    float gz_std = stat_std(&h->gz_win);

    w.acc_mean = stat_mean(&h->acc_norm_win);
    w.acc_std = stat_std(&h->acc_norm_win);
    w.gyro_mean = stat_mean(&h->gyro_norm_win);
    w.gyro_std = stat_std(&h->gyro_norm_win);
    w.acc_axis_std = sqrtf(ax_std * ax_std + ay_std * ay_std + az_std * az_std);
    w.gyro_axis_std = sqrtf(gx_std * gx_std + gy_std * gy_std + gz_std * gz_std);
    w.acc_delta_mean = stat_mean(&h->acc_delta_win);
    w.gyro_delta_mean = stat_mean(&h->gyro_delta_win);
    w.acc_range = stat_range(&h->acc_norm_win);
    w.gyro_range = stat_range(&h->gyro_norm_win);
    w.posture_std = stat_std(&h->pitch_win) + stat_std(&h->roll_win);

    const pet_behavior_config_t *c = &h->cfg;
    bool acc_near_1g = fabsf(w.acc_mean - 1.0f) < 0.22f;

    w.ultra_static = is_ultra_static_window(&w, c);

    float rest_acc_score = score_down(w.acc_std, c->rest_acc_std_th, 0.15f);
    float rest_axis_score = score_down(w.acc_axis_std, c->rest_acc_axis_std_th, 0.20f);
    float rest_gyro_score = score_down(w.gyro_mean, c->rest_gyro_mean_th, 85.0f);
    float rest_posture_score = score_down(w.posture_std, c->rest_posture_std_th, 18.0f);
    w.rest_score = acc_near_1g ? min3f(rest_acc_score, rest_axis_score, rest_gyro_score) : 0.0f;
    if (w.rest_score > 0.0f && rest_posture_score < w.rest_score)
        w.rest_score = rest_posture_score;

    float acc_energy = score_up(w.acc_std, 0.060f, 0.42f);
    float axis_energy = score_up(w.acc_axis_std, 0.095f, 0.58f);
    float jerk_energy = score_up(w.acc_delta_mean, 0.024f, 0.145f);
    float gyro_energy = score_up(w.gyro_mean, 28.0f, 190.0f);
    float gyro_std_energy = score_up(w.gyro_std, 24.0f, 165.0f);

    w.activity_score = 0.25f * acc_energy +
                       0.20f * axis_energy +
                       0.20f * jerk_energy +
                       0.20f * gyro_energy +
                       0.15f * gyro_std_energy;
    w.activity_score = clampf_local(w.activity_score, 0.0f, 100.0f);

    w.burst_score = 0.35f * score_up(w.acc_range, 0.45f, 1.40f) +
                    0.35f * score_up(w.gyro_range, 170.0f, 560.0f) +
                    0.30f * score_up(w.gyro_delta_mean, 45.0f, 160.0f);
    w.burst_score = clampf_local(w.burst_score, 0.0f, 100.0f);

    w.irregular_score = 0.35f * score_up(w.gyro_std, 65.0f, 190.0f) +
                        0.25f * score_up(w.gyro_range, 220.0f, 620.0f) +
                        0.20f * score_up(w.gyro_delta_mean, 55.0f, 170.0f) +
                        0.20f * score_up(w.posture_std, 8.0f, 32.0f);
    w.irregular_score = clampf_local(w.irregular_score, 0.0f, 100.0f);

    /*
     * 这是一个轻量“节律倾向”启发式：
     * - 有运动强度；
     * - 但 burst 和不规则旋转不能过高；
     * - 更像走/跑的连续运动，而不是甩头/乱玩。
     * 后续如果要更像手环，可在这里加入峰值间隔/自相关/FFT。
     */
    w.rhythm_score = w.activity_score;
    w.rhythm_score -= 0.45f * w.burst_score;
    w.rhythm_score -= 0.30f * w.irregular_score;
    w.rhythm_score += 0.20f * score_up(w.acc_delta_mean, c->walk_acc_delta_th, c->run_acc_delta_th);
    w.rhythm_score = clampf_local(w.rhythm_score, 0.0f, 100.0f);

    w.run_score = 0.40f * score_up(w.acc_std, c->trot_acc_std_th, 0.48f) +
                  0.30f * score_up(w.acc_delta_mean, 0.070f, 0.170f) +
                  0.20f * score_up(w.gyro_mean, c->run_gyro_mean_th, 210.0f) +
                  0.10f * w.rhythm_score;
    w.run_score -= 0.25f * score_up(w.irregular_score, 65.0f, 100.0f);
    w.run_score = clampf_local(w.run_score, 0.0f, 100.0f);

    w.play_score = 0.32f * score_up(w.gyro_std, c->play_gyro_std_th * 0.65f, c->play_gyro_std_th * 1.45f) +
                   0.25f * score_up(w.gyro_range, c->play_gyro_range_th * 0.70f, c->play_gyro_range_th * 1.75f) +
                   0.20f * score_up(w.gyro_delta_mean, c->play_gyro_delta_th * 0.55f, c->play_gyro_delta_th * 1.60f) +
                   0.13f * score_up(w.acc_std, c->play_acc_std_th * 0.55f, c->play_acc_std_th * 1.35f) +
                   0.10f * w.burst_score;
    w.play_score = clampf_local(w.play_score, 0.0f, 100.0f);

    bool strict_rest =
        acc_near_1g &&
        w.acc_std < c->rest_acc_std_th &&
        w.acc_axis_std < c->rest_acc_axis_std_th &&
        w.acc_delta_mean < c->rest_acc_delta_th &&
        w.gyro_mean < c->rest_gyro_mean_th &&
        w.gyro_std < c->rest_gyro_std_th &&
        w.posture_std < c->rest_posture_std_th;

    bool soft_rest =
        acc_near_1g &&
        w.acc_std < 0.095f &&
        w.acc_axis_std < 0.125f &&
        w.acc_delta_mean < 0.035f &&
        w.gyro_mean < 70.0f &&
        w.gyro_std < 55.0f &&
        w.posture_std < 9.0f;

    bool passive_like =
        w.gyro_mean > 60.0f &&
        w.gyro_axis_std > 35.0f &&
        w.acc_std < 0.100f &&
        w.acc_axis_std < 0.135f &&
        w.acc_delta_mean < 0.038f;

    bool play_like =
        w.play_score >= 72.0f &&
        w.irregular_score >= 52.0f &&
        (w.gyro_std >= c->play_gyro_std_th ||
         w.gyro_range >= c->play_gyro_range_th ||
         w.gyro_delta_mean >= c->play_gyro_delta_th) &&
        (w.acc_std >= 0.16f || w.acc_delta_mean >= 0.055f || w.burst_score >= 65.0f);

    /*
     * v6 RUN 判断：
     * - 不能靠单次 gyro/转头触发；
     * - 必须有明显身体运动 acc_std/acc_delta；
     * - rhythm_score 不能太低，避免把 PLAY/甩头当 RUN。
     */
    bool run_body_motion =
        (w.acc_std >= c->run_acc_std_th && w.acc_delta_mean >= 0.085f) ||
        (w.acc_axis_std >= 0.42f && w.acc_delta_mean >= c->run_acc_delta_th) ||
        (w.acc_range >= 0.48f && w.acc_std >= 0.240f);

    bool run_gyro_support =
        w.gyro_mean >= c->run_gyro_mean_th ||
        w.gyro_std >= 62.0f;

    bool run_like =
        w.run_score >= 64.0f &&
        w.rhythm_score >= 32.0f &&
        w.irregular_score < 75.0f &&
        run_body_motion &&
        (run_gyro_support || w.activity_score >= 66.0f);

    bool trot_like =
        !run_like &&
        w.activity_score >= 48.0f &&
        (w.acc_std >= c->trot_acc_std_th ||
         w.acc_axis_std >= 0.32f ||
         w.acc_delta_mean >= 0.080f);

    /*
     * v4 关键修正：WALK 不能由 gyro 单独触发。
     * 项圈在脖子上，拿起/转动/甩一下经常会产生较高 gyro，
     * 但真正走路通常会同时有 acc_std、acc_axis_std、acc_delta 或 acc_range 的持续变化。
     */
    bool body_motion =
        w.acc_std >= c->walk_acc_std_th ||
        w.acc_axis_std >= c->walk_acc_axis_std_th ||
        w.acc_delta_mean >= c->walk_acc_delta_th ||
        w.acc_range >= 0.20f;

    bool gyro_support =
        w.gyro_mean >= c->walk_gyro_mean_th ||
        w.gyro_std >= c->walk_gyro_std_th;

    bool walk_like =
        body_motion &&
        (w.activity_score >= 38.0f || gyro_support || w.rhythm_score >= 22.0f) &&
        !passive_like;

    if (strict_rest || soft_rest || w.rest_score >= 72.0f)
        w.candidate = PET_STATE_REST;
    else if (passive_like)
        w.candidate = PET_STATE_PASSIVE_MOTION;
    else if (play_like)
        w.candidate = PET_STATE_PLAY;
    else if (run_like)
        w.candidate = PET_STATE_RUN;
    else if (trot_like)
        w.candidate = PET_STATE_TROT;
    else if (walk_like)
        w.candidate = PET_STATE_WALK;
    else
        w.candidate = PET_STATE_REST;

    return w;
}
