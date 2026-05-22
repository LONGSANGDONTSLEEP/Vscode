idf_component_register(
    SRCS
        "pet_behavior.c"
        "pet_behavior_math.c"
        "pet_behavior_config.c"
        "pet_behavior_features.c"
        "pet_behavior_history.c"
        "pet_behavior_state.c"
        "pet_behavior_events.c"
        "pet_behavior_update.c"
        "pet_behavior_strings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES qmi8658a freertos log
)
