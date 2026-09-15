/*
 * squat_fsm.c — 蹲下检测状态机实现（纯 C）
 *
 * 判据逻辑从 caterpillar 项目的 radar_fsm.c:162-227 原样搬来，
 * 仅把事件枚举 EV_SQUAT/EV_SQUAT_END 换成自带的 SQUAT_EV_*。
 */
#include "squat_fsm.h"
#include <string.h>

/* 无符号回绕安全的差值（上游 radar_fsm.c:5 同款；millis() 同语义） */
#define ELAPSED(now, since) ((uint32_t)((now) - (since)))

void squat_default_config(squat_config_t *c)
{
    c->squat_move_min       = 40;
    c->squat_xy_max_cm      = 15;
    c->squat_high_count_min = 2;
    c->squat_hold_ms        = 3000;
}

void squat_init(squat_fsm_t *f, const squat_config_t *cfg,
                squat_event_cb cb, void *user)
{
    memset(f, 0, sizeof(*f));
    if (cfg) f->cfg = *cfg;
    else     squat_default_config(&f->cfg);
    f->cb = cb;
    f->cb_user = user;
}

void squat_on_body_move(squat_fsm_t *f, uint8_t body_move, uint32_t now)
{
    const squat_config_t *c = &f->cfg;

    if (body_move >= c->squat_move_min) {
        f->high_count++;
        f->last_high_ms = now;

        if (!f->active && f->high_count >= c->squat_high_count_min && f->pos_stable) {
            f->active = true;
            if (f->cb) f->cb(SQUAT_EV_DETECTED, (int)body_move, f->cb_user);
        }
    } else {
        f->high_count = 0;
    }

    if (f->active && ELAPSED(now, f->last_high_ms) >= c->squat_hold_ms) {
        f->active = false;
        if (f->cb) f->cb(SQUAT_EV_ENDED, 0, f->cb_user);
    }
}

void squat_on_position(squat_fsm_t *f, int16_t x_cm, int16_t y_cm, uint32_t now)
{
    const squat_config_t *c = &f->cfg;
    (void)now;

    if (!f->pos_initialized) {
        /* 首帧只做基准，不判稳定——否则初值 (0,0) 会产生一次假跳变 */
        f->pos_x_prev = x_cm;
        f->pos_y_prev = y_cm;
        f->pos_initialized = true;
        f->pos_stable = true;
        return;
    }

    int16_t dx = x_cm - f->pos_x_prev;
    int16_t dy = y_cm - f->pos_y_prev;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    f->pos_stable = (dx < (int16_t)c->squat_xy_max_cm &&
                     dy < (int16_t)c->squat_xy_max_cm);
    f->pos_x_prev = x_cm;
    f->pos_y_prev = y_cm;
}
