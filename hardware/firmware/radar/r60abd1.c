/* 
    r60abd1.c — R60ABD1 UART 帧解析实现（纯 C，无平台依赖） 
    将雷达模组通过UART上报的原始字节流解析为结构化数据
    帧格式定义如下：
    固定帧头            控制字        命令字         数据长度           数据      校验和          固定结尾 
    SOF(2B) 0x53 0x59 | control(1B) | command(1B) | length(2B, 大端) | data(N) | checksum(1B) | EOF(2B) 0x54 0x43
    其中Control也称为大类，command称为小类
*/
#include "r60abd1.h"
#include <string.h>

// 分别对应帧结构固定的两字节的开头和结尾
#define SOF1 0x53   
#define SOF2 0x59
#define EOF1 0x54
#define EOF2 0x43

/* 帧内偏移（相对 buf[0]=SOF1）*/
#define OFF_CTL   2
#define OFF_CMD   3
#define OFF_LENH  4
#define OFF_LENL  5
#define OFF_DATA  6

// 初始化解析器并注册回调函数
void r60_init(r60_parser_t *p, r60_frame_cb cb, void *user)
{
    memset(p, 0, sizeof(*p));   // 将解析器实例结构体成员全部清零
    p->cb = cb;             // 帧回调函数
    p->cb_user = user;      // 用户指针
}

// 内联函数，返回雷达状态快照结构体指针
// 根据 ctl 大类进入不同分支，再根据 cmd 子类将 data[] 解析为具体状态字段
static void apply_frame(r60_parser_t *p, uint32_t now_ms)
{
    uint8_t ctl = p->buf[OFF_CTL];
    uint8_t cmd = p->buf[OFF_CMD];
    const uint8_t *d = &p->buf[OFF_DATA];       // 获取帧数据指针
    uint16_t len = p->expect_len;
    r60_status_t *s = &p->status;               // 获取雷达快照结构体指针

    if (ctl == R60_CTL_PRESENCE) {
        if (cmd == R60_CMD_EXIST && len >= 1)      s->presence  = (d[0] != 0);
        else if (cmd == R60_CMD_MOTION && len >= 1) s->motion    = d[0];
        else if (cmd == R60_CMD_BODYMOVE && len >= 1) s->body_move = d[0];
#if 0  /* distance 已弃用——改用 DP5 x/y */
        else if (cmd == R60_CMD_DISTANCE && len >= 2)
            s->distance_cm = ((uint16_t)d[0] << 8) | d[1];
#endif
        /* DP5 x/y 已停用——位置改用 LD2450 的 10Hz 坐标（见 radar.cpp 的
         * on_ld2450_frame）。R60 的 DP5 上报频率远低于 LD2450，本板的蹲下
         * 检测需要更平滑的位置数据。停用后 s->x_cm / s->y_cm 恒为 0；
         * 保留代码以备回退（与上面 distance_cm 同样的处理方式）。 */
#if 0
        else if (cmd == R60_CMD_POSITION && len >= 4) {
            uint16_t raw_x = ((uint16_t)d[0] << 8) | d[1];
            uint16_t raw_y = ((uint16_t)d[2] << 8) | d[3];
            s->x_cm = (raw_x & 0x8000) ? -((int16_t)(raw_x & 0x7FFF)) : (int16_t)(raw_x & 0x7FFF);
            s->y_cm = (raw_y & 0x8000) ? -((int16_t)(raw_y & 0x7FFF)) : (int16_t)(raw_y & 0x7FFF);
        }
#endif
    } else if (ctl == R60_CTL_BREATH) {
        if (cmd == R60_CMD_BREATH_VAL && len >= 1)  s->breath_rate = d[0];
    } else if (ctl == R60_CTL_SLEEP) {
        if (cmd == R60_CMD_BED && len >= 1)         s->in_bed      = (d[0] != 0);
        else if (cmd == R60_CMD_SLEEP_STG && len >= 1) s->sleep_stage = d[0];
    } else if (ctl == R60_CTL_HEART) {
        if (cmd == R60_CMD_HEART_VAL && len >= 1)   s->heart_rate  = d[0];
    }
    s->last_update_ms = now_ms;

    if (p->cb)
        p->cb(ctl, cmd, d, len, p->cb_user);
}

// 其中b是新接收到的单个字节，now_ms是宿主毫秒时钟，用于给status.last_update_ms打戳
void r60_feed(r60_parser_t *p, uint8_t b, uint32_t now_ms)
{
    switch (p->stage) {
    case 0: /* 等 SOF1 */
        if (b == SOF1) { p->buf[0] = b; p->pos = 1; p->stage = 1; }
        break;
    case 1: /* 等 SOF2 */
        if (b == SOF2) { p->buf[1] = b; p->pos = 2; p->stage = 2; }
        else           { p->stage = (b == SOF1) ? 1 : 0; p->pos = (b == SOF1) ? 1 : 0; }
        break;
    case 2: /* ctl/cmd/len(2) */
        p->buf[p->pos++] = b;
        if (p->pos == OFF_DATA) {
            p->expect_len = ((uint16_t)p->buf[OFF_LENH] << 8) | p->buf[OFF_LENL];
            if (p->expect_len > sizeof(p->buf) - OFF_DATA - 3) { /* 超长，丢帧 */
                p->frames_bad++;
                p->stage = 0;
            } else {
                p->stage = 3;
            }
        }
        break;
    case 3: /* data + checksum + EOF(2) */
        p->buf[p->pos++] = b;
        if (p->pos == OFF_DATA + p->expect_len + 3) {
            uint16_t ck_off  = OFF_DATA + p->expect_len;
            uint8_t  sum     = 0;
            for (uint16_t i = 0; i < ck_off; i++)
                sum = (uint8_t)(sum + p->buf[i]);
            bool ok = (p->buf[ck_off] == sum) &&
                      (p->buf[ck_off + 1] == EOF1) &&
                      (p->buf[ck_off + 2] == EOF2);
            if (ok) { p->frames_ok++;  apply_frame(p, now_ms); }
            else    { p->frames_bad++; }
            p->stage = 0;
            p->pos = 0;
        }
        break;
    default:
        p->stage = 0;
        p->pos = 0;
        break;
    }
}
