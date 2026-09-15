/*
 * squat_fsm.h — 蹲下检测状态机（纯 C，事件驱动）
 *
 * 从 caterpillar 项目的 radar_fsm.{h,c} 剥离而来：上游把「蹲下检测」和
 * 「睡眠/闹钟主状态机」放在同一对文件里，但两者代码零交叉（不共享静态
 * 函数或变量，互不引用），这里独立出来。判据逻辑与上游一致，未做修改。
 *
 * 判据（三重与）：
 *   1. body_move >= squat_move_min(40) 连续 squat_high_count_min(2) 次
 *      —— body_move 是 1Hz 上报且一旦不达标就清零，故等价于「连续 ≥2 秒体动 ≥40」
 *   2. pos_stable == true —— 由连续两帧坐标的 |dx|/|dy| < squat_xy_max_cm(15) 判定。
 *      语义是「原地动」：体动大但位置几乎没变 → 蹲下；位置大幅变化 → 走开，不算
 *   3. 三者同时满足 → 触发 SQUAT_EV_DETECTED
 *   结束：距最后一次超阈值 squat_hold_ms(3000) 后 → SQUAT_EV_ENDED
 *
 * ⚠️ 已知行为（与上游一致，未修）：SQUAT_EV_ENDED 只在 squat_on_body_move()
 *    里检测。若 R60 完全停止上报（拔线/静默），active 会一直保持 true、
 *    不再产出「结束」事件。
 *
 * ⚠️ 阈值（40 / 15cm / 2 / 3000ms）是在毛毛虫的机械结构上标定的，本板
 *    安装高度与角度不同，可能需要实测重标定。
 *
 * 用法：
 *   squat_init(&f, NULL, cb, user);              // NULL = 用默认配置
 *   收到 R60 帧时按 ctl/cmd 分发：
 *     ctl==0x80 && cmd==0x03  →  squat_on_body_move(&f, body_move, now)
 *     ctl==0x80 && cmd==0x05  →  squat_on_position(&f, x_cm, y_cm, now)
 *   随时 squat_is_active(&f) 查询当前状态
 */
#ifndef SQUAT_FSM_H
#define SQUAT_FSM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SQUAT_EV_DETECTED = 0, /* 蹲下确认 */
    SQUAT_EV_ENDED,        /* 蹲下结束 */
} squat_event_t;

typedef void (*squat_event_cb)(squat_event_t ev, int data, void *user);

typedef struct {
    uint8_t  squat_move_min;       /* body_move 阈值，默认 40 */
    uint16_t squat_xy_max_cm;      /* x/y 波动上限 cm，默认 15 */
    uint8_t  squat_high_count_min; /* 连续超阈值最少次数，默认 2 */
    uint32_t squat_hold_ms;        /* 保持时间 ms，默认 3000 */
} squat_config_t;

typedef struct {
    squat_config_t cfg;
    bool           active;
    uint8_t        high_count;
    uint32_t       last_high_ms;
    int16_t        pos_x_prev, pos_y_prev;
    bool           pos_stable;
    bool           pos_initialized;
    squat_event_cb cb;
    void          *cb_user;
} squat_fsm_t;

void squat_default_config(squat_config_t *c);
void squat_init(squat_fsm_t *f, const squat_config_t *cfg,
                squat_event_cb cb, void *user);
/** 喂体动幅度（R60 ctl=0x80 cmd=0x03，1Hz）。蹲下触发与结束都在这里判定。 */
void squat_on_body_move(squat_fsm_t *f, uint8_t body_move, uint32_t now);
/** 喂 2D 坐标（**单位必须是 cm**）。仅维护 pos_stable。
 *  本板的坐标来自 LD2450（10Hz）——LD2450 原始单位是 mm，调用方需先 /10。 */
void squat_on_position(squat_fsm_t *f, int16_t x_cm, int16_t y_cm, uint32_t now);
static inline bool squat_is_active(const squat_fsm_t *f) { return f->active; }

#ifdef __cplusplus
}
#endif
#endif /* SQUAT_FSM_H */
