/*
 * wave_fsm.c — LD2450 挥手检测状态机实现（事件驱动）
 *
 * 每帧检测 speed 方向翻转和 x 过零，满足条件即确认挥手。
 * 不存滑动窗口，不回溯统计，仅维护事件计数 + 时间戳。
 */
#include "wave_fsm.h"
#include <string.h>

#define ELAPSED(now, since) ((uint32_t)((now) - (since)))

void wave_default_config(wave_config_t *c)
{
    c->event_min       = 8;
    c->hold_ms         = 3000;
    c->speed_threshold = 8;
}

void wave_init(wave_fsm_t *f, const wave_config_t *cfg,
               wave_event_cb cb, void *user)
{
    memset(f, 0, sizeof(*f));
    if (cfg) f->cfg = *cfg;
    else     wave_default_config(&f->cfg);
    f->cb = cb;
    f->cb_user = user;
    f->first_frame = true;
}

void wave_on_frame(wave_fsm_t *f, bool valid,
                   int16_t x_mm, int16_t y_mm, int16_t speed_cms,
                   uint32_t now)
{
    const wave_config_t *c = &f->cfg;

    if (!valid) {
        f->first_frame = true;
        return;
    }

    bool event = false;

    if (!f->first_frame) {
        int16_t sp_prev = f->last_speed;
        int16_t sp_curr = speed_cms;
        int16_t abs_prev = (sp_prev < 0) ? -sp_prev : sp_prev;
        int16_t abs_curr = (sp_curr < 0) ? -sp_curr : sp_curr;

        if (abs_prev >= c->speed_threshold && abs_curr >= c->speed_threshold &&
            ((sp_prev > 0 && sp_curr < 0) || (sp_prev < 0 && sp_curr > 0))) {
            event = true;
        }

        if ((f->last_x > 0 && x_mm < 0) || (f->last_x < 0 && x_mm > 0)) {
            event = true;
        }
    }

    f->first_frame = false;
    f->last_speed = speed_cms;
    f->last_x = x_mm;

    if (event) {
        f->event_count++;
        f->last_event_ms = now;

        if (!f->active && f->event_count >= c->event_min) {
            f->active = true;
            if (f->cb) f->cb(WAVE_EV_DETECTED, (int)f->event_count, f->cb_user);
        }
    }

    if (f->active && ELAPSED(now, f->last_event_ms) >= c->hold_ms) {
        f->active = false;
        f->event_count = 0;
        if (f->cb) f->cb(WAVE_EV_ENDED, 0, f->cb_user);
    }

    if (!f->active && ELAPSED(now, f->last_event_ms) >= 2000) {
        f->event_count = 0;
    }
}
