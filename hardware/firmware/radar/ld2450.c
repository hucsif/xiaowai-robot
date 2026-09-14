/*
 * ld2450.c — 海凌科 LD2450 24GHz 运动目标追踪雷达 UART 帧解析实现
 *
 * 帧格式（固定30字节，小端序，来源：LD2450 串口通信协议 V1.03）：
 *
 *   SOF(4B) AA FF 03 00 | T1(8B) T2(8B) T3(8B) | EOF(2B) 55 CC
 *
 * 无需配置命令，上电后每秒自动上报 10 帧。
 */

#include "ld2450.h"
#include <string.h>

/* 读 LE uint16 */
static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* 解析 LD2450 有符号坐标：bit15=1正 0负，去符号位 */
static int16_t decode_coord(uint16_t raw) {
    if (raw & 0x8000) return (int16_t)(raw & 0x7FFF);
    else              return -(int16_t)(raw & 0x7FFF);
}

static void parse_target(const uint8_t *d, ld2450_target_t *t) {
    uint16_t rx = rd16(d);
    uint16_t ry = rd16(d + 2);
    uint16_t rs = rd16(d + 4);
    uint16_t rr = rd16(d + 6);

    /* 全零 = 无目标 */
    if (rx == 0 && ry == 0 && rs == 0 && rr == 0) {
        memset(t, 0, sizeof(*t));
        return;
    }
    t->valid         = true;
    t->x_mm          = decode_coord(rx);
    t->y_mm          = decode_coord(ry);
    t->speed_cms     = decode_coord(rs);
    t->resolution_mm = rr;
}

void ld2450_init(ld2450_parser_t *p, ld2450_frame_cb cb, void *user) {
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->cb_user = user;
}

void ld2450_feed(ld2450_parser_t *p, uint8_t b, uint32_t now_ms) {
    /* 无状态机——帧长固定，只需对齐 SOF 后读满 30 字节 */
    p->buf[p->pos++] = b;

    if (p->pos == 1) {
        if (b != LD2450_SOF1) { p->pos = 0; return; }
    } else if (p->pos == 2) {
        if (b != LD2450_SOF2) { p->pos = (b == LD2450_SOF1) ? 1 : 0; return; }
    } else if (p->pos == 3) {
        if (b != LD2450_SOF3) { p->pos = (b == LD2450_SOF1) ? 1 : 0; return; }
    } else if (p->pos == 4) {
        if (b != LD2450_SOF4) { p->pos = (b == LD2450_SOF1) ? 1 : 0; return; }
    } else if (p->pos == LD2450_FRAME_LEN) {
        /* 帧尾校验 */
        if (p->buf[LD2450_FRAME_LEN - 2] == LD2450_EOF1 &&
            p->buf[LD2450_FRAME_LEN - 1] == LD2450_EOF2) {
            p->frames_ok++;

            ld2450_status_t *s = &p->status;
            int count = 0;
            for (int i = 0; i < LD2450_TARGET_COUNT; i++) {
                parse_target(&p->buf[4 + i * 8], &s->targets[i]);
                if (s->targets[i].valid) count++;
            }
            s->last_update_ms = now_ms;

            if (p->cb) p->cb(s->targets, count, p->cb_user);
        } else {
            p->frames_bad++;
        }
        p->pos = 0;
    }
}
