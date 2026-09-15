#include "radar.h"

#include "ld2450.h"
#include "r60abd1.h"
#include "radar_config.h"
#include "seat_fsm.h"
#include "squat_fsm.h"
#include "wave_fsm.h"

#include "logger.h"
#include "utils/utils.h"
#include "ws_transport.h"

#include <Arduino.h>
#include <HardwareSerial.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 雷达采集：两路 UART 非阻塞轮询 → 纯 C 解析器 → 限频日志。
 *
 * UART 实现用 Arduino HardwareSerial 而非裸 IDF uart_driver_install：
 *   - begin(baud, cfg, rxPin, txPin) 自动走 GPIO 矩阵重映射，正好满足
 *     R60ABD1 在 G9/G10 上跑 UART2 的需求；
 *   - 两路数据率极低（LD2450 满帧 30B×10Hz=300B/s，R60ABD1 更低），
 *     不存在缓冲溢出风险，无需 DMA；
 *   - 与项目 Arduino 风格一致，也不干扰 cmd.cpp 的 USB CDC 读取与 logger 输出。
 *
 * 注：自行构造 HardwareSerial(UART1/UART2) 是安全的——内核的 uart_instances[]
 * 注册只发生在 begin() 且「先到先得」，构造函数不做任何校验或断言。
 * 内核预定义的 Serial1/Serial2 从未 begin()，处于惰性状态。
 */

namespace {

constexpr uint32_t kRadarTaskStack = 8 * 1024;
constexpr UBaseType_t kRadarTaskPrio = 3; /* 与 motor / camera 同档：非实时 */

/** 每轮每路最多消化的字节数：防止某一路突发把任务按在这里、饿死另一路。 */
constexpr int kDrainMaxBytes = 128;
/** 轮询周期（ms）。两路合计约 300B/s，10ms 轮询余量极大。 */
constexpr uint32_t kPollIntervalMs = 10;
/** 入座/离座 FSM 的 tick 周期（ms）。与上游 fsm_tick 同为 200ms。 */
constexpr uint32_t kSeatTickMs = 200;

HardwareSerial s_r60_serial(RADAR_R60_UART_NUM);
HardwareSerial s_ld2450_serial(RADAR_LD2450_UART_NUM);

r60_parser_t    s_r60;
ld2450_parser_t s_ld2450;

/* 姿势检测 FSM（纯 C，判据与上游 caterpillar 项目的 radar_fsm / wave_fsm 一致） */
squat_fsm_t s_squat; /* R60：蹲下检测（事件驱动） */
wave_fsm_t  s_wave;  /* LD2450：挥手检测（事件驱动） */
seat_fsm_t  s_seat;  /* R60：入座/离座（tick 驱动，200ms） */

TaskHandle_t s_task = nullptr;
bool         s_ready = false;

uint32_t s_last_log_ms = 0;
/** 各路最近一次「有效帧数增加」的时刻，用于无数据排障。 */
uint32_t s_r60_ok_ms = 0;
uint32_t s_ld_ok_ms = 0;
uint32_t s_r60_prev_ok = 0;
uint32_t s_ld_prev_ok = 0;
bool     s_r60_warned = false;
bool     s_ld_warned = false;
/** 上次记录的存在状态 / 目标数，-1 表示尚未初始化（首帧不报跳变）。 */
int s_prev_presence = -1;
int s_prev_targets = -1;
int s_prev_bed = -1;    /* 上次的在床状态（-1 = 尚未初始化，首帧不报跳变） */
int s_prev_stage = -1;  /* 上次的睡眠分期 */
uint32_t s_last_seat_tick = 0;
/** WS 是否处于「已连接」的活动态；false 时采集/检测/日志全部暂停。 */
bool s_ws_active = false;

/* ──────────────────────────────────────────────────────────────────────
 * 采集：非阻塞 drain
 * ──────────────────────────────────────────────────────────────────── */

/** 每轮只消化当前可读的字节，读完立刻返回，绝不等待。 */
void drain_r60(uint32_t now) {
  const int avail = s_r60_serial.available();
  if (avail <= 0) {
    return;
  }
  const int n = (avail > kDrainMaxBytes) ? kDrainMaxBytes : avail;
  for (int i = 0; i < n; ++i) {
    const int b = s_r60_serial.read();
    if (b < 0) {
      break;
    }
    r60_feed(&s_r60, (uint8_t)b, now);
  }
}

void drain_ld2450(uint32_t now) {
  const int avail = s_ld2450_serial.available();
  if (avail <= 0) {
    return;
  }
  const int n = (avail > kDrainMaxBytes) ? kDrainMaxBytes : avail;
  for (int i = 0; i < n; ++i) {
    const int b = s_ld2450_serial.read();
    if (b < 0) {
      break;
    }
    ld2450_feed(&s_ld2450, (uint8_t)b, now);
  }
}

/* ──────────────────────────────────────────────────────────────────────
 * 日志
 * ──────────────────────────────────────────────────────────────────── */

int ld2450_valid_count(const ld2450_status_t* l) {
  int count = 0;
  for (int i = 0; i < LD2450_TARGET_COUNT; ++i) {
    if (l->targets[i].valid) {
      ++count;
    }
  }
  return count;
}

/** 状态跳变即时输出（有人/无人、目标数），让你能直接看到实时活动。 */
void log_events() {
  const r60_status_t* r = r60_status(&s_r60);
  const int presence = r->presence ? 1 : 0;
  if (presence != s_prev_presence) {
    if (s_prev_presence >= 0) {
      log_warn("[RADAR/R60] presence: %d -> %d", s_prev_presence, presence);
    }
    s_prev_presence = presence;
  }

  const int targets = ld2450_valid_count(ld2450_status(&s_ld2450));
  if (targets != s_prev_targets) {
    if (s_prev_targets >= 0) {
      log_warn("[RADAR/LD2450] targets: %d -> %d", s_prev_targets, targets);
    }
    s_prev_targets = targets;
  }

  /* 在床/离床：R60 硬件已判定，直接读字段（入床即时上报；离床约 30s 后） */
  const int bed = r->in_bed ? 1 : 0;
  if (bed != s_prev_bed) {
    if (s_prev_bed >= 0) {
      log_warn("[RADAR/R60] bed: %d -> %d (%s)", s_prev_bed, bed, bed ? "在床" : "离床");
    }
    s_prev_bed = bed;
  }

  /* 睡眠分期：0=深睡 1=浅睡 2=清醒 3=无(离床/实时模式)。
   * 仅入床状态下有效，且每 10 分钟才上报一次，故变化很稀疏。 */
  const int stage = (int)r->sleep_stage;
  if (stage != s_prev_stage) {
    if (s_prev_stage >= 0) {
      static const char* const kStageName[] = {"深睡", "浅睡", "清醒", "无"};
      const char* nm = (stage >= 0 && stage <= 3) ? kStageName[stage] : "未知";
      log_warn("[RADAR/R60] sleep_stage: %d -> %d (%s)", s_prev_stage, stage, nm);
    }
    s_prev_stage = stage;
  }
}

/** 记录「有效帧数增加」的时刻；恢复后允许再次告警。 */
void update_liveness(uint32_t now) {
  if (s_r60.frames_ok != s_r60_prev_ok) {
    s_r60_prev_ok = s_r60.frames_ok;
    s_r60_ok_ms = now;
    s_r60_warned = false;
  }
  if (s_ld2450.frames_ok != s_ld_prev_ok) {
    s_ld_prev_ok = s_ld2450.frames_ok;
    s_ld_ok_ms = now;
    s_ld_warned = false;
  }
}

/**
 * 无数据排障提示：某路连续 RADAR_SILENT_WARN_MS 无有效帧就打一次。
 * 这是验证接线的主要手段，文案直接引导排查方向：
 *   - 完全没有字节进来 -> 接线/供电/波特率完全不匹配（frames_bad==0）
 *   - 有字节但帧校验不过 -> 多半是波特率不匹配或未共地（frames_bad 在涨）
 */
void log_silent_diag(uint32_t now) {
  if (!s_r60_warned && s_r60_ok_ms != 0 &&
      (uint32_t)(now - s_r60_ok_ms) >= RADAR_SILENT_WARN_MS) {
    s_r60_warned = true;
    if (s_r60.frames_bad > 0) {
      log_warn("[RADAR/R60] %us 无有效帧(bad=%u 在增长) — 多是波特率不匹配/未共地，应为 %d 8N1",
               (unsigned)(RADAR_SILENT_WARN_MS / 1000), (unsigned)s_r60.frames_bad,
               RADAR_R60_BAUD);
    } else {
      log_warn("[RADAR/R60] %us 无任何数据 — 查接线: 模块TX->GPIO%d, 模块RX->GPIO%d, %d 8N1, 3V3/GND",
               (unsigned)(RADAR_SILENT_WARN_MS / 1000), RADAR_R60_RX_PIN, RADAR_R60_TX_PIN,
               RADAR_R60_BAUD);
    }
  }

  if (!s_ld_warned && s_ld_ok_ms != 0 &&
      (uint32_t)(now - s_ld_ok_ms) >= RADAR_SILENT_WARN_MS) {
    s_ld_warned = true;
    if (s_ld2450.frames_bad > 0) {
      log_warn("[RADAR/LD2450] %us 无有效帧(bad=%u 在增长) — 多是波特率不匹配/未共地，应为 %d 8N1",
               (unsigned)(RADAR_SILENT_WARN_MS / 1000), (unsigned)s_ld2450.frames_bad,
               RADAR_LD2450_BAUD);
    } else {
      log_warn("[RADAR/LD2450] %us 无任何数据 — 查接线: 模块TX->GPIO%d, 模块RX->GPIO%d, %d 8N1, 3V3/GND",
               (unsigned)(RADAR_SILENT_WARN_MS / 1000), RADAR_LD2450_RX_PIN, RADAR_LD2450_TX_PIN,
               RADAR_LD2450_BAUD);
    }
  }
}

/** 周期摘要：计数器 + 关键字段。LD2450 的 mm 换算成 cm 更易读。 */
void log_summary(uint32_t now) {
#if RADAR_LOG_INTERVAL_MS > 0
  if (s_last_log_ms != 0 && (uint32_t)(now - s_last_log_ms) < RADAR_LOG_INTERVAL_MS) {
    return;
  }
  s_last_log_ms = now;

  const r60_status_t* r = r60_status(&s_r60);
  /* 不再打印 pos= —— R60 的 DP5 坐标已停用，位置看下面 LD2450 那一行 */
  log_warn("[RADAR/R60] ok=%u bad=%u | presence=%d motion=%u move=%u "
           "breath=%u heart=%u bed=%d stage=%u",
           (unsigned)s_r60.frames_ok, (unsigned)s_r60.frames_bad, r->presence ? 1 : 0,
           (unsigned)r->motion, (unsigned)r->body_move,
           (unsigned)r->breath_rate, (unsigned)r->heart_rate, r->in_bed ? 1 : 0,
           (unsigned)r->sleep_stage);

  const ld2450_status_t* l = ld2450_status(&s_ld2450);
  char tgt[128];
  size_t off = 0;
  int count = 0;
  for (int i = 0; i < LD2450_TARGET_COUNT; ++i) {
    const ld2450_target_t& t = l->targets[i];
    if (!t.valid) {
      continue;
    }
    const int n = snprintf(tgt + off, sizeof(tgt) - off, " T%d(x=%d,y=%d,v=%d)", i,
                           (int)(t.x_mm / 10), (int)(t.y_mm / 10), (int)t.speed_cms);
    if (n <= 0 || (size_t)n >= (sizeof(tgt) - off)) {
      break; /* 缓冲放不下剩余目标：截断，不溢出 */
    }
    off += (size_t)n;
    ++count;
  }
  if (count == 0) {
    snprintf(tgt, sizeof(tgt), " none");
  }
  log_warn("[RADAR/LD2450] ok=%u bad=%u | n=%d%s", (unsigned)s_ld2450.frames_ok,
           (unsigned)s_ld2450.frames_bad, count, tgt);
#else
  (void)now;
#endif
}

/* ──────────────────────────────────────────────────────────────────────
 * 姿势检测：帧回调 → FSM 分发
 *
 * 注册为解析器的帧回调（r60_init / ld2450_init 的第二个参数），每解析出
 * 一帧就调用一次。分发条件与上游 caterpillar 的 glue.cc 保持一致。
 * ──────────────────────────────────────────────────────────────────── */

/** R60 帧回调：只把体动幅度分发给蹲下 FSM。
 *  蹲下的「位置」判据已改由 LD2450 提供（见 on_ld2450_frame）——R60 的 DP5
 *  上报频率远低于 LD2450，不够平滑。DP5 的解析也已在 r60abd1.c 里 #if 0。 */
void on_r60_frame(uint8_t ctl, uint8_t cmd, const uint8_t* /*data*/, uint16_t /*len*/,
                  void* /*user*/) {
  if (ctl != R60_CTL_PRESENCE || cmd != R60_CMD_BODYMOVE) {
    return; /* 只处理 0x80/0x03：体动幅度，1Hz */
  }
  squat_on_body_move(&s_squat, r60_status(&s_r60)->body_move, millis());
}

/** LD2450 帧回调：取第一个有效且在 3m 内的目标喂挥手 FSM。 */
void on_ld2450_frame(const ld2450_target_t* targets, int count, void* /*user*/) {
  const uint32_t now = millis();
  const ld2450_target_t* pick = nullptr;
  for (int i = 0; i < count; ++i) {
    if (targets[i].valid && targets[i].y_mm > 0 && targets[i].y_mm < 3000) {
      pick = &targets[i];
      break;
    }
  }
  if (pick) {
    wave_on_frame(&s_wave, true, pick->x_mm, pick->y_mm, pick->speed_cms, now);

    /* 蹲下的「原地」判据用 LD2450 的坐标：10Hz，比 R60 的 DP5 平滑得多。
     * ⚠️ 单位换算：LD2450 给的是 mm，而 squat 的阈值 squat_xy_max_cm 是 cm，
     *    必须 /10 —— 否则 15 会变成 15mm（1.5cm），判据严到永远不成立。 */
    squat_on_position(&s_squat, (int16_t)(pick->x_mm / 10), (int16_t)(pick->y_mm / 10), now);
  } else {
    /* 无有效目标：喂 valid=false，让 wave FSM 重置首帧标志，避免跨空档误判翻转。
     * squat 这边不喂 —— 拿不到位置时保持上次的 pos_stable，不凭空判「移动了」。 */
    wave_on_frame(&s_wave, false, 0, 0, 0, now);
  }
}

/** 事件回调：当前只打日志；后续可接舵机（head.h）/ 表情（display.h）/ 上报。 */
void on_squat_event(squat_event_t ev, int data, void* /*user*/) {
  log_warn("[RADAR/SQUAT] %s body_move=%d", ev == SQUAT_EV_DETECTED ? "蹲下" : "结束", data);
}

void on_wave_event(wave_event_t ev, int data, void* /*user*/) {
  log_warn("[RADAR/WAVE] %s events=%d", ev == WAVE_EV_DETECTED ? "挥手" : "结束", data);
}

void on_seat_event(seat_event_t ev, int data, void* /*user*/) {
  log_warn("[RADAR/SEAT] %s body_move=%d", ev == SEAT_EV_SEATED ? "入座" : "离座", data);
}

/* ──────────────────────────────────────────────────────────────────────
 * 任务
 * ──────────────────────────────────────────────────────────────────── */

/** 只排空、不喂解析器：WS 断开期间防止 RX ring 溢出。 */
void drain_discard(HardwareSerial& serial) {
  const int avail = serial.available();
  if (avail <= 0) {
    return;
  }
  const int n = (avail > kDrainMaxBytes) ? kDrainMaxBytes : avail;
  for (int i = 0; i < n; ++i) {
    if (serial.read() < 0) {
      break;
    }
  }
}

/** 从暂停恢复：丢弃断线期积压、重置解析器与各 FSM、清掉计时锚点。
 *  否则解析器会拿断线期间的陈旧字节续帧，排障计时也会立刻误报「无数据」。 */
void resume_from_pause(uint32_t now) {
  drain_discard(s_r60_serial);
  drain_discard(s_ld2450_serial);

  /* 解析器：重置帧状态机与 status 快照（回调要一并重挂） */
  r60_init(&s_r60, on_r60_frame, nullptr);
  ld2450_init(&s_ld2450, on_ld2450_frame, nullptr);

  /* FSM：断线期间的状态不应延续到新会话 */
  squat_init(&s_squat, nullptr, on_squat_event, nullptr);
  wave_init(&s_wave, nullptr, on_wave_event, nullptr);
  seat_init(&s_seat, nullptr, on_seat_event, nullptr);

  /* 计时锚点：全部重置到「此刻」，避免立刻触发无数据告警/摘要 */
  s_last_log_ms = now;
  s_last_seat_tick = now;
  s_r60_ok_ms = now;
  s_ld_ok_ms = now;
  s_r60_prev_ok = s_r60.frames_ok;
  s_ld_prev_ok = s_ld2450.frames_ok;
  s_r60_warned = false;
  s_ld_warned = false;
  /* 跳变检测的基准也重置：恢复后第一帧不该被当成「跳变」 */
  s_prev_presence = -1;
  s_prev_targets = -1;
  s_prev_bed = -1;
  s_prev_stage = -1;
}

void task_loop_radar(void* /*arg*/) {
  for (;;) {
    const uint32_t now = millis();

    /* 未连上 service 时完全静默：不解析、不检测、不输出日志。
     * UART 硬件照常收进 ring，这里只排空丢弃防止溢出。
     * 注意：初始化与任务创建仍在 setup() 早期完成（不受连接状态影响），
     * 否则首次连接失败就永远不会启动。这里只控制「运行期是否干活」。 */
    if (!ws_transport_ready()) {
      if (s_ws_active) {
        s_ws_active = false;
        log_warn("[RADAR] ws down -> 采集/检测/日志已暂停");
      }
      drain_discard(s_r60_serial);
      drain_discard(s_ld2450_serial);
      vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
      continue;
    }
    if (!s_ws_active) {
      s_ws_active = true;
      resume_from_pause(now);
      log_warn("[RADAR] ws up -> 采集/检测/日志已恢复");
    }

    drain_r60(now);
    drain_ld2450(now);
    update_liveness(now);

    /* 入座/离座是 tick 驱动（另有 squat/wave 在帧回调里事件驱动） */
    if ((uint32_t)(now - s_last_seat_tick) >= kSeatTickMs) {
      s_last_seat_tick = now;
      const r60_status_t* r = r60_status(&s_r60);
      seat_tick(&s_seat, r->body_move, r->presence, now);
    }

    log_events();
    log_summary(now);
    log_silent_diag(now);
    vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
  }
}

}  // namespace

/* ──────────────────────────────────────────────────────────────────────
 * 公开接口
 * ──────────────────────────────────────────────────────────────────── */

bool setup_radar() {
  if (s_ready) {
    return true;
  }

  /* 注册帧回调：每解析出一帧即分发给姿势检测 FSM。
   * 回调触发前 status 已按本帧更新完毕，回调里可直接读 r60_status()。 */
  r60_init(&s_r60, on_r60_frame, nullptr);
  ld2450_init(&s_ld2450, on_ld2450_frame, nullptr);

  /* 姿势检测 FSM：nullptr = 用默认配置（阈值见 squat_fsm.h / wave_fsm.h）。
   * 注意这些阈值是在上游毛毛虫的机械结构上标定的，本板安装高度/角度不同，
   * 可能需要实测重标定。 */
  squat_init(&s_squat, nullptr, on_squat_event, nullptr);
  wave_init(&s_wave, nullptr, on_wave_event, nullptr);
  seat_init(&s_seat, nullptr, on_seat_event, nullptr);

  /* RX ring 默认仅 256B。日志出口（USB CDC）在主机遇忙时可能阻塞较久，
   * 消费停顿期间靠 ring 兜住；提到 1024B 留足余量。必须在 begin() 之前设置。 */
  s_r60_serial.setRxBufferSize(1024);
  s_ld2450_serial.setRxBufferSize(1024);
  s_r60_serial.begin(RADAR_R60_BAUD, SERIAL_8N1, RADAR_R60_RX_PIN, RADAR_R60_TX_PIN);
  s_ld2450_serial.begin(RADAR_LD2450_BAUD, SERIAL_8N1, RADAR_LD2450_RX_PIN,
                        RADAR_LD2450_TX_PIN);

  const uint32_t now = millis();
  s_r60_ok_ms = now;
  s_ld_ok_ms = now;
  s_last_log_ms = now;
  s_ready = true;

  /* 就绪行带上实际引脚/波特率，便于和实物接线逐一核对。 */
  log_warn("[RADAR] R60ABD1 ready uart%d rx=G%d tx=G%d @%d", RADAR_R60_UART_NUM,
           RADAR_R60_RX_PIN, RADAR_R60_TX_PIN, RADAR_R60_BAUD);
  log_warn("[RADAR] LD2450  ready uart%d rx=G%d tx=G%d @%d", RADAR_LD2450_UART_NUM,
           RADAR_LD2450_RX_PIN, RADAR_LD2450_TX_PIN, RADAR_LD2450_BAUD);
  log_warn("[RADAR] FSM ready: squat(R60) + seat(R60) + wave(LD2450)");
  return true;
}

void task_setup_radar() {
  if (s_task) {
    return;
  }
  if (!s_ready) {
    log_warn("[RADAR] task_setup_radar skipped (setup_radar not ok)");
    return;
  }
  const BaseType_t rc = utils_task_create_pinned(task_loop_radar, "radar", kRadarTaskStack,
                                                 nullptr, kRadarTaskPrio, &s_task, APP_CPU_NUM);
  if (rc != pdPASS) {
    log_error("[RADAR] task create failed rc=%d", (int)rc);
    s_task = nullptr;
    return;
  }
  log_warn("[RADAR] task OK stack=%u prio=%u log_interval=%ums", (unsigned)kRadarTaskStack,
           (unsigned)kRadarTaskPrio, (unsigned)RADAR_LOG_INTERVAL_MS);
}
