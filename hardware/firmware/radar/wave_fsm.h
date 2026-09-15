/*
 * wave_fsm.h — LD2450 挥手检测状态机（事件驱动，仿 radar_fsm 风格）
 *
 * 从 caterpillar 项目的 components/caterpillar_radar/ 原样拷贝，逻辑未改动。
 *
 * 用法：
 *   1. wave_init() 初始化
 *   2. LD2450 每帧到达时调用 wave_on_frame()
 *   3. 任何时候 wave_is_active() 查询当前状态
 *
 * 不使用滑动窗口，仅维护事件计数 + 时间戳。
 *
 * ⚠️ 判据里 x 的过零用的是「原始 mm 值跨零点」（wave_fsm.c:54），
 *    意味着人需要大致正对雷达轴线才灵敏；偏轴摆放时可能漏检。
 *    若实测不灵敏，可参考上游已废弃的 gesture_detect.c —— 那边用的是
 *    「相对窗口均值的中心线穿越」，对偏轴更鲁棒。
 */
#ifndef WAVE_FSM_H
#define WAVE_FSM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAVE_EV_DETECTED = 0,
    WAVE_EV_ENDED,
} wave_event_t;

typedef void (*wave_event_cb)(wave_event_t ev, int data, void *user);

typedef struct {
    uint8_t  event_min;         /* 时间窗内最少摆动事件数，默认 8（10Hz 下约 0.8s 持续摆动） */
    uint32_t hold_ms;           /* 最后一次事件后保持 ms，默认 3000 */
    int16_t  speed_threshold;   /* speed 有效阈值，默认 8 cm/s */
} wave_config_t;

typedef struct {
    wave_config_t cfg;
    bool          active;
    uint8_t       event_count;
    uint32_t      last_event_ms;
    int16_t       last_speed;
    int16_t       last_x;
    bool          first_frame;
    wave_event_cb cb;
    void         *cb_user;
} wave_fsm_t;

void wave_default_config(wave_config_t *c);
void wave_init(wave_fsm_t *f, const wave_config_t *cfg,
               wave_event_cb cb, void *user);
void wave_on_frame(wave_fsm_t *f, bool valid,
                   int16_t x_mm, int16_t y_mm, int16_t speed_cms,
                   uint32_t now);
static inline bool wave_is_active(const wave_fsm_t *f) { return f->active; }

#ifdef __cplusplus
}
#endif
#endif /* WAVE_FSM_H */
