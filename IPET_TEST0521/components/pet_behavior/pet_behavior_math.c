#include "pet_behavior_internal.h"

float clampf_local(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

float score_up(float v, float lo, float hi)
{
    if (hi <= lo)
        return 0.0f;
    return clampf_local((v - lo) * 100.0f / (hi - lo), 0.0f, 100.0f);
}

float score_down(float v, float lo, float hi)
{
    if (hi <= lo)
        return 0.0f;
    return clampf_local((hi - v) * 100.0f / (hi - lo), 0.0f, 100.0f);
}

float min3f(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

float vec_norm3(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

float angle_diff_deg(float a, float b)
{
    float d = a - b;
    while (d > 180.0f)
        d -= 360.0f;
    while (d < -180.0f)
        d += 360.0f;
    return fabsf(d);
}

void stat_reset(stat_win_t *s)
{
    s->sum = 0.0f;
    s->sum2 = 0.0f;
    s->max = -1e9f;
    s->min = 1e9f;
    s->n = 0;
}

void stat_push(stat_win_t *s, float v)
{
    s->sum += v;
    s->sum2 += v * v;
    if (v > s->max)
        s->max = v;
    if (v < s->min)
        s->min = v;
    s->n++;
}

float stat_mean(const stat_win_t *s)
{
    return s->n ? s->sum / s->n : 0.0f;
}

float stat_std(const stat_win_t *s)
{
    if (!s->n)
        return 0.0f;

    float mean = stat_mean(s);
    float var = s->sum2 / s->n - mean * mean;
    if (var < 0.0f)
        var = 0.0f;

    return sqrtf(var);
}

float stat_range(const stat_win_t *s)
{
    return s->n ? (s->max - s->min) : 0.0f;
}
