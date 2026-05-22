#include "pet_behavior_internal.h"

void push_history(pet_behavior_handle_t h, const behavior_window_t *w)
{
    h->history[h->history_pos] = *w;
    h->history_pos = (uint8_t)((h->history_pos + 1) % h->cfg.history_window_count);
    if (h->history_count < h->cfg.history_window_count)
        h->history_count++;
}

history_summary_t summarize_history(pet_behavior_handle_t h)
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
        s.avg_acc_std += w->acc_std;
        s.avg_gyro_mean += w->gyro_mean;
        s.avg_gyro_std += w->gyro_std;
        s.avg_acc_delta += w->acc_delta_mean;
        s.avg_gyro_delta += w->gyro_delta_mean;
        s.avg_acc_range += w->acc_range;
        s.avg_gyro_range += w->gyro_range;
        s.avg_posture_std += w->posture_std;
        if (w->ultra_static)
            s.ultra_static++;

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
        s.avg_acc_std *= inv;
        s.avg_gyro_mean *= inv;
        s.avg_gyro_std *= inv;
        s.avg_acc_delta *= inv;
        s.avg_gyro_delta *= inv;
        s.avg_acc_range *= inv;
        s.avg_gyro_range *= inv;
        s.avg_posture_std *= inv;
    }

    return s;
}
