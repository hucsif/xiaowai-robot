#pragma once

#include <stddef.h>
#include <ESP32Servo.h>
#include "deskbot_config.h"
#include "pb_model.h"

// Servo（见 deskbot_config.h）
#define X_PIN DESKBOT_ROM_X_PIN
#define Y_PIN DESKBOT_ROM_Y_PIN
/** 舵机物理极限（°）；所有运动均 constrain 于此。 */
#define X_MIN_LIMIT 0
#define X_MAX_LIMIT 180
#define Y_MIN_LIMIT 70
#define Y_MAX_LIMIT 110
/** 舵机 PWM 更新周期（ms）= 50Hz，motor_task 每拍间隔。 */
constexpr uint16_t SERVO_TICK_MS = 20;
constexpr size_t HEAD_MOTOR_QUEUE_DEPTH = DESKBOT_PB_EXECUTOR_QUEUE_DEPTH;

/** 固定逻辑中位（°）。 */
constexpr int X_CENTER = 90;
constexpr int Y_CENTER = 90;

extern Servo servo_x;
extern Servo servo_y;

/** 读 X 轴 PWM 目标角（逻辑角）；无物理反馈，不等于机械真实位置。 */
int head_read_x();
/** 读 Y 轴 PWM 目标角（逻辑角）；同上。 */
int head_read_y();

/** 与 pb_servo_frame.xm / .ym 一致；motor 队列内 `MotorCmd` 使用同一编码。 */
constexpr uint8_t HEAD_SERVO_ABS = 0;
constexpr uint8_t HEAD_SERVO_REL = 1;
constexpr uint8_t HEAD_SERVO_HOLD = 2;
/** 「看向人」：X 轴目标角不由下发方给，而是运行时向已注册的提供者（radar 模块）
 *  索取 —— 服务端不知道雷达坐标，只发这个模式位表示「转向说话人」。
 *  仅对 X 轴有效；提供者返回 HEAD_LOOK_ANGLE_NONE 时本轴不动。 */
constexpr uint8_t HEAD_SERVO_LOOK = 3;
/** 提供者返回此值表示「无可用目标」，舵机不动。 */
constexpr int HEAD_LOOK_ANGLE_NONE = -1;

/** 注册「看向人」目标角提供者（幂等）。由 radar 模块在 setup 时调用一次。
 *  提供者须返回**已 constrain 到 [X_MIN_LIMIT, X_MAX_LIMIT] 的目标角** ——
 *  方位角→舵机角的映射（增益/中心/距离窗口）是雷达模块自己的配置。 */
void head_set_look_angle_provider(int (*fn)(void));

// Functions
/** 双轴 MCPWM attach + 写中位（幂等），上电即可调用。 */
void setup_head();
/** 启动舵机 motor 队列与 motor_task（幂等）；enqueue 路径亦可兜底。 */
void task_setup_head();
/** 提交 pb_servo_frame[] chunk 所有权到 motor 队列（1 chunk = 1 队列项）。 */
void head_submit_pb_servo_chunk_owned(pb_servo_frame* frames, size_t count);

/**
 * 本地（非 pb）X 轴绝对角度平滑移动：从当前逻辑角按 ms 线性插值转过去。
 *
 * 与 head_submit_pb_servo_chunk_owned 的区别：**不分配堆内存、无所有权转移、
 * 不触碰 pb 的 s_task_done 标志** —— 后者被本地调用会与 pb_runtime 的末窗口
 * 等待产生竞态（可致 120s 超时）。
 *
 * 须在 task_setup_head() 之后调用；队列未建或已满时返回 false（非阻塞）。
 * 角度由 execute_motor_cmd() 内部 constrain 到 [X_MIN_LIMIT, X_MAX_LIMIT]。
 */
bool head_move_x_abs(int deg, uint16_t ms);
/** 打断：置 need_cancel，再队尾入队 type=cancel。 */
void head_abort();
/** 当前任务是否已执行完毕（kEndOfTask 已出队）。 */
bool head_task_done();
/** 入队 kEndOfTask 标记；task_loop 处理后 head_task_done() 为 true。 */
void head_signal_task_done();
/** 直接设置完成标志（cancel 场景，不走队列）。 */
void head_set_task_done_flag();
/** xQueue 缓冲深度（供 pb 回压）。 */
unsigned head_motor_input_queue_depth();

