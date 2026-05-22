#include "pet_behavior_internal.h"

void update_rest_timer(pet_behavior_handle_t h, bool rest_like, uint32_t now_ms)
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

void update_ultra_static_timer(pet_behavior_handle_t h, bool ultra_static_like, uint32_t now_ms)
{
    if (ultra_static_like)
    {
        if (!h->ultra_static_active)
        {
            h->ultra_static_active = true;
            h->ultra_static_since_ms = now_ms;
        }
    }
    else
    {
        h->ultra_static_active = false;
        h->ultra_static_since_ms = 0;
    }
}

pet_state_t decide_raw_from_history(pet_behavior_handle_t h,
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
                      s->avg_run >= 56.0f &&
                      s->avg_activity >= 54.0f &&
                      s->avg_rhythm >= 28.0f &&
                      s->avg_irregular < 75.0f;

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

    /*
     * v5 新增：先判断“超静止”。
     * 桌面/取下放置会连续多个窗口 ultra_static；睡觉也很安静，但通常会有呼吸、皮毛/项圈微动、姿态轻微变化。
     */
    bool ultra_static_like =
        raw == PET_STATE_REST &&
        s->valid >= 3 &&
        s->ultra_static >= (s->valid >= 5 ? 4 : s->valid) &&
        s->avg_activity < 5.0f &&
        s->avg_acc_std <= c->not_worn_acc_std_th &&
        s->avg_gyro_mean <= c->not_worn_gyro_mean_th &&
        s->avg_gyro_std <= c->not_worn_gyro_std_th &&
        s->avg_acc_range <= c->not_worn_acc_range_th &&
        s->avg_gyro_range <= c->not_worn_gyro_range_th;

    update_ultra_static_timer(h, ultra_static_like, now_ms);

    uint32_t rest_ms = (h->rest_like_active && h->rest_like_since_ms > 0)
                           ? now_ms - h->rest_like_since_ms
                           : 0;
    uint32_t ultra_static_ms = (h->ultra_static_active && h->ultra_static_since_ms > 0)
                                   ? now_ms - h->ultra_static_since_ms
                                   : 0;

    if (raw == PET_STATE_REST && ultra_static_ms >= c->not_worn_after_rest_ms)
    {
        raw = PET_STATE_NOT_WORN;
        conf = 100.0f;
    }
    else if (raw == PET_STATE_REST && rest_ms >= c->sleep_after_rest_ms)
    {
        raw = PET_STATE_SLEEP;
        conf = ultra_static_like ? 80.0f : 92.0f;
    }

    if (confidence_out)
        *confidence_out = conf;

    return raw;
}

uint32_t transition_required_ms(pet_state_t current, pet_state_t target)
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
    if (target == PET_STATE_NOT_WORN)
        return 8000;

    if (target == PET_STATE_REST || target == PET_STATE_SLEEP)
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

pet_state_t apply_raw_state_machine(pet_behavior_handle_t h,
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

pet_state_t map_raw_to_display(pet_behavior_handle_t h, pet_state_t raw)
{
    if (h->cfg.enable_fine_states)
        return raw;

    switch (raw)
    {
    case PET_STATE_RUN:
        /*
         * v6: 跑步先作为正式输出状态开放。
         * TROT/PLAY 仍默认合并到 WALK，等后续数据够了再逐步打开。
         */
        return h->cfg.enable_run_state ? PET_STATE_RUN : PET_STATE_WALK;

    case PET_STATE_TROT:
    case PET_STATE_PLAY:
        return PET_STATE_WALK;

    case PET_STATE_PASSIVE_MOTION:
        return h->cfg.passive_motion_as_rest ? PET_STATE_REST : PET_STATE_PASSIVE_MOTION;

    default:
        return raw;
    }
}

void update_display_state(pet_behavior_handle_t h, pet_state_t display, uint32_t now_ms)
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
