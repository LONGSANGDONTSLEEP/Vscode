#include "pet_behavior_internal.h"

uint32_t detect_events_from_window(pet_behavior_handle_t h,
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
