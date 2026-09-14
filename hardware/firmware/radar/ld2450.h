/*
 * ld2450.h — 海凌科 LD2450 24GHz 运动目标追踪雷达 UART 协议解析器
 *
 * 纯 C、无平台依赖。帧格式（来源：LD2450 串口通信协议 V1.03）：
 *
 *   SOF(4B) 0xAA 0xFF 0x03 0x00 | 目标1(8B) 目标2(8B) 目标3(8B) | EOF(2B) 0x55 0xCC
 *
 * 每个目标 8 字节（小端序）：
 *   [0-1] X坐标 signed int16 LE, 最高位1=正 0=负, 单位 mm
 *   [2-3] Y坐标 signed int16 LE, 最高位1=正 0=负, 单位 mm
 *   [4-5] 速度   signed int16 LE, 最高位1=正 0=负, 单位 cm/s
 *   [6-7] 距离分辨率 uint16 LE, 单位 mm
 *
 * 每秒上报 10 帧，帧长固定 30 字节，无校验和。
 * UART: 256000 8N1, TTL 3.3V
 *
 * 面向"转头朝人"功能：只需目标 X/Y 坐标算出水平角度。
 */
#ifndef LD2450_H
#define LD2450_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 帧固定长度 */
#define LD2450_FRAME_LEN        30  /* SOF(4) + 3*8 + EOF(2) */
#define LD2450_TARGET_COUNT      3

/* 帧标记 */
#define LD2450_SOF1  0xAA
#define LD2450_SOF2  0xFF
#define LD2450_SOF3  0x03
#define LD2450_SOF4  0x00
#define LD2450_EOF1  0x55
#define LD2450_EOF2  0xCC

/* 上报数据帧（无需命令，上电自动输出） */

/* ---- 单个目标信息 ---- */
typedef struct {
    bool     valid;         /* 是否有目标（坐标全零=无目标） */
    int16_t  x_mm;          /* X坐标 mm（雷达平面横向） */
    int16_t  y_mm;          /* Y坐标 mm（雷达前方距离） */
    int16_t  speed_cms;     /* 速度 cm/s */
    uint16_t resolution_mm; /* 距离分辨率 mm */
} ld2450_target_t;

/* ---- 解析结果：实时追踪快照 ---- */
typedef struct {
    ld2450_target_t targets[LD2450_TARGET_COUNT]; /* 最多3个目标 */
    uint32_t last_update_ms;
} ld2450_status_t;

/* 帧回调 */
typedef void (*ld2450_frame_cb)(const ld2450_target_t *targets,
                                 int count, void *user);

/* 解析器实例 */
typedef struct {
    uint8_t  buf[LD2450_FRAME_LEN];
    uint8_t  pos;
    ld2450_status_t status;
    ld2450_frame_cb cb;
    void    *cb_user;
    uint32_t frames_ok;
    uint32_t frames_bad;
} ld2450_parser_t;

void ld2450_init(ld2450_parser_t *p, ld2450_frame_cb cb, void *user);
void ld2450_feed(ld2450_parser_t *p, uint8_t byte, uint32_t now_ms);

static inline const ld2450_status_t *ld2450_status(const ld2450_parser_t *p) {
    return &p->status;
}

#ifdef __cplusplus
}
#endif
#endif /* LD2450_H */
