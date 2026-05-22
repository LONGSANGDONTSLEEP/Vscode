#include "pet_behavior.h"

/*
 * pet_behavior 已拆分为多个小文件：
 * - pet_behavior_config.c   默认配置、生命周期
 * - pet_behavior_update.c   主 update 入口
 * - pet_behavior_features.c 1 秒窗口特征和候选状态
 * - pet_behavior_history.c  多窗口历史汇总
 * - pet_behavior_state.c    状态机与显示映射
 * - pet_behavior_events.c   事件检测
 * - pet_behavior_math.c     数学/统计工具
 * - pet_behavior_strings.c  字符串转换
 *
 * 对外 API 仍然在 include/pet_behavior.h 中，调用方不用改。
 */
