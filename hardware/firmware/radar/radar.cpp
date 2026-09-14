#include "radar.h"

#include "ld2450.h"
#include "r60abd1.h"
#include "radar_config.h"

#include "logger.h"
#include "utils/utils.h"

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

HardwareSerial s_r60_serial(RADAR_R60_UART_NUM);
HardwareSerial s_ld2450_serial(RADAR_LD2450_UART_NUM);

r60_parser_t    s_r60;
ld2450_parser_t s_ld2450;

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
  log_warn("[RADAR/R60] ok=%u bad=%u | presence=%d motion=%u move=%u pos=(x=%d,y=%d)cm "
           "breath=%u heart=%u bed=%d stage=%u",
           (unsigned)s_r60.frames_ok, (unsigned)s_r60.frames_bad, r->presence ? 1 : 0,
           (unsigned)r->motion, (unsigned)r->body_move, (int)r->x_cm, (int)r->y_cm,
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
 * 任务
 * ──────────────────────────────────────────────────────────────────── */

void task_loop_radar(void* /*arg*/) {
  for (;;) {
    const uint32_t now = millis();
    drain_r60(now);
    drain_ld2450(now);
    update_liveness(now);
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

  r60_init(&s_r60, nullptr, nullptr);
  ld2450_init(&s_ld2450, nullptr, nullptr);

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
