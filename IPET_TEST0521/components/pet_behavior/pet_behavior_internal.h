#pragma once

#include "pet_behavior.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    bool ultra_static;
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
    uint8_t ultra_static;
    uint8_t valid;

    float avg_activity;
    float avg_rest;
    float avg_rhythm;
    float avg_irregular;
    float avg_burst;
    float avg_run;
    float avg_play;

    float avg_acc_std;
    float avg_gyro_mean;
    float avg_gyro_std;
    float avg_acc_delta;
    float avg_gyro_delta;
    float avg_acc_range;
    float avg_gyro_range;
    float avg_posture_std;
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

    uint32_t ultra_static_since_ms;
    bool ultra_static_active;

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

/* math/stat helpers */
float clampf_local(float v, float lo, float hi);
float score_up(float v, float lo, float hi);
float score_down(float v, float lo, float hi);
float min3f(float a, float b, float c);
float vec_norm3(float x, float y, float z);
float angle_diff_deg(float a, float b);
void stat_reset(stat_win_t *s);
void stat_push(stat_win_t *s, float v);
float stat_mean(const stat_win_t *s);
float stat_std(const stat_win_t *s);
float stat_range(const stat_win_t *s);

/* window/feature helpers */
bool is_ultra_static_window(const behavior_window_t *w, const pet_behavior_config_t *c);
void reset_windows(pet_behavior_handle_t h, uint32_t now_ms);
behavior_window_t build_window_feature(pet_behavior_handle_t h, uint32_t now_ms);

/* history helpers */
void push_history(pet_behavior_handle_t h, const behavior_window_t *w);
history_summary_t summarize_history(pet_behavior_handle_t h);

/* state machine helpers */
void update_rest_timer(pet_behavior_handle_t h, bool rest_like, uint32_t now_ms);
void update_ultra_static_timer(pet_behavior_handle_t h, bool ultra_static_like, uint32_t now_ms);
pet_state_t decide_raw_from_history(pet_behavior_handle_t h, const history_summary_t *s, uint32_t now_ms, float *confidence_out);
uint32_t transition_required_ms(pet_state_t current, pet_state_t target);
pet_state_t apply_raw_state_machine(pet_behavior_handle_t h, pet_state_t target, uint32_t now_ms);
pet_state_t map_raw_to_display(pet_behavior_handle_t h, pet_state_t raw);
void update_display_state(pet_behavior_handle_t h, pet_state_t display, uint32_t now_ms);

/* events */
uint32_t detect_events_from_window(pet_behavior_handle_t h, const qmi8658a_sample_t *last_sample, float acc_norm, float gyro_norm, const behavior_window_t *w, uint32_t now_ms);
