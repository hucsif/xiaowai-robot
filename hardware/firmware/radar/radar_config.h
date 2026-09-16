#pragma once

/* ========== 雷达模块配置（自包含：引脚 / UART / 波特率 / 开关）==========
 * 两种雷达都是「只上报」的 UART 外设：上电即自动输出数据帧，无需下发配置命令。
 * 引脚沿用本板已禁用的摄像头引脚（DESKBOT_HAS_CAMERA=0 后空出，全项目无其他占用）。
 *
 * 本文件刻意不并入 deskbot_config.h：雷达模块整体自包含，
 * 方便整目录搬移/删除而不污染主配置。
 */

/* 总开关：置 0 可完全关闭雷达模块（不初始化 UART、不建任务、不输出日志）。
 * 置 1 = 开启。代码本身始终参与编译，此开关只控制运行期是否启动。
 * 当前为 1：雷达已接线，正常运行。 */
#ifndef DESKBOT_RADAR_ENABLE
#define DESKBOT_RADAR_ENABLE 1
#endif

/* ---- R60ABD1 云帆瑞达 60GHz 呼吸/睡眠雷达 ----
 * 60GHz，上报人体存在/运动/体动幅度/3D坐标/呼吸率/心率/在床/睡眠分期。
 * UART2 + GPIO 矩阵重映射到 G9/G10：UART2 在 ESP32-S3 上默认引脚是 G17/G18，
 * 与本板 LD2450 冲突，故必须重映射（HardwareSerial::begin 传显式引脚即自动走 GPIO 矩阵）。
 * 方向：模块 TX -> GPIO10(ESP RX)；模块 RX <- GPIO9(ESP TX)。
 * 帧格式：SOF 53 59 | ctl | cmd | len(2B 大端) | data | checksum | EOF 54 43 */
#define RADAR_R60_UART_NUM 2
/* 本板丝印约定：GPIO9=TX、GPIO10=RX（从 ESP32 视角）。
 * 故 ESP 的 RX 是 GPIO10 —— 模块的 TX 要接到这里。 */
#define RADAR_R60_RX_PIN   10
#define RADAR_R60_TX_PIN   9
#define RADAR_R60_BAUD     115200

/* ---- LD2450 海凌科 24GHz 运动目标追踪雷达 ----
 * 24GHz，出厂设置即 10Hz 自动上报，最多 3 个目标的 x/y 坐标与速度。
 * UART1 + G17/G18。
 * 方向：模块 TX -> GPIO18(ESP RX)；模块 RX <- GPIO17(ESP TX)。
 * 帧格式：固定 30B，SOF AA FF 03 00 | T1(8B) T2(8B) T3(8B) | EOF 55 CC，小端、无校验和 */
#define RADAR_LD2450_UART_NUM 1
/* 本板丝印约定：GPIO17=TX、GPIO18=RX（从 ESP32 视角）。
 * 故 ESP 的 RX 是 GPIO18 —— 模块的 TX 要接到这里。 */
#define RADAR_LD2450_RX_PIN   18
#define RADAR_LD2450_TX_PIN   17
#define RADAR_LD2450_BAUD     256000

/* 周期摘要日志间隔（ms）；置 0 关闭周期日志（仅保留事件与排障日志）。 */
#ifndef RADAR_LOG_INTERVAL_MS
#define RADAR_LOG_INTERVAL_MS 1000
#endif

/* 某路连续多久无有效帧就打一次排障提示（ms）。 */
#ifndef RADAR_SILENT_WARN_MS
#define RADAR_SILENT_WARN_MS 5000
#endif

/* ========== 看人转头（说话时转向说话人，看不见人了回中）==========
 *
 * 【转向】触发来自服务端：ASR 识别成功（已过 VAD 切句 + 注意力门控，噪音与
 * 闲聊都滤掉了）时下发 HEAD_SERVO_LOOK 模式舵机帧，设备按 LD2450 目标算角度。
 * 为什么不用 ESP-SR VAD 本地触发（试过，已弃用）：
 *   - VAD 是「音量」判据 → 噪音/电视会误触
 *   - VAD 只在 silence→speech 跳变时产生事件 → 话说快了（间隔 <256ms 确认窗）
 *     就没有新事件 → 漏触发（表现为「时灵时不灵」）
 * 服务端的 ASR 成功事件两个毛病都没有，语义上等价于上游的唤醒词。
 *
 * 【回中】用 LD2450「看不见人了」判据，**不用 R60 的 presence**：
 *   R60 报「有人→无人」要 ~40s 才上报 → 回中会拖到下一次对话时才落地，
 *   和新的 LOOK 抢同一个舵机（实测表现为「有时只回正不转向」「有时转后不回正」）。
 *   LD2450 是 10Hz，反应快，且语义一致 —— 看不见人 → 看回正前方。
 *
 * 纯本地功能，刻意放在 ws_transport_ready() 门控【之前】—— 断网时依然可用。
 */

/* 总开关：置 0 关闭。 */
#ifndef DESKBOT_LOOK_ENABLE
#define DESKBOT_LOOK_ENABLE 1
#endif

/* 平滑转动时长（ms）。400ms 转满 180° ≈ 450°/s，偏快；舵机有噪声就调大。 */
#ifndef DESKBOT_LOOK_MS
#define DESKBOT_LOOK_MS 400
#endif

/* 方位角 → 舵机角度增益。1.0 = 雷达转 1° 舵机也转 1°。
 * ⚠️ x_mm 的符号方向取决于雷达实际装配朝向：若实测「人在左边却往右转」，
 *    改成 -1.0f 重新烧录即可。 */
#ifndef DESKBOT_LOOK_GAIN
#define DESKBOT_LOOK_GAIN 1.0f
#endif

/* 回中判据（ms）：LD2450 连续这么久看不到任何目标 → 平滑转回中位。
 * 调大 = 更不容易「人还坐着但雷达暂时丢了」就转回去；调小 = 回中更快。
 * 置 0 = 关闭自动回中（转过头就一直停在那个角度，直到下次说话）。 */
#ifndef DESKBOT_LOOK_RECENTER_QUIET_MS
#define DESKBOT_LOOK_RECENTER_QUIET_MS 8000
#endif

/* 目标距离筛选（mm）：超出 MAX 视为无目标（与摘要日志同口径）；
 * 小于 MIN 视为贴脸噪声 —— 近距离时 atan2 会剧烈跳变。 */
#ifndef DESKBOT_LOOK_MIN_DIST_MM
#define DESKBOT_LOOK_MIN_DIST_MM 200
#endif
#ifndef DESKBOT_LOOK_MAX_DIST_MM
#define DESKBOT_LOOK_MAX_DIST_MM 3000
#endif

/* ========== 雷达状态上行（radar_state → 服务端 LLM 工具）==========
 * 按 RADAR_UPLINK_INTERVAL_MS 把 radar_snapshot() 打成一条 JSON 交给
 * ws_transport_enqueue_state()（走主连接 /asr_chat）。服务端只把它写进按设备
 * 的缓存，LLM 调用 get_heart_rate / get_breath_rate / get_radar_position 工具时读出来 —— 上行本身【不触发任何
 * 对话轮次】，与 pb_ack 同构（写缓存后 continue）。
 *
 * 隐私：心率属健康类敏感信息，开了这个开关就意味着它离开设备、进入服务端内存
 * 与第三方 LLM 的上下文。不想给就置 0。
 */

/* 总开关：置 0 = 完全不上行（服务端也就不会注册 get_heart_rate / get_breath_rate / get_radar_position 工具）。 */
#ifndef DESKBOT_RADAR_UPLINK_ENABLE
#define DESKBOT_RADAR_UPLINK_ENABLE 1
#endif

/* 上报周期（ms）。1Hz 约 140 B/s，带宽可忽略。
 * ⚠️ 勿为了「更实时」提到 10Hz —— 上行与音频共用同一条 32 深的 TX 队列
 *    （ws_transport.cpp），满即丢弃且无优先级，10Hz 会真去挤音频。 */
#ifndef RADAR_UPLINK_INTERVAL_MS
#define RADAR_UPLINK_INTERVAL_MS 1000
#endif

/* 心率/呼吸的新鲜度上限（ms）。R60 是 3 秒一帧，容忍连续几次没更新；
 * 超时后该字段从 JSON 里【整个省略】（而不是发 0 —— 0 在协议里就是「无效」）。 */
#ifndef RADAR_VITAL_TTL_MS
#define RADAR_VITAL_TTL_MS 12000
#endif

/* 方位窗口（mm）。刻意与 DESKBOT_LOOK_* 同口径 —— 让「LLM 说的方位」与
 * 「头实际转过去的方向」取同一个目标，两处不一致会显得精神分裂。 */
#ifndef DESKBOT_RADAR_MIN_DIST_MM
#define DESKBOT_RADAR_MIN_DIST_MM 200
#endif
#ifndef DESKBOT_RADAR_MAX_DIST_MM
#define DESKBOT_RADAR_MAX_DIST_MM 3000
#endif

/* 横向坐标符号标定：把线上契约固定为「负 = 机器人左侧」。
 * ⚠️ 必须实测一次 —— LD2450 数据手册说 x 左负右正，但实际装配朝向可能相反
 *    （DESKBOT_LOOK_GAIN 就是为同一件事留的 -1.0f）。
 *    标定：人站到机器人【左侧】，看串口上行日志里的 x_mm；
 *          若 x_mm > 0（报成了右侧），把本宏改成 -1 重烧。
 *    转头方向反了 → 改 DESKBOT_LOOK_GAIN；方位说反了 → 改这个。两件事同源。 */
#ifndef DESKBOT_RADAR_X_SIGN
#define DESKBOT_RADAR_X_SIGN 1
#endif
