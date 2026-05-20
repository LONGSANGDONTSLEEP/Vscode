#include "pet_behavior.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float sum;
    float sum2;
    float max;
    float min;
    uint32_t n;
} stat_win_t;

struct pet_behavior_t {
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

    uint32_t win_start_ms;
    float last_pitch;
    float last_roll;

    pet_behavior_result_t last_result;
};

static float vec_norm3(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
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
    if (v > s->max) s->max = v;
    if (v < s->min) s->min = v;
    s->n++;
}

static float stat_mean(const stat_win_t *s)
{
    return s->n ? s->sum / s->n : 0.0f;
}

static float stat_std(const stat_win_t *s)
{
    if (!s->n) return 0.0f;
    float mean = stat_mean(s);
    float var = s->sum2 / s->n - mean * mean;
    if (var < 0.0f) var = 0.0f;
    return sqrtf(var);
}

static void reset_windows(pet_behavior_handle_t h, uint32_t now_ms)
{
    stat_reset(&h->acc_norm_win);
    stat_reset(&h->gyro_norm_win);
    h->win_start_ms = now_ms;
}

void pet_behavior_default_config(pet_behavior_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    cfg->sample_rate_hz = 50.0f;
    cfg->window_ms = 1000;
    cfg->min_state_hold_ms = 3000;

    // 下面是第一版经验阈值。真实狗/猫佩戴后需要根据日志调参。
    cfg->rest_acc_std_th = 0.025f;
    cfg->rest_gyro_mean_th = 3.0f;
    cfg->walk_acc_std_th = 0.035f;
    cfg->trot_acc_std_th = 0.080f;
    cfg->run_acc_std_th = 0.160f;
    cfg->play_gyro_std_th = 80.0f;

    cfg->impact_acc_norm_th = 3.0f;
    cfg->jump_low_g_th = 0.45f;
    cfg->jump_land_g_th = 2.0f;
    cfg->shake_gyro_th = 180.0f;
    cfg->scratch_gyro_std_min = 25.0f;
    cfg->scratch_acc_std_min = 0.03f;

    cfg->sleep_after_rest_ms = 5 * 60 * 1000;
    cfg->not_worn_after_rest_ms = 20 * 60 * 1000;
}

pet_behavior_handle_t pet_behavior_create(const pet_behavior_config_t *cfg)
{
    pet_behavior_handle_t h = calloc(1, sizeof(struct pet_behavior_t));
    if (!h) return NULL;

    if (cfg) {
        h->cfg = *cfg;
    } else {
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
    if (!h) return;

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

    const bool rest_like =
        fabsf(acc_mean - 1.0f) < 0.08f &&
        acc_std < c->rest_acc_std_th &&
        gyro_mean < c->rest_gyro_mean_th;

    if (rest_like) {
        if (!h->rest_like_active) {
            h->rest_like_active = true;
            h->rest_like_since_ms = now_ms;
        }
    } else {
        h->rest_like_active = false;
        h->rest_like_since_ms = 0;
    }

    uint32_t rest_ms = (h->rest_like_active && h->rest_like_since_ms > 0) ?
                       now_ms - h->rest_like_since_ms : 0;

    if (rest_like && rest_ms >= c->not_worn_after_rest_ms) {
        return PET_STATE_NOT_WORN;
    }
    if (rest_like && rest_ms >= c->sleep_after_rest_ms) {
        return PET_STATE_SLEEP;
    }
    if (rest_like) {
        return PET_STATE_REST;
    }

    // 被动运动：有轻微晃动但没有明显步态。后续可结合 GPS/速度/充电/佩戴检测修正。
    if (acc_std < c->walk_acc_std_th && gyro_mean >= c->rest_gyro_mean_th && gyro_std < 10.0f) {
        return PET_STATE_PASSIVE_MOTION;
    }

    // 玩耍：旋转/摆头/方向变化较多，通常比跑步更无规律。
    if (gyro_std >= c->play_gyro_std_th || gyro_mean > 120.0f) {
        return PET_STATE_PLAY;
    }
    if (acc_std >= c->run_acc_std_th) {
        return PET_STATE_RUN;
    }
    if (acc_std >= c->trot_acc_std_th) {
        return PET_STATE_TROT;
    }
    if (acc_std >= c->walk_acc_std_th) {
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

    if (acc_norm >= c->impact_acc_norm_th) {
        ev |= PET_EVENT_IMPACT;
    }

    // Jump: 先出现低 g，再出现落地高 g。
    if (acc_norm < c->jump_low_g_th) {
        if (!h->low_g_active) {
            h->low_g_active = true;
            h->low_g_since_ms = now_ms;
        }
    }

    if (h->low_g_active) {
        uint32_t low_g_ms = now_ms - h->low_g_since_ms;
        if (acc_norm > c->jump_land_g_th && low_g_ms >= 40 && low_g_ms <= 600) {
            ev |= PET_EVENT_JUMP;
            h->low_g_active = false;
        } else if (low_g_ms > 800) {
            h->low_g_active = false;
        }
    }

    // 甩头/抖毛：瞬时角速度很大。
    if (gyro_norm > c->shake_gyro_th) {
        ev |= PET_EVENT_SHAKE;
    }

    // 抓挠：第一版用短窗口方差近似，后续可以升级为频域/峰间距检测。
    if (gyro_std >= c->scratch_gyro_std_min &&
        acc_std >= c->scratch_acc_std_min &&
        gyro_norm < c->shake_gyro_th) {
        ev |= PET_EVENT_SCRATCH;
    }

    // 翻滚：姿态变化大且角速度明显。
    float pitch = atan2f(-s->ax_g, sqrtf(s->ay_g * s->ay_g + s->az_g * s->az_g)) * 180.0f / (float)M_PI;
    float roll = atan2f(s->ay_g, s->az_g) * 180.0f / (float)M_PI;
    float d_angle = fabsf(pitch - h->last_pitch) + fabsf(roll - h->last_roll);
    if (d_angle > 80.0f && gyro_norm > 80.0f) {
        ev |= PET_EVENT_ROLL_OVER;
    }
    h->last_pitch = pitch;
    h->last_roll = roll;

    return ev;
}

static void apply_state_debounce(pet_behavior_handle_t h, pet_state_t candidate, uint32_t now_ms)
{
    if (h->state == PET_STATE_UNKNOWN) {
        h->state = candidate;
        h->state_since_ms = now_ms;
        h->candidate = candidate;
        h->candidate_since_ms = now_ms;
        return;
    }

    if (candidate == h->state) {
        h->candidate = candidate;
        h->candidate_since_ms = now_ms;
        return;
    }

    if (candidate != h->candidate) {
        h->candidate = candidate;
        h->candidate_since_ms = now_ms;
        return;
    }

    if ((now_ms - h->candidate_since_ms) >= h->cfg.min_state_hold_ms) {
        h->state = candidate;
        h->state_since_ms = now_ms;
    }
}

bool pet_behavior_update(pet_behavior_handle_t h,
                         const qmi8658a_sample_t *sample,
                         uint32_t now_ms,
                         pet_behavior_result_t *out)
{
    if (!h || !sample || !out) {
        return false;
    }

    if (h->win_start_ms == 0) {
        reset_windows(h, now_ms);
        h->state_since_ms = now_ms;
        h->candidate_since_ms = now_ms;
    }

    float acc_norm = vec_norm3(sample->ax_g, sample->ay_g, sample->az_g);
    float gyro_norm = vec_norm3(sample->gx_dps, sample->gy_dps, sample->gz_dps);

    stat_push(&h->acc_norm_win, acc_norm);
    stat_push(&h->gyro_norm_win, gyro_norm);

    bool window_ready = (now_ms - h->win_start_ms) >= h->cfg.window_ms;
    float acc_mean = h->last_result.acc_norm_mean;
    float acc_std = h->last_result.acc_norm_std;
    float gyro_mean = h->last_result.gyro_norm_mean;
    float gyro_std = h->last_result.gyro_norm_std;

    pet_state_t candidate = h->candidate;
    if (window_ready) {
        acc_mean = stat_mean(&h->acc_norm_win);
        acc_std = stat_std(&h->acc_norm_win);
        gyro_mean = stat_mean(&h->gyro_norm_win);
        gyro_std = stat_std(&h->gyro_norm_win);

        candidate = classify_from_window(h, acc_mean, acc_std, gyro_mean, gyro_std, now_ms);
        apply_state_debounce(h, candidate, now_ms);
        reset_windows(h, now_ms);
    }

    uint32_t events = detect_events(h, sample, acc_norm, gyro_norm, gyro_std, acc_std, now_ms);

    float pitch = atan2f(-sample->ax_g, sqrtf(sample->ay_g * sample->ay_g + sample->az_g * sample->az_g)) * 180.0f / (float)M_PI;
    float roll = atan2f(sample->ay_g, sample->az_g) * 180.0f / (float)M_PI;

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
        .rest_like_duration_ms = (h->rest_like_active && h->rest_like_since_ms > 0) ? now_ms - h->rest_like_since_ms : 0,
        .sample_count = h->last_result.sample_count + 1,
    };

    h->last_result = r;
    *out = r;

    return window_ready || events != PET_EVENT_NONE;
}

const char *pet_state_to_str(pet_state_t state)
{
    switch (state) {
    case PET_STATE_UNKNOWN: return "UNKNOWN";
    case PET_STATE_NOT_WORN: return "NOT_WORN";
    case PET_STATE_SLEEP: return "SLEEP";
    case PET_STATE_REST: return "REST";
    case PET_STATE_WALK: return "WALK";
    case PET_STATE_TROT: return "TROT";
    case PET_STATE_RUN: return "RUN";
    case PET_STATE_PLAY: return "PLAY";
    case PET_STATE_PASSIVE_MOTION: return "PASSIVE_MOTION";
    case PET_STATE_ABNORMAL_INACTIVE: return "ABNORMAL_INACTIVE";
    default: return "INVALID";
    }
}

void pet_events_to_str(uint32_t events, char *buf, uint32_t buf_len)
{
    if (!buf || buf_len == 0) return;

    buf[0] = '\0';
    if (events == PET_EVENT_NONE) {
        snprintf(buf, buf_len, "NONE");
        return;
    }

    bool first = true;
    struct {
        uint32_t bit;
        const char *name;
    } map[] = {
        {PET_EVENT_SHAKE, "SHAKE"},
        {PET_EVENT_SCRATCH, "SCRATCH"},
        {PET_EVENT_JUMP, "JUMP"},
        {PET_EVENT_IMPACT, "IMPACT"},
        {PET_EVENT_ROLL_OVER, "ROLL_OVER"},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (events & map[i].bit) {
            size_t used = strlen(buf);
            snprintf(buf + used, buf_len > used ? buf_len - used : 0,
                     "%s%s", first ? "" : "|", map[i].name);
            first = false;
        }
    }
}
