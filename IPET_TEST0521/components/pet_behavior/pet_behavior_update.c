#include "pet_behavior_internal.h"

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
            .run_score = w.run_score,
            .play_score = w.play_score,
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
