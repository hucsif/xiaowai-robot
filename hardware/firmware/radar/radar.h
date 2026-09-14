#pragma once

#include "radar_config.h"

/* 雷达采集模块对外接口（R60ABD1 60GHz 呼吸睡眠 + LD2450 24GHz 运动追踪）。
 *
 * 本模块只做「采集 + 解析 + 日志」：初始化两路 UART，把原始字节喂给两个
 * 纯 C 解析器，并按周期把解析结果打到串口，用于验证接线与观察实时数据。
 * 不含姿态/睡眠等业务状态机，也不做任何网络上报。
 *
 * 日志级别说明：本固件运行期日志级别固定为 WARN（log_info 是空操作），
 * 因此这里所有输出都用 log_warn，标签为 [RADAR] / [RADAR/R60] / [RADAR/LD2450]。
 */

/** 初始化两路雷达 UART + 解析器（幂等）。
 *  @return 始终 true（UART begin 无失败返回；接线问题由日志的排障提示暴露）。
 *          失败或未就绪时切勿调用 task_setup_radar()。 */
bool setup_radar();

/** 创建雷达采集任务（幂等）：10ms 轮询两路 UART，按 RADAR_LOG_INTERVAL_MS 输出摘要。 */
void task_setup_radar();
