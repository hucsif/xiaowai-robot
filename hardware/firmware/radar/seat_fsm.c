/*
 * seat_fsm.c — 入座/离座检测状态机实现（纯 C）
 *
 * 判据与去抖逻辑从 caterpillar 项目的 radar_fsm.c:116-138（tick_day）搬来，
 * 去掉了主状态机的 set_state()/EV_STATE_CHANGED（那是给睡眠链路用的）。
 */
#include "seat_fsm.h"
#include <string.h>

/* 无符号回绕安全的差值（上游 radar_fsm.c:5 同款；millis() 同语义） */
#define ELAPSED(now, since) ((uint32_t)((now) - (since)))

/* 「有人」的体动兜底阈值（上游 tick_day 里的字面量 5） */
#define SEAT_BODY_MOVE_MIN 5

void seat_default_config(seat_config_t *c)
{
    c->seat_debounce_ms = 5000;
}

void seat_init(seat_fsm_t *f, const seat_config_t *cfg,
               seat_event_cb cb, void *user)
{
    memset(f, 0, sizeof(*f));
    if (cfg) f->cfg = *cfg;
    else     seat_default_config(&f->cfg);
    f->cb = cb;
    f->cb_user = user;
    /* seated 初值 false（未入座）；若一开始就有人，会在 debounce 后发出 EV_SEATED */
}

void seat_tick(seat_fsm_t *f, uint8_t body_move, bool presence, uint32_t now)
{
    const seat_config_t *c = &f->cfg;
    /* 上游原型注释：presence 在某些雷达固件中不工作，故用体动 > 5 兜底 */
    const bool seated_now = (body_move > SEAT_BODY_MOVE_MIN) || presence;

    if (seated_now != f->seated) {
        if (f->t_change == 0) {
            f->t_change = now; /* 变化起点，开始计时 */
        }
        if (ELAPSED(now, f->t_change) >= c->seat_debounce_ms) {
            f->seated = seated_now;
            f->t_change = 0;
            if (f->cb) {
                f->cb(seated_now ? SEAT_EV_SEATED : SEAT_EV_AWAY, (int)body_move, f->cb_user);
            }
        }
    } else {
        /* 状态回到一致：取消待确认（例如起身又立刻坐下） */
        f->t_change = 0;
    }
}
