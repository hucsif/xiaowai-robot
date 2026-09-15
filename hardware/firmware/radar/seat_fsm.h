/*
 * seat_fsm.h — 入座/离座检测状态机（纯 C，tick 驱动）
 *
 * 从 caterpillar 项目的 radar_fsm.{h,c} 的 tick_day() 剥离而来。上游把这段
 * 和睡眠/闹钟主状态机耦合在同一个 radar_fsm_t 里，这里独立成模块。
 * 判据与去抖逻辑与上游一致，未做修改。
 *
 * 判据：
 *   seated_now = (body_move > 5) || presence
 *     上游原型注释：presence 在某些雷达固件中不工作，故用「体动 > 5」兜底。
 *     本板的 R60 presence 工作正常（日志可见 presence=0/1 跳变），
 *     两者取或，哪个可靠都能用上。
 *
 *   状态变化后需持续 seat_debounce_ms(5000) 才确认，防止路过/起身拿东西误判：
 *     离座 → 入座 持续 5s  → SEAT_EV_SEATED
 *     入座 → 离座 持续 5s  → SEAT_EV_AWAY
 *
 * 与 squat_fsm 的差异：squat 是事件驱动（每帧喂），本模块是 tick 驱动，
 * 需要宿主周期性调用 seat_tick()（上游用 200ms，与 fsm_tick 同频）。
 *
 * 用法：
 *   seat_init(&f, NULL, cb, user);              // NULL = 默认配置
 *   每 200ms： seat_tick(&f, body_move, presence, now);
 *   随时：     seat_is_seated(&f)
 */
#ifndef SEAT_FSM_H
#define SEAT_FSM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SEAT_EV_SEATED = 0, /* 入座确认 */
    SEAT_EV_AWAY,       /* 离座确认 */
} seat_event_t;

typedef void (*seat_event_cb)(seat_event_t ev, int data, void *user);

typedef struct {
    uint32_t seat_debounce_ms; /* 去抖时长，默认 5000 */
} seat_config_t;

typedef struct {
    seat_config_t cfg;
    bool          seated;    /* 当前确认状态；初值 false = 未入座 */
    uint32_t      t_change;  /* 待确认的状态变化起点；0 = 无待确认 */
    seat_event_cb cb;
    void         *cb_user;
} seat_fsm_t;

void seat_default_config(seat_config_t *c);
void seat_init(seat_fsm_t *f, const seat_config_t *cfg,
               seat_event_cb cb, void *user);
/** 周期喂当前状态快照（建议 200ms）。body_move/presence 直接取 r60_status_t 的同名字段。 */
void seat_tick(seat_fsm_t *f, uint8_t body_move, bool presence, uint32_t now);
static inline bool seat_is_seated(const seat_fsm_t *f) { return f->seated; }

#ifdef __cplusplus
}
#endif
#endif /* SEAT_FSM_H */
