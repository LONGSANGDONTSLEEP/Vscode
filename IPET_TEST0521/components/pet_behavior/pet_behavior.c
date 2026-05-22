#include "pet_behavior.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct
{
    float sum;
    float sum2;
    float max;
    float min;
    uint32_t n;
} stat_win_t;

typedef struct
{
    bool valid;
    uint32_t end_ms;
    pet_state_t candidate;

    float acc_mean;
    float acc_std;
    float gyro_mean;
    float gyro_std;
    float acc_axis_std;
    float gyro_axis_std;
    float acc_delta_mean;
    float gyro_delta_mean;
    float acc_range;
    float gyro_range;
    float posture_std;

    float activity_score;
    float rest_score;
    float rhythm_score;
    float irregular_score;
    float burst_score;
    float run_score;
    float play_score;
} behavior_window_t;

typedef struct
{
    uint8_t rest;
    uint8_t walk;
    uint8_t trot;
    uint8_t run;
    uint8_t play;
    uint8_t passive;
    uint8_t active;
    uint8_t valid;

    float avg_activity;
    float avg_rest;
    float avg_rhythm;
    float avg_irregular;
    float avg_burst;
    float avg_run;
    float avg_play;
} history_summary_t;

struct pet_behavior_t
{
    pet_behavior_config_t cfg;

    pet_state_t state;       // 显示/上报状态
    pet_state_t raw_state;   // 内部多秒综合状态
    pet_state_t candidate;   // 当前短窗口候选状态

    pet_state_t pending_state;
    uint32_t pending_since_ms;
    uint32_t state_since_ms;
    uint32_t raw_state_since_ms;

    uint32_t rest_like_since_ms;
    bool rest_like_active;

    uint32_t low_g_since_ms;
    bool low_g_active;

    stat_win_t acc_norm_win;
    stat_win_t gyro_norm_win;
    stat_win_t ax_win;
    stat_win_t ay_win;
    stat_win_t az_win;
    stat_win_t gx_win;
    stat_win_t gy_win;
    stat_win_t gz_win;
    stat_win_t acc_delta_win;
    stat_win_t gyro_delta_win;
    stat_win_t pitch_win;
    stat_win_t roll_win;

    uint32_t win_start_ms;
    bool window_initialized;
    float last_pitch;
    float last_roll;

    bool have_last_sample;
    float last_ax_g;
    float last_ay_g;
    float last_az_g;
    float last_gx_dps;
    float last_gy_dps;
    float last_gz_dps;

    uint32_t last_active_ms;

    behavior_window_t history[PET_BEHAVIOR_HISTORY_MAX];
    uint8_t history_pos;
    uint8_t history_count;

    pet_behavior_result_t last_result;
};

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float score_up(float v, float lo, float hi)
{
    if (hi <= lo)
        return 0.0f;
    return clampf_local((v - lo) * 100.0f / (hi - lo), 0.0f, 100.0f);
}

static float score_down(float v, float lo, float hi)
{
    if (hi <= lo)
        return 0.0f;
    return clampf_local((hi - v) * 100.0f / (hi - lo), 0.0f, 100.0f);
}

static float min3f(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

static float vec_norm3(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

static float angle_diff_deg(float a, float b)
{
    float d = a - b;
    while (d > 180.0f)
        d -= 360.0f;
    while (d < -180.0f)
        d += 360.0f;
    return fabsf(d);
}

static void stat_reset(stat_win_t *s)
{
    s->sum = 0.0f;
    s->sum2 = 0.0f;
    s->max = -1e9f;
    s->min = 1e9f;
    s->n = 0;
}

static void stat_push(stat_win_t *s, float v)
{
    s->sum += v;
    s->sum2 += v * v;
    if (v > s->max)
        s->max = v;
    if (v < s->min)
        s->min = v;
    s->n++;
}

static float stat_mean(const stat_win_t *s)
{
    return s->n ? s->sum / s->n : 0.0f;
}

static float stat_std(const stat_win_t *s)
{
    if (!s->n)
        return 0.0f;

    float mean = stat_mean(s);
    float var = s->sum2 / s->n - mean * mean;
    if (var < 0.0f)
        var = 0.0f;

    return sqrtf(var);
}

static float stat_range(const stat_win_t *s)
{
    return s->n ? (s->max - s->min) : 0.0f;
}

static void reset_windows(pet_behavior_handle_t h, uint32_t now_ms)
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

void pet_behavior_default_config(pet_behavior_config_t *cfg)
{
    if (!cfg)
        return;

    memset(cfg, 0, sizeof(*cfg));

    cfg->sample_rate_hz = 50.0f;
    cfg->window_ms = 1000;
    cfg->history_window_count = 7;
    cfg->min_state_hold_ms = 2500;
    cfg->terminal_report_interval_ms = 5000;

    /* 默认先做“好用”的产品判断：细分类只写 raw_state，不直接显示。 */
    cfg->enable_fine_states = false;
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

    cfg->walk_acc_std_th = 0.095f;
    cfg->walk_acc_axis_std_th = 0.145f;
    cfg->walk_acc_delta_th = 0.040f;
    cfg->walk_gyro_mean_th = 45.0f;
    cfg->walk_gyro_std_th = 32.0f;

    cfg->trot_acc_std_th = 0.180f;
    cfg->run_acc_std_th = 0.320f;
    cfg->run_acc_delta_th = 0.130f;
    cfg->run_gyro_mean_th = 95.0f;

    cfg->play_gyro_std_th = 135.0f;
    cfg->play_gyro_range_th = 330.0f;
    cfg->play_gyro_delta_th = 95.0f;
    cfg->play_acc_std_th = 0.38f;
    cfg->play_acc_delta_th = 0.095f;

    cfg->active_enter_votes = 4;
    cfg->rest_enter_votes = 5;
    cfg->run_enter_votes = 3;
    cfg->play_enter_votes = 3;

    cfg->impact_acc_norm_th = 3.0f;
    cfg->jump_low_g_th = 0.50f;
    cfg->jump_land_g_th = 1.85f;
    cfg->shake_gyro_th = 450.0f;
    cfg->scratch_gyro_std_min = 120.0f;
    cfg->scratch_acc_std_min = 0.10f;

    cfg->sleep_after_rest_ms = 5 * 60 * 1000;
    cfg->not_worn_after_rest_ms = 20 * 60 * 1000;
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
        h->cfg.history_window_count = 7;
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

static behavior_window_t build_window_feature(pet_behavior_handle_t h, uint32_t now_ms)
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

    bool run_like =
        w.run_score >= 68.0f &&
        w.rhythm_score >= 35.0f &&
        w.irregular_score < 78.0f &&
        (w.acc_std >= c->run_acc_std_th ||
         w.acc_delta_mean >= c->run_acc_delta_th ||
         w.gyro_mean >= c->run_gyro_mean_th);

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

static void push_history(pet_behavior_handle_t h, const behavior_window_t *w)
{
    h->history[h->history_pos] = *w;
    h->history_pos = (uint8_t)((h->history_pos + 1) % h->cfg.history_window_count);
    if (h->history_count < h->cfg.history_window_count)
        h->history_count++;
}

static history_summary_t summarize_history(pet_behavior_handle_t h)
{
    history_summary_t s;
    memset(&s, 0, sizeof(s));

    uint8_t n = h->history_count;
    if (n > h->cfg.history_window_count)
        n = h->cfg.history_window_count;

    for (uint8_t i = 0; i < n; ++i)
    {
        const behavior_window_t *w = &h->history[i];
        if (!w->valid)
            continue;

        s.valid++;
        s.avg_activity += w->activity_score;
        s.avg_rest += w->rest_score;
        s.avg_rhythm += w->rhythm_score;
        s.avg_irregular += w->irregular_score;
        s.avg_burst += w->burst_score;
        s.avg_run += w->run_score;
        s.avg_play += w->play_score;

        switch (w->candidate)
        {
        case PET_STATE_REST:
        case PET_STATE_SLEEP:
        case PET_STATE_NOT_WORN:
            s.rest++;
            break;
        case PET_STATE_WALK:
            s.walk++;
            s.active++;
            break;
        case PET_STATE_TROT:
            s.trot++;
            s.active++;
            break;
        case PET_STATE_RUN:
            s.run++;
            s.active++;
            break;
        case PET_STATE_PLAY:
            s.play++;
            s.active++;
            break;
        case PET_STATE_PASSIVE_MOTION:
            s.passive++;
            break;
        default:
            break;
        }
    }

    if (s.valid)
    {
        float inv = 1.0f / (float)s.valid;
        s.avg_activity *= inv;
        s.avg_rest *= inv;
        s.avg_rhythm *= inv;
        s.avg_irregular *= inv;
        s.avg_burst *= inv;
        s.avg_run *= inv;
        s.avg_play *= inv;
    }

    return s;
}

static void update_rest_timer(pet_behavior_handle_t h, bool rest_like, uint32_t now_ms)
{
    if (rest_like)
    {
        if (!h->rest_like_active)
        {
            h->rest_like_active = true;
            h->rest_like_since_ms = now_ms;
        }
    }
    else
    {
        h->rest_like_active = false;
        h->rest_like_since_ms = 0;
        h->last_active_ms = now_ms;
    }
}

static pet_state_t decide_raw_from_history(pet_behavior_handle_t h,
                                           const history_summary_t *s,
                                           uint32_t now_ms,
                                           float *confidence_out)
{
    const pet_behavior_config_t *c = &h->cfg;
    float conf = 45.0f;
    pet_state_t raw = PET_STATE_REST;

    if (s->valid == 0)
    {
        if (confidence_out)
            *confidence_out = 0.0f;
        return PET_STATE_UNKNOWN;
    }

    bool enough_rest = s->rest >= c->rest_enter_votes ||
                       (s->rest >= (s->valid >= 5 ? 4 : s->valid) && s->avg_rest >= 70.0f);

    bool enough_play = s->play >= c->play_enter_votes &&
                       s->avg_play >= 62.0f &&
                       s->avg_irregular >= 48.0f;

    bool enough_run = s->run >= c->run_enter_votes &&
                      s->avg_run >= 58.0f &&
                      s->avg_rhythm >= 30.0f &&
                      s->avg_irregular < 78.0f;

    bool enough_active =
        s->active >= c->active_enter_votes ||
        (s->active >= 3 && s->avg_activity >= 48.0f && s->avg_rhythm >= 18.0f);

    if (enough_rest && !enough_active)
    {
        raw = PET_STATE_REST;
        conf = clampf_local(55.0f + s->avg_rest * 0.45f, 0.0f, 100.0f);
    }
    else if (enough_play)
    {
        raw = PET_STATE_PLAY;
        conf = clampf_local(45.0f + s->avg_play * 0.35f + s->avg_irregular * 0.20f, 0.0f, 100.0f);
    }
    else if (enough_run)
    {
        raw = PET_STATE_RUN;
        conf = clampf_local(45.0f + s->avg_run * 0.35f + s->avg_rhythm * 0.20f, 0.0f, 100.0f);
    }
    else if ((s->trot + s->run) >= 3 && s->avg_activity >= 45.0f)
    {
        raw = PET_STATE_TROT;
        conf = clampf_local(45.0f + s->avg_activity * 0.40f, 0.0f, 92.0f);
    }
    else if (s->passive >= 2 && s->active == 0)
    {
        raw = PET_STATE_PASSIVE_MOTION;
        conf = 65.0f;
    }
    else if (enough_active)
    {
        raw = PET_STATE_WALK;
        conf = clampf_local(50.0f + s->avg_activity * 0.35f + s->avg_rhythm * 0.15f, 0.0f, 90.0f);
    }
    else
    {
        raw = PET_STATE_REST;
        conf = clampf_local(45.0f + s->avg_rest * 0.35f, 0.0f, 85.0f);
    }

    bool rest_like = (raw == PET_STATE_REST || raw == PET_STATE_PASSIVE_MOTION) && s->avg_activity < 38.0f;
    update_rest_timer(h, rest_like, now_ms);

    uint32_t rest_ms = (h->rest_like_active && h->rest_like_since_ms > 0)
                           ? now_ms - h->rest_like_since_ms
                           : 0;

    if (raw == PET_STATE_REST && rest_ms >= c->not_worn_after_rest_ms)
        raw = PET_STATE_NOT_WORN;
    else if (raw == PET_STATE_REST && rest_ms >= c->sleep_after_rest_ms)
        raw = PET_STATE_SLEEP;

    if (confidence_out)
        *confidence_out = conf;

    return raw;
}

static uint32_t transition_required_ms(pet_state_t current, pet_state_t target)
{
    if (current == PET_STATE_UNKNOWN)
        return 0;
    if (current == target)
        return 0;

    /*
     * v4 产品策略：终端显示给用户看的状态要稳。
     * REST -> WALK 慢一点：必须经过历史投票 + 这里的持续确认。
     * WALK -> REST 也慢一点：避免走两步停一下就频繁跳。
     */
    if (target == PET_STATE_REST || target == PET_STATE_SLEEP || target == PET_STATE_NOT_WORN)
        return 3500;

    if ((current == PET_STATE_REST || current == PET_STATE_SLEEP || current == PET_STATE_NOT_WORN) &&
        (target == PET_STATE_WALK || target == PET_STATE_TROT || target == PET_STATE_RUN))
        return 2500;

    /* PLAY/RUN 已经经过多窗口投票，这里再给一点兜底即可。 */
    if (target == PET_STATE_PLAY || target == PET_STATE_RUN)
        return 1000;

    /* PLAY/RUN 回 WALK 要快一些，但不直接跳 REST。 */
    if ((current == PET_STATE_PLAY || current == PET_STATE_RUN) && target == PET_STATE_WALK)
        return 1000;

    return 2000;
}

static pet_state_t apply_raw_state_machine(pet_behavior_handle_t h,
                                           pet_state_t target,
                                           uint32_t now_ms)
{
    if (h->raw_state == PET_STATE_UNKNOWN)
    {
        h->raw_state = target;
        h->raw_state_since_ms = now_ms;
        h->pending_state = target;
        h->pending_since_ms = now_ms;
        return h->raw_state;
    }

    if (target == h->raw_state)
    {
        h->pending_state = target;
        h->pending_since_ms = now_ms;
        return h->raw_state;
    }

    if (target != h->pending_state)
    {
        h->pending_state = target;
        h->pending_since_ms = now_ms;
        return h->raw_state;
    }

    uint32_t need_ms = transition_required_ms(h->raw_state, target);
    if ((now_ms - h->pending_since_ms) >= need_ms)
    {
        h->raw_state = target;
        h->raw_state_since_ms = now_ms;
    }

    return h->raw_state;
}

static pet_state_t map_raw_to_display(pet_behavior_handle_t h, pet_state_t raw)
{
    if (h->cfg.enable_fine_states)
        return raw;

    switch (raw)
    {
    case PET_STATE_TROT:
    case PET_STATE_RUN:
    case PET_STATE_PLAY:
        return PET_STATE_WALK;

    case PET_STATE_PASSIVE_MOTION:
        return h->cfg.passive_motion_as_rest ? PET_STATE_REST : PET_STATE_PASSIVE_MOTION;

    default:
        return raw;
    }
}

static void update_display_state(pet_behavior_handle_t h, pet_state_t display, uint32_t now_ms)
{
    if (h->state == PET_STATE_UNKNOWN)
    {
        h->state = display;
        h->state_since_ms = now_ms;
        return;
    }

    if (display != h->state)
    {
        h->state = display;
        h->state_since_ms = now_ms;
    }
}

static uint32_t detect_events_from_window(pet_behavior_handle_t h,
                                          const qmi8658a_sample_t *last_sample,
                                          float acc_norm,
                                          float gyro_norm,
                                          const behavior_window_t *w,
                                          uint32_t now_ms)
{
    const pet_behavior_config_t *c = &h->cfg;
    uint32_t ev = PET_EVENT_NONE;

    if (acc_norm >= c->impact_acc_norm_th || w->acc_range > 1.65f)
        ev |= PET_EVENT_IMPACT;

    if (acc_norm < c->jump_low_g_th)
    {
        if (!h->low_g_active)
        {
            h->low_g_active = true;
            h->low_g_since_ms = now_ms;
        }
    }

    if (h->low_g_active)
    {
        uint32_t low_g_ms = now_ms - h->low_g_since_ms;
        if ((acc_norm > c->jump_land_g_th || w->acc_range > 1.20f) &&
            low_g_ms >= 30 &&
            low_g_ms <= 700)
        {
            ev |= PET_EVENT_JUMP;
            h->low_g_active = false;
        }
        else if (low_g_ms > 900)
        {
            h->low_g_active = false;
        }
    }

    if (gyro_norm > c->shake_gyro_th ||
        (w->gyro_range > 520.0f && w->gyro_delta_mean > 120.0f))
    {
        ev |= PET_EVENT_SHAKE;
    }

    bool high_activity =
        w->acc_std > 0.20f ||
        w->acc_delta_mean > 0.060f ||
        w->gyro_std > 110.0f ||
        gyro_norm > 280.0f;

    if (!high_activity &&
        !(ev & PET_EVENT_SHAKE) &&
        gyro_norm > 90.0f &&
        gyro_norm < 260.0f &&
        acc_norm > 0.80f &&
        acc_norm < 1.45f &&
        w->gyro_std >= c->scratch_gyro_std_min &&
        w->acc_std >= c->scratch_acc_std_min &&
        w->acc_std < 0.24f)
    {
        ev |= PET_EVENT_SCRATCH;
    }

    float pitch = atan2f(-last_sample->ax_g,
                         sqrtf(last_sample->ay_g * last_sample->ay_g +
                               last_sample->az_g * last_sample->az_g)) *
                  180.0f / (float)M_PI;

    float roll = atan2f(last_sample->ay_g, last_sample->az_g) *
                 180.0f / (float)M_PI;

    float d_angle = angle_diff_deg(pitch, h->last_pitch) +
                    angle_diff_deg(roll, h->last_roll);

    bool attitude_valid = (acc_norm > 0.75f && acc_norm < 1.30f);

    if (attitude_valid &&
        d_angle > 100.0f &&
        gyro_norm > 100.0f &&
        gyro_norm < 650.0f)
    {
        ev |= PET_EVENT_ROLL_OVER;
    }

    h->last_pitch = pitch;
    h->last_roll = roll;

    return ev;
}

bool pet_behavior_update(pet_behavior_handle_t h,
                         const qmi8658a_sample_t *sample,
                         uint32_t now_ms,
                         pet_behavior_result_t *out)
{
    if (!h || !sample || !out)
        return false;

    if (!h->window_initialized)
    {
        reset_windows(h, now_ms);
        h->state_since_ms = now_ms;
        h->raw_state_since_ms = now_ms;
        h->pending_since_ms = now_ms;
    }

    float acc_norm = vec_norm3(sample->ax_g,
                               sample->ay_g,
                               sample->az_g);

    float gyro_norm = vec_norm3(sample->gx_dps,
                                sample->gy_dps,
                                sample->gz_dps);

    float pitch = atan2f(-sample->ax_g,
                         sqrtf(sample->ay_g * sample->ay_g + sample->az_g * sample->az_g)) *
                  180.0f / (float)M_PI;

    float roll = atan2f(sample->ay_g, sample->az_g) *
                 180.0f / (float)M_PI;

    stat_push(&h->acc_norm_win, acc_norm);
    stat_push(&h->gyro_norm_win, gyro_norm);
    stat_push(&h->ax_win, sample->ax_g);
    stat_push(&h->ay_win, sample->ay_g);
    stat_push(&h->az_win, sample->az_g);
    stat_push(&h->gx_win, sample->gx_dps);
    stat_push(&h->gy_win, sample->gy_dps);
    stat_push(&h->gz_win, sample->gz_dps);
    stat_push(&h->pitch_win, pitch);
    stat_push(&h->roll_win, roll);

    if (h->have_last_sample)
    {
        float acc_delta = vec_norm3(sample->ax_g - h->last_ax_g,
                                    sample->ay_g - h->last_ay_g,
                                    sample->az_g - h->last_az_g);
        float gyro_delta = vec_norm3(sample->gx_dps - h->last_gx_dps,
                                     sample->gy_dps - h->last_gy_dps,
                                     sample->gz_dps - h->last_gz_dps);
        stat_push(&h->acc_delta_win, acc_delta);
        stat_push(&h->gyro_delta_win, gyro_delta);
    }

    h->have_last_sample = true;
    h->last_ax_g = sample->ax_g;
    h->last_ay_g = sample->ay_g;
    h->last_az_g = sample->az_g;
    h->last_gx_dps = sample->gx_dps;
    h->last_gy_dps = sample->gy_dps;
    h->last_gz_dps = sample->gz_dps;

    bool window_ready = (now_ms - h->win_start_ms) >= h->cfg.window_ms;
    uint32_t events = PET_EVENT_NONE;
    behavior_window_t w;
    memset(&w, 0, sizeof(w));

    float confidence = h->last_result.confidence;

    if (window_ready)
    {
        w = build_window_feature(h, now_ms);
        h->candidate = w.candidate;

        /* 事件检测必须在 reset_windows() 前做，否则 range/delta 已经被清空。 */
        events = detect_events_from_window(h, sample, acc_norm, gyro_norm, &w, now_ms);

        push_history(h, &w);
        history_summary_t hs = summarize_history(h);

        pet_state_t target_raw = decide_raw_from_history(h, &hs, now_ms, &confidence);
        pet_state_t raw = apply_raw_state_machine(h, target_raw, now_ms);
        pet_state_t display = map_raw_to_display(h, raw);
        update_display_state(h, display, now_ms);

        pet_behavior_result_t r = {
            .state = h->state,
            .candidate_state = h->candidate,
            .raw_state = h->raw_state,
            .events = events,
            .acc_norm_g = acc_norm,
            .gyro_norm_dps = gyro_norm,
            .pitch_deg = pitch,
            .roll_deg = roll,
            .acc_norm_mean = w.acc_mean,
            .acc_norm_std = w.acc_std,
            .gyro_norm_mean = w.gyro_mean,
            .gyro_norm_std = w.gyro_std,
            .acc_axis_std = w.acc_axis_std,
            .gyro_axis_std = w.gyro_axis_std,
            .acc_delta_mean = w.acc_delta_mean,
            .gyro_delta_mean = w.gyro_delta_mean,
            .acc_range = w.acc_range,
            .gyro_range = w.gyro_range,
            .posture_std = w.posture_std,
            .activity_score = w.activity_score,
            .rest_score = w.rest_score,
            .rhythm_score = w.rhythm_score,
            .irregular_score = w.irregular_score,
            .burst_score = w.burst_score,
            .confidence = confidence,
            .history_count = hs.valid,
            .vote_rest = hs.rest,
            .vote_walk = hs.walk,
            .vote_trot = hs.trot,
            .vote_run = hs.run,
            .vote_play = hs.play,
            .vote_passive = hs.passive,
            .state_duration_ms = now_ms - h->state_since_ms,
            .rest_like_duration_ms = (h->rest_like_active && h->rest_like_since_ms > 0)
                                         ? now_ms - h->rest_like_since_ms
                                         : 0,
            .sample_count = h->last_result.sample_count + 1,
            .algo_version = PET_BEHAVIOR_ALGO_VERSION,
        };

        h->last_result = r;
        *out = r;

        reset_windows(h, now_ms);
        return true;
    }

    /* 非窗口边界：返回最近一次结果，便于上层读取；函数返回 false，不建议打印日志。 */
    pet_behavior_result_t r = h->last_result;
    r.acc_norm_g = acc_norm;
    r.gyro_norm_dps = gyro_norm;
    r.pitch_deg = pitch;
    r.roll_deg = roll;
    r.events = PET_EVENT_NONE;
    r.sample_count = h->last_result.sample_count + 1;
    r.algo_version = PET_BEHAVIOR_ALGO_VERSION;
    h->last_result = r;
    *out = r;

    return false;
}

const char *pet_state_to_str(pet_state_t state)
{
    switch (state)
    {
    case PET_STATE_UNKNOWN:
        return "UNKNOWN";
    case PET_STATE_NOT_WORN:
        return "NOT_WORN";
    case PET_STATE_SLEEP:
        return "SLEEP";
    case PET_STATE_REST:
        return "REST";
    case PET_STATE_WALK:
        return "WALK";
    case PET_STATE_TROT:
        return "TROT";
    case PET_STATE_RUN:
        return "RUN";
    case PET_STATE_PLAY:
        return "PLAY";
    case PET_STATE_PASSIVE_MOTION:
        return "PASSIVE_MOTION";
    case PET_STATE_ABNORMAL_INACTIVE:
        return "ABNORMAL_INACTIVE";
    default:
        return "INVALID";
    }
}

void pet_events_to_str(uint32_t events, char *buf, uint32_t buf_len)
{
    if (!buf || buf_len == 0)
        return;

    buf[0] = '\0';

    if (events == PET_EVENT_NONE)
    {
        snprintf(buf, buf_len, "NONE");
        return;
    }

    bool first = true;

    struct
    {
        uint32_t bit;
        const char *name;
    } map[] = {
        {PET_EVENT_SHAKE, "SHAKE"},
        {PET_EVENT_SCRATCH, "SCRATCH"},
        {PET_EVENT_JUMP, "JUMP"},
        {PET_EVENT_IMPACT, "IMPACT"},
        {PET_EVENT_ROLL_OVER, "ROLL_OVER"},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
    {
        if (events & map[i].bit)
        {
            size_t used = strlen(buf);
            snprintf(buf + used,
                     buf_len > used ? buf_len - used : 0,
                     "%s%s",
                     first ? "" : "|",
                     map[i].name);
            first = false;
        }
    }
}
