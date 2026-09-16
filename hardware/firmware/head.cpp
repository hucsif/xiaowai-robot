#include "head.h"

#include <ESP32Servo.h>
#include <atomic>
#include <driver/gpio.h>
#include <soc/gpio_periph.h>
#include <soc/io_mux_reg.h>

#include "logger.h"
#include "utils/utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

Servo servo_x;
Servo servo_y;

/** motor_task 维护的逻辑角；未 attach 时 head_read_* 返回此值。 */
static int s_logical_x = 90;
static int s_logical_y = 90;

static bool s_servos_attached = false;

static constexpr int kServoPulseMinUs = 1000;
static constexpr int kServoPulseMaxUs = 2000;

/** attach 单轴并立即 write，防止 attach 时输出默认脉冲导致舵机乱动。 */
static bool head_servo_attach_axis(Servo& servo, int pin, int deg, const char* label) {
  const int ch = servo.attach(pin, kServoPulseMinUs, kServoPulseMaxUs);
  if (!servo.attached()) {
    log_error("[SERVO] attach %s pin=%d failed (ch=%d)", label, pin, ch);
    return false;
  }
  servo.write(deg);
  log_info("[SERVO] attach %s pin=%d ok ch=%d deg=%d", label, pin, ch, deg);
  return true;
}

int head_read_x() { return s_logical_x; }

int head_read_y() { return s_logical_y; }

namespace {

/** 与 pb_servo_frame 同形。xm/ym 见 head.h HEAD_SERVO_*。 */
struct MotorCmd {
  uint8_t xm, ym;
  int     x,  y;
  uint16_t ms;       /**< 非 0：本段墙钟总预算（ms）。 */
};

/** 执行器任务元素 type；cancel 走 xQueue 队尾，作旧/新任务分界。 */
enum class MotorJobType : uint8_t {
  kCancel = 0,
  kPbServoChunk = 2,
  kEndOfTask = 3,
  /** 本地（非 pb）单条舵机指令：纯 POD、无堆所有权、不碰 s_task_done。
   *  供 VAD 转头等本地功能使用 —— 走 pb 的 kPbServoChunk 会置 s_task_done=false，
   *  与 pb_runtime 的末窗口等待存在竞态（可致 120s 超时）。 */
  kLocalServo = 4,
};

struct MotorJob {
  MotorJobType type = MotorJobType::kPbServoChunk;
  pb_servo_frame* servo_frames = nullptr;
  size_t servo_count = 0;
  MotorCmd local{};  /* kLocalServo 用；无所有权，直接内联 */
};

QueueHandle_t s_motor_queue = nullptr;
TaskHandle_t  s_motor_task  = nullptr;
std::atomic<bool> s_need_cancel{false};
static std::atomic<bool> s_task_done{true};

/** 「看向人」目标角提供者（radar 模块注册）；nullptr = LOOK 模式不可用。 */
int (*s_look_angle_provider)(void) = nullptr;

static void free_motor_job(MotorJob& job) {
  if (job.type == MotorJobType::kPbServoChunk) {
    pb_servo_frames_free(job.servo_frames);
    job.servo_frames = nullptr;
    job.servo_count = 0;
  }
}

/** 根据 xm/ym 模式将命令值转换为目标角度。 */
static int resolve_target(uint8_t mode, int cur, int val, int lo, int hi) {
  if (mode == HEAD_SERVO_ABS) return constrain(val,       lo, hi);
  if (mode == HEAD_SERVO_REL) return constrain(cur + val, lo, hi);
  return cur; /* HEAD_SERVO_HOLD 或非法值 */
}

/**
 * need_cancel==false → false。
 * 否则非阻塞丢弃旧任务，见到 cancel 则清 flag 并 return true（其后新任务保留）。
 * 队列空且未见 cancel → return true，保持 need_cancel。
 */
static bool poll_cancel() {
  if (!s_need_cancel.load(std::memory_order_acquire)) {
    return false;
  }
  MotorJob j{};
  while (xQueueReceive(s_motor_queue, &j, 0) == pdTRUE) {
    if (j.type == MotorJobType::kCancel) {
      s_need_cancel.store(false, std::memory_order_release);
      log_info("[HEAD] cancel");
      return true;
    }
    free_motor_job(j);
  }
  return true;
}

/** @return false 若中途 need_cancel。仅时间预算模式（ms > 0）。 */
/** LOOK 模式：向 radar 模块索取目标角。无目标/未注册 → HEAD_LOOK_ANGLE_NONE。
 *
 *  ⚠️ 这里刻意**不做**「已在位就不动」的死区判断（曾经有 `< 3°` 的版本，已删）：
 *     转过去的角度还没回中、人又站在同一位置时，新 LOOK 算出的目标角与当前角
 *     几乎相同 → 被判成「已在位」→ 整个转向被吞掉（表现为「有时不转向」）。
 *     代价是目标角与当前角相同时也走一次 no-op 插值，但 LOOK 每轮 ASR 只下发
 *     一次，这点开销无关紧要。 */
static int resolve_look_target() {
  if (!s_look_angle_provider) {
    return HEAD_LOOK_ANGLE_NONE;
  }
  const int deg = s_look_angle_provider();
  if (deg == HEAD_LOOK_ANGLE_NONE) {
    return HEAD_LOOK_ANGLE_NONE;
  }
  return constrain(deg, X_MIN_LIMIT, X_MAX_LIMIT);
}

static bool execute_motor_cmd(const MotorCmd& cmd) {
  if (cmd.ms <= 0) return true;
  int x_target;
  if (cmd.xm == HEAD_SERVO_LOOK) {
    x_target = resolve_look_target();
    if (x_target == HEAD_LOOK_ANGLE_NONE) {
      return true; /* 无目标或已在位：当作已完成，不占用插值时间 */
    }
  } else {
    x_target = resolve_target(cmd.xm, s_logical_x, cmd.x, X_MIN_LIMIT, X_MAX_LIMIT);
  }
  const int y_target = resolve_target(cmd.ym, s_logical_y, cmd.y, Y_MIN_LIMIT, Y_MAX_LIMIT);

  const int x_start = s_logical_x, y_start = s_logical_y;
  const long dx_total = (long)x_target - x_start;
  const long dy_total = (long)y_target - y_start;
  const uint16_t total_ms = cmd.ms;

  TickType_t last_wake = xTaskGetTickCount();
  uint16_t elapsed_ms = 0;

  while (elapsed_ms < total_ms) {
    if (poll_cancel()) {
      return false;
    }
    const uint16_t slice = (total_ms - elapsed_ms >= SERVO_TICK_MS)
                               ? SERVO_TICK_MS
                               : (total_ms - elapsed_ms);
    elapsed_ms += slice;
    const int x_next = x_start + (int)(dx_total * elapsed_ms / total_ms);
    s_logical_x = constrain(x_next, X_MIN_LIMIT, X_MAX_LIMIT);
    servo_x.write(s_logical_x);

    const int y_next = y_start + (int)(dy_total * elapsed_ms / total_ms);
    s_logical_y = constrain(y_next, Y_MIN_LIMIT, Y_MAX_LIMIT);
#if DESKBOT_HAS_SERVO_Y
    servo_y.write(s_logical_y);
#endif
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(slice));
  }
  return true;
}

/* ---- motor_task ---- */

static void task_loop_motor(void* /*arg*/) {
  MotorJob job{};
  for (;;) {
    (void)poll_cancel();
    if (xQueueReceive(s_motor_queue, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (job.type == MotorJobType::kCancel) {
      if (s_need_cancel.exchange(false, std::memory_order_acq_rel)) {
        log_info("[HEAD] cancel");
      }
      continue;
    }
    if (job.type == MotorJobType::kEndOfTask) {
      s_task_done.store(true, std::memory_order_release);
      continue;
    }
    if (job.type == MotorJobType::kLocalServo) {
      /* 本地单条指令：不需要 free_motor_job（无堆所有权） */
      (void)execute_motor_cmd(job.local);
      continue;
    }
    /* kPbServoChunk：逐帧执行，任意帧被 cancel 则中断本 chunk。 */
    if (job.servo_frames && job.servo_count > 0) {
      for (size_t i = 0; i < job.servo_count; ++i) {
        const pb_servo_frame& f = job.servo_frames[i];
        MotorCmd cmd{};
        cmd.xm = (uint8_t)constrain(f.xm, 0, 3); /* 3 = HEAD_SERVO_LOOK */
        cmd.ym = (uint8_t)constrain(f.ym, 0, 2);
        cmd.x  = f.x;
        cmd.y  = f.y;
        cmd.ms = f.ms > 0 ? (uint16_t)constrain(f.ms, 0, 65535) : (uint16_t)SERVO_TICK_MS;
        if (!execute_motor_cmd(cmd)) {
          break;
        }
      }
    }
    free_motor_job(job);
  }
}

}  // namespace

/* ================================================================
 * 公开接口实现
 * ================================================================ */

void task_setup_head() {
  if (s_motor_queue && s_motor_task) return;
  s_motor_queue       = xQueueCreate(HEAD_MOTOR_QUEUE_DEPTH, sizeof(MotorJob));
  if (!s_motor_queue) {
    log_error("[HEAD] motor queue create failed");
    return;
  }
  const BaseType_t rc =
      utils_task_create_pinned(task_loop_motor, "motor", 8 * 1024, nullptr, 3, &s_motor_task, APP_CPU_NUM);
  if (rc != pdPASS) {
    log_error("[HEAD] motor task create rc=%d", (int)rc);
  } else {
    log_info("[HEAD] motor task started");
  }
}

void head_submit_pb_servo_chunk_owned(pb_servo_frame* frames, size_t count) {
  if (!frames || count == 0) {
    if (frames) {
      pb_servo_frames_free(frames);
    }
    return;
  }
  MotorJob job{};
  job.type = MotorJobType::kPbServoChunk;
  job.servo_frames = frames;
  job.servo_count = count;
  s_task_done.store(false, std::memory_order_release);
  if (xQueueSend(s_motor_queue, &job, 0) != pdTRUE) {
    pb_servo_frames_free(frames);
    s_task_done.store(true, std::memory_order_release);
    log_warn("[HEAD] motor queue full; drop new command");
    return;
  }
  log_info("[HEAD] pb servo[] submitted segs=%u", (unsigned)count);
}

void head_set_look_angle_provider(int (*fn)(void)) {
  s_look_angle_provider = fn;
}

bool head_move_x_abs(int deg, uint16_t ms) {
  /* 队列由 task_setup_head() 建；它晚于 task_setup_radar()，所以开机早期
   * 调用会走到这里 → 返回 false 而不是在 NULL 队列上 xQueueSend。 */
  if (!s_motor_queue) {
    return false;
  }
  MotorJob job{};
  job.type = MotorJobType::kLocalServo;
  job.local.xm = HEAD_SERVO_ABS;   /* 绝对角度 */
  job.local.ym = HEAD_SERVO_HOLD;  /* 只动 X，Y 保持（本板 Y 也未接） */
  job.local.x  = deg;
  job.local.y  = 0;
  job.local.ms = (ms > 0) ? ms : (uint16_t)SERVO_TICK_MS;
  if (xQueueSend(s_motor_queue, &job, 0) != pdTRUE) {
    log_warn("[HEAD] local move queue full; drop deg=%d", deg);
    return false;
  }
  return true;
}

/* ---- 初始化 ---- */

/** 双轴 MCPWM attach + 立即写中位。 */
void setup_head() {
  if (s_servos_attached) return;
  const int x = constrain(X_CENTER, X_MIN_LIMIT, X_MAX_LIMIT);
  const int y = constrain(Y_CENTER, Y_MIN_LIMIT, Y_MAX_LIMIT);
#if DESKBOT_HAS_SERVO_Y
  if (!head_servo_attach_axis(servo_y, Y_PIN, y, "Y")) {
    log_error("[SERVO] boot: attach Y failed");
    return;
  }
#endif
  if (!head_servo_attach_axis(servo_x, X_PIN, x, "X")) {
#if DESKBOT_HAS_SERVO_Y
    servo_y.detach();
#endif
    log_error("[SERVO] boot: attach X failed");
    return;
  }
  s_servos_attached = true;
  s_logical_x = X_CENTER;
  s_logical_y = Y_CENTER;
  log_info("[SERVO] boot attach ok pos=(%d,%d)", x, y);
}

/* ---- 任务管理 ---- */

void head_abort() {
  if (!s_motor_queue) {
    return;
  }
  MotorJob job{};
  job.type = MotorJobType::kCancel;
  s_need_cancel.store(true, std::memory_order_release);
  (void)xQueueSend(s_motor_queue, &job, portMAX_DELAY);
}

unsigned head_motor_input_queue_depth() {
  if (!s_motor_queue) {
    return 0;
  }
  return (unsigned)uxQueueMessagesWaiting(s_motor_queue);
}

bool head_task_done() {
  return s_task_done.load(std::memory_order_acquire);
}

void head_signal_task_done() {
  MotorJob job{};
  job.type = MotorJobType::kEndOfTask;
  (void)xQueueSend(s_motor_queue, &job, portMAX_DELAY);
}

void head_set_task_done_flag() {
  s_task_done.store(true, std::memory_order_release);
}
