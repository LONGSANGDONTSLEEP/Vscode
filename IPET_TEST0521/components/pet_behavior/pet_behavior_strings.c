#include "pet_behavior_internal.h"

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
