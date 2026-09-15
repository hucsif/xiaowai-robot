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
