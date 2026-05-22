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

struct pet_behavior_t
{
    pet_behavior_config_t cfg;

    pet_state_t state;
    pet_state_t candidate;
    uint32_t candidate_since_ms;
    uint32_t state_since_ms;

    uint32_t rest_like_since_ms;
    bool rest_like_active;

    uint32_t low_g_since_ms;
    bool low_g_active;

    stat_win_t acc_norm_win;
    stat_win_t gyro_norm_win;

    /* 新增：六轴单独窗口。慢走时 acc_norm 可能接近 1g，但单轴会变化。 */
    stat_win_t ax_win;
    stat_win_t ay_win;
    stat_win_t az_win;
    stat_win_t gx_win;
    stat_win_t gy_win;
    stat_win_t gz_win;

    /* 新增：相邻采样变化量。用于区分真正移动和站着/趴着轻微晃动。 */
    stat_win_t acc_delta_win;
    stat_win_t gyro_delta_win;

    /* 新增：姿态窗口。用于判断站/趴稳定和翻滚。 */
    stat_win_t pitch_win;
    stat_win_t roll_win;

    uint32_t win_start_ms;
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
    uint32_t last_play_ms;

    pet_behavior_result_t last_result;
};

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
}

void pet_behavior_default_config(pet_behavior_config_t *cfg)
{
    if (!cfg)
        return;

    memset(cfg, 0, sizeof(*cfg));

    cfg->sample_rate_hz = 50.0f;

    /* 1 秒窗口，配合 1 秒 POST/终端输出。 */
    cfg->window_ms = 1000;

    /* 这里不再简单使用一个固定防抖，apply_state_debounce() 里做非对称防抖。 */
    cfg->min_state_hold_ms = 1200;

    cfg->rest_acc_std_th = 0.045f;
    cfg->rest_gyro_mean_th = 18.0f;

    /* acc_norm 阈值不要太高；慢走时 acc_norm_std 经常不大。 */
    cfg->walk_acc_std_th = 0.085f;
    cfg->trot_acc_std_th = 0.180f;
    cfg->run_acc_std_th = 0.320f;

    /* PLAY 不再只靠 gyro_mean，主要看 gyro_std + burst。 */
    cfg->play_gyro_std_th = 135.0f;

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
    {
        h->cfg = *cfg;
    }
    else
    {
        pet_behavior_default_config(&h->cfg);
    }

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

    h->state = PET_STATE_UNKNOWN;
    h->candidate = PET_STATE_UNKNOWN;
    h->candidate_since_ms = 0;
    h->state_since_ms = 0;

    h->rest_like_since_ms = 0;
    h->rest_like_active = false;

    h->low_g_since_ms = 0;
    h->low_g_active = false;

    h->last_pitch = 0.0f;
    h->last_roll = 0.0f;

    h->have_last_sample = false;
    h->last_ax_g = 0.0f;
    h->last_ay_g = 0.0f;
    h->last_az_g = 0.0f;
    h->last_gx_dps = 0.0f;
    h->last_gy_dps = 0.0f;
    h->last_gz_dps = 0.0f;

    h->last_active_ms = 0;
    h->last_play_ms = 0;

    memset(&h->last_result, 0, sizeof(h->last_result));
    reset_windows(h, 0);
}

static pet_state_t classify_from_window(pet_behavior_handle_t h,
                                        float acc_mean,
                                        float acc_std,
                                        float gyro_mean,
                                        float gyro_std,
                                        uint32_t now_ms)
{
    const pet_behavior_config_t *c = &h->cfg;

    float ax_std = stat_std(&h->ax_win);
    float ay_std = stat_std(&h->ay_win);
    float az_std = stat_std(&h->az_win);
    float gx_std = stat_std(&h->gx_win);
    float gy_std = stat_std(&h->gy_win);
    float gz_std = stat_std(&h->gz_win);

    float acc_axis_std = sqrtf(ax_std * ax_std + ay_std * ay_std + az_std * az_std);
    float gyro_axis_std = sqrtf(gx_std * gx_std + gy_std * gy_std + gz_std * gz_std);
    float acc_delta_mean = stat_mean(&h->acc_delta_win);
    float gyro_delta_mean = stat_mean(&h->gyro_delta_win);
    float acc_range = stat_range(&h->acc_norm_win);
    float gyro_range = stat_range(&h->gyro_norm_win);
    float pitch_std = stat_std(&h->pitch_win);
    float roll_std = stat_std(&h->roll_win);
    float posture_std = pitch_std + roll_std;

    bool acc_near_1g = fabsf(acc_mean - 1.0f) < 0.22f;

    /*
     * 真静止：站着不动、趴着不动都属于 REST。
     * 如果你需要区分 STAND 和 REST，需要在 pet_state_t 里新增 PET_STATE_STAND。
     */
    bool strict_rest =
        acc_near_1g &&
        acc_std < c->rest_acc_std_th &&
        acc_axis_std < 0.075f &&
        acc_delta_mean < 0.018f &&
        gyro_mean < c->rest_gyro_mean_th &&
        gyro_std < 18.0f &&
        posture_std < 5.0f;

    /* 趴着喘气：允许轻微周期起伏，但不能有明显单轴步态/jerk。 */
    bool panting_rest =
        acc_near_1g &&
        acc_std < 0.115f &&
        acc_axis_std < 0.155f &&
        acc_delta_mean < 0.040f &&
        gyro_mean < 75.0f &&
        gyro_std < 65.0f &&
        posture_std < 12.0f;

    bool motion_like =
        acc_std >= c->walk_acc_std_th ||
        acc_axis_std >= 0.125f ||
        acc_delta_mean >= 0.030f ||
        gyro_mean >= 35.0f ||
        gyro_std >= 28.0f ||
        posture_std >= 8.0f;

    bool burst_like =
        acc_range > 0.90f ||
        gyro_range > 280.0f ||
        gyro_delta_mean > 90.0f;

    /*
     * PLAY 不能只靠 gyro_mean，否则慢跑/奔跑很容易被归成 PLAY。
     * 这里要求“高 gyro_std 或 burst”，并且同时有加速度/jerk 配合。
     */
    bool play_like =
        (gyro_std >= c->play_gyro_std_th && (acc_std > 0.18f || acc_delta_mean > 0.055f || gyro_mean > 130.0f)) ||
        (gyro_mean > 260.0f && gyro_std > 75.0f) ||
        (acc_std > 0.55f && gyro_std > 85.0f) ||
        burst_like;

    if (play_like)
    {
        h->last_active_ms = now_ms;
        h->last_play_ms = now_ms;
        h->rest_like_active = false;
        h->rest_like_since_ms = 0;
        return PET_STATE_PLAY;
    }

    if (motion_like)
    {
        h->last_active_ms = now_ms;
    }

    /* 丢球游戏有短暂停顿：刚刚发生过 PLAY，不要 1~2 秒停顿就掉 REST。 */
    bool recent_play_pause =
        h->last_play_ms > 0 &&
        (now_ms - h->last_play_ms) < 5000 &&
        !strict_rest;

    if (recent_play_pause)
    {
        return PET_STATE_PLAY;
    }

    if (strict_rest || panting_rest)
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
    }

    uint32_t rest_ms = (h->rest_like_active && h->rest_like_since_ms > 0)
                           ? now_ms - h->rest_like_since_ms
                           : 0;

    if ((strict_rest || panting_rest) && rest_ms >= c->not_worn_after_rest_ms)
    {
        return PET_STATE_NOT_WORN;
    }

    if ((strict_rest || panting_rest) && rest_ms >= c->sleep_after_rest_ms)
    {
        return PET_STATE_SLEEP;
    }

    if (strict_rest || panting_rest)
    {
        return PET_STATE_REST;
    }

    /* 被抱着/被动车动：角速度有变化，但加速度步态和 jerk 不明显。 */
    if (gyro_mean > 55.0f &&
        gyro_axis_std > 35.0f &&
        acc_std < 0.080f &&
        acc_axis_std < 0.105f &&
        acc_delta_mean < 0.028f)
    {
        return PET_STATE_PASSIVE_MOTION;
    }

    /* 二级速度分类：优先用 acc_norm，其次用单轴 std 和 jerk。 */
    if (acc_std >= c->run_acc_std_th || acc_axis_std >= 0.52f || acc_delta_mean >= 0.135f)
    {
        return PET_STATE_RUN;
    }

    if (acc_std >= c->trot_acc_std_th || acc_axis_std >= 0.32f || acc_delta_mean >= 0.080f)
    {
        return PET_STATE_TROT;
    }

    if (acc_std >= c->walk_acc_std_th || acc_axis_std >= 0.125f || acc_delta_mean >= 0.030f ||
        (gyro_mean > 35.0f && gyro_std > 22.0f && posture_std > 4.0f))
    {
        return PET_STATE_WALK;
    }

    return PET_STATE_REST;
}

static uint32_t detect_events(pet_behavior_handle_t h,
                              const qmi8658a_sample_t *s,
                              float acc_norm,
                              float gyro_norm,
                              float gyro_std,
                              float acc_std,
                              uint32_t now_ms)
{
    const pet_behavior_config_t *c = &h->cfg;
    uint32_t ev = PET_EVENT_NONE;

    float acc_delta_mean = stat_mean(&h->acc_delta_win);
    float gyro_delta_mean = stat_mean(&h->gyro_delta_win);
    float acc_range = stat_range(&h->acc_norm_win);
    float gyro_range = stat_range(&h->gyro_norm_win);

    if (acc_norm >= c->impact_acc_norm_th)
    {
        ev |= PET_EVENT_IMPACT;
    }

    /* Jump: 先低 g，再高 g；同时允许强烈 acc_range 触发。 */
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

        if ((acc_norm > c->jump_land_g_th || acc_range > 1.20f) &&
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
        (gyro_range > 520.0f && gyro_delta_mean > 120.0f))
    {
        ev |= PET_EVENT_SHAKE;
    }

    bool high_activity =
        acc_std > 0.20f ||
        acc_delta_mean > 0.060f ||
        gyro_std > 110.0f ||
        gyro_norm > 280.0f;

    /* 抓挠只在中等强度、相对稳定姿态下判断；剧烈玩耍时不报 SCRATCH。 */
    if (!high_activity &&
        !(ev & PET_EVENT_SHAKE) &&
        gyro_norm > 90.0f &&
        gyro_norm < 260.0f &&
        acc_norm > 0.80f &&
        acc_norm < 1.45f &&
        gyro_std >= c->scratch_gyro_std_min &&
        acc_std >= c->scratch_acc_std_min &&
        acc_std < 0.24f)
    {
        ev |= PET_EVENT_SCRATCH;
    }

    float pitch = atan2f(-s->ax_g,
                         sqrtf(s->ay_g * s->ay_g + s->az_g * s->az_g)) *
                  180.0f / (float)M_PI;

    float roll = atan2f(s->ay_g, s->az_g) *
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

static uint32_t debounce_required_ms(pet_state_t current, pet_state_t candidate)
{
    if (current == PET_STATE_UNKNOWN)
        return 0;

    if (candidate == PET_STATE_PLAY)
        return 500;

    if (current == PET_STATE_PLAY && candidate == PET_STATE_REST)
        return 6000;

    if ((current == PET_STATE_WALK || current == PET_STATE_TROT || current == PET_STATE_RUN) &&
        candidate == PET_STATE_REST)
        return 3500;

    if (current == PET_STATE_REST &&
        (candidate == PET_STATE_WALK || candidate == PET_STATE_TROT || candidate == PET_STATE_RUN))
        return 800;

    if (candidate == PET_STATE_REST)
        return 2500;

    return 1200;
}

static void apply_state_debounce(pet_behavior_handle_t h,
                                 pet_state_t candidate,
                                 uint32_t now_ms)
{
    if (h->state == PET_STATE_UNKNOWN)
    {
        h->state = candidate;
        h->state_since_ms = now_ms;
        h->candidate = candidate;
        h->candidate_since_ms = now_ms;
        return;
    }

    if (candidate == h->state)
    {
        h->candidate = candidate;
        h->candidate_since_ms = now_ms;
        return;
    }

    if (candidate != h->candidate)
    {
        h->candidate = candidate;
        h->candidate_since_ms = now_ms;
        return;
    }

    uint32_t need_ms = debounce_required_ms(h->state, candidate);
    if ((now_ms - h->candidate_since_ms) >= need_ms)
    {
        h->state = candidate;
        h->state_since_ms = now_ms;
    }
}

bool pet_behavior_update(pet_behavior_handle_t h,
                         const qmi8658a_sample_t *sample,
                         uint32_t now_ms,
                         pet_behavior_result_t *out)
{
    if (!h || !sample || !out)
    {
        return false;
    }

    if (h->win_start_ms == 0)
    {
        reset_windows(h, now_ms);
        h->state_since_ms = now_ms;
        h->candidate_since_ms = now_ms;
    }

    float acc_norm = vec_norm3(sample->ax_g,
                               sample->ay_g,
                               sample->az_g);

    float gyro_norm = vec_norm3(sample->gx_dps,
                                sample->gy_dps,
                                sample->gz_dps);

    float pitch = atan2f(-sample->ax_g,
                         sqrtf(sample->ay_g * sample->ay_g +
                               sample->az_g * sample->az_g)) *
                  180.0f / (float)M_PI;

    float roll = atan2f(sample->ay_g,
                        sample->az_g) *
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

    float acc_mean = h->last_result.acc_norm_mean;
    float acc_std = h->last_result.acc_norm_std;
    float gyro_mean = h->last_result.gyro_norm_mean;
    float gyro_std = h->last_result.gyro_norm_std;

    pet_state_t candidate = h->candidate;

    if (window_ready)
    {
        acc_mean = stat_mean(&h->acc_norm_win);
        acc_std = stat_std(&h->acc_norm_win);
        gyro_mean = stat_mean(&h->gyro_norm_win);
        gyro_std = stat_std(&h->gyro_norm_win);

        candidate = classify_from_window(h,
                                         acc_mean,
                                         acc_std,
                                         gyro_mean,
                                         gyro_std,
                                         now_ms);

        apply_state_debounce(h, candidate, now_ms);
        reset_windows(h, now_ms);
    }

    uint32_t events = detect_events(h,
                                    sample,
                                    acc_norm,
                                    gyro_norm,
                                    gyro_std,
                                    acc_std,
                                    now_ms);

    pet_behavior_result_t r = {
        .state = h->state,
        .candidate_state = candidate,
        .events = events,
        .acc_norm_g = acc_norm,
        .gyro_norm_dps = gyro_norm,
        .pitch_deg = pitch,
        .roll_deg = roll,
        .acc_norm_mean = acc_mean,
        .acc_norm_std = acc_std,
        .gyro_norm_mean = gyro_mean,
        .gyro_norm_std = gyro_std,
        .state_duration_ms = now_ms - h->state_since_ms,
        .rest_like_duration_ms = (h->rest_like_active && h->rest_like_since_ms > 0)
                                     ? now_ms - h->rest_like_since_ms
                                     : 0,
        .sample_count = h->last_result.sample_count + 1,
    };

    h->last_result = r;
    *out = r;

    return window_ready;
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
