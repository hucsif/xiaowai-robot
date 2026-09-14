/*
 * r60abd1.h — 云帆瑞达 R60ABD1 60GHz 呼吸/睡眠雷达 UART 协议解析器（原型）
 *
 * 纯 C、无平台依赖：宿主逐字节喂入 r60abd1_feed()，解析出完整帧后经回调抛出。
 * 帧格式（MicRadar 通用协议，来源：厂商《R60ABD1 串口通信协议》及
 * github.com/tsunglung/esphome-micradar 实现；量产前须对照官方协议文档逐条核对）：
 *
 *   SOF(2B) 0x53 0x59 | control(1B) | command(1B) | length(2B, 大端) |
 *   data(N) | checksum(1B, 前面所有字节求和取低8位) | EOF(2B) 0x54 0x43
 *
 * UART: 115200 8N1, TTL 3.3V 兼容
 */
#ifndef R60ABD1_H
#define R60ABD1_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 关注的上报类型（control/command）——量产前对照协议文档核对（2026.7.17已核对用户手册，无误） ---- */
#define R60_CTL_PRESENCE   0x80  /* 人体存在类 */
#define R60_CMD_EXIST      0x01  /*   存在: 0=无人 1=有人 */
#define R60_CMD_MOTION     0x02  /*   运动: 0=无 1=静止 2=活动 */
#define R60_CMD_BODYMOVE   0x03  /*   体动幅度参数: 0~100 */
#define R60_CMD_DISTANCE   0x04  /*   目标距离(cm, 2B 大端) */
#define R60_CMD_POSITION   0x05  /*   人体3D坐标(cm, 6B: x(2B) y(2B) z(2B), 有符号大端) */

#define R60_CTL_BREATH     0x81  /* 呼吸类 */
#define R60_CMD_BREATH_VAL 0x02  /*   呼吸率(次/分) */

#define R60_CTL_SLEEP      0x84  /* 睡眠类 */
#define R60_CMD_BED        0x01  /*   在床: 0=离床 1=在床 */
#define R60_CMD_SLEEP_STG  0x02  /*   分期: 0=深睡 1=浅睡 2=清醒 (核对!) shouc*/

#define R60_CTL_HEART      0x85  /* 心率类 */
#define R60_CMD_HEART_VAL  0x02  /*   心率(次/分) */

/* ---- 解析结果：雷达实时状态快照 ---- */
typedef struct {
    bool     presence;        /* 有人/无人 */
    uint8_t  motion;          /* 0 无 / 1 静止 / 2 活动 */
    uint8_t  body_move;       /* 体动幅度 0~100 */
    /* distance_cm 已弃用——改用 DP5 x/y 判断位移 */
    int16_t  x_cm;            /* DP5 人体X坐标 cm */
    int16_t  y_cm;            /* DP5 人体Y坐标 cm */
    uint8_t  breath_rate;     /* 呼吸率 次/分（0=无效） */
    uint8_t  heart_rate;      /* 心率 次/分（0=无效） */
    bool     in_bed;          /* 在床/离床 */
    uint8_t  sleep_stage;     /* 睡眠分期原始值 */
    uint32_t last_update_ms;  /* 最近一次任意上报的时间戳（宿主时钟） */
} r60_status_t;

/* 帧回调：ctl/cmd/数据区。返回前 status 已按已知消息类型更新。 */
typedef void (*r60_frame_cb)(uint8_t ctl, uint8_t cmd,
                             const uint8_t *data, uint16_t len, void *user);
// 解析器实例
typedef struct {
    /* 内部解析状态机 */
    uint8_t  buf[64];         // 接受缓冲区
    uint16_t pos;             // 状态机内部变量
    uint16_t expect_len;     /* data 长度（从帧头解析） */
    int      stage;          /* 0:找SOF1 1:找SOF2 2:头部 3:数据+校验+尾 */
    /* 输出 */
    r60_status_t status;        // 解析得到的雷达状态快照
    r60_frame_cb cb;            // 帧回调函数
    void        *cb_user;       // 用户指针
    uint32_t     frames_ok;     // 成功解析的帧数
    uint32_t     frames_bad;    // 解析失败的帧数(占比需小于1%)
} r60_parser_t;

// 初始化解析器并注册回调函数
void r60_init(r60_parser_t *p, r60_frame_cb cb, void *user);

/* 逐字节喂入（可在 UART 事件任务里按块循环调用）。
 * now_ms: 宿主毫秒时钟，用于给 status.last_update_ms 打戳。 */
void r60_feed(r60_parser_t *p, uint8_t byte, uint32_t now_ms);

// 内联函数，返回雷达状态快照结构体指针
static inline const r60_status_t *r60_status(const r60_parser_t *p) { return &p->status; }

/* ---- 通用命令帧构建 ---- */
static inline int r60_build_cmd(uint8_t *out_buf,
                                 uint8_t ctl, uint8_t cmd,
                                 const uint8_t *data, uint8_t data_len)
{
    out_buf[0] = 0x53;
    out_buf[1] = 0x59;
    out_buf[2] = ctl;
    out_buf[3] = cmd;
    out_buf[4] = 0x00;
    out_buf[5] = data_len;
    if (data_len > 0 && data)
        memcpy(&out_buf[6], data, data_len);
    uint16_t sum = 0;
    for (uint8_t i = 0; i < (uint8_t)(6 + data_len); i++)
        sum = (uint16_t)(sum + out_buf[i]);
    out_buf[6 + data_len] = (uint8_t)sum;
    out_buf[7 + data_len] = 0x54;
    out_buf[8 + data_len] = 0x43;
    return 9 + data_len;
}

#ifdef __cplusplus
}
#endif
#endif /* R60ABD1_H */
