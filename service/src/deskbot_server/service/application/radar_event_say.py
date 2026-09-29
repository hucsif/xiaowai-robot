"""雷达动作事件 → 说一句固定台词（挥手 / 入座 / 离座）。

**刻意不过 LLM**：这三句是固定场景的寒暄，走 `run_chat_turn` 要多一次 LLM 往返
（几百 ms ~ 几秒），还可能被模型说成「我检测到你入座了」这类机器人腔。
这里直接 TTS 合成 → 组 pb → 下发，与聊天回复**共用同一条播放链路**
（`PbService.build_pb_seq_from_tts`），只是跳过了 LLM 与那套轮次编排。

事件来自固件的 `radar_event` 上行（见 `firmware/radar/radar.cpp` 的
`uplink_radar_event`）：`wave` / `seated` / `away`。触发者见 `wave_fsm.c`
（LD2450 挥手）与 `seat_fsm.c`（R60 入座/离座）。

这一路仍然**可被用户说话打断**：下发时带上当前轮代号（`turn_epoch`），
用户一开口新语音轮就把代号 +1，本条未播完的 pb 会被 `device_ws.send` 丢弃 ——
与主动轮的处理方式一致。
"""

from __future__ import annotations

import logging
import time
import uuid
from typing import Any

from deskbot_server.constants import RADAR_EVENT_SAY_COOLDOWN_SEC, RADAR_EVENT_SAY_ENABLED
from deskbot_server.dao.radar_event_say_store import event_line
from deskbot_server.pb.shapes import PB_ACTION_REPLACE, PB_LEVEL_TASK
from deskbot_server.service.pb_service import PbService
from deskbot_server.service.tts_service import TtsService

logger = logging.getLogger("deskbot-server")

# 台词与逐项开关在 data/radar_event_say.json（首次运行自动生成）。
# 这里只管「什么时候说」——说什么、说不说，由那个文件决定。
# 环境变量 RADAR_EVENT_SAY_ENABLED 是总闸，与文件里的逐项开关是两级关系。

# 按 (设备, 事件) 记录上次尝试开口的时刻（单调钟）。进程内，重启清零。
_last_say: dict[tuple[str, str], float] = {}


def _clear_throttle(device_id: str | None = None) -> None:
    """清节流记录。device_id 为空则全清（测试与「清除设备数据」用）。"""
    if not device_id:
        _last_say.clear()
        return
    dev = str(device_id).strip()
    for key in [k for k in _last_say if k[0] == dev]:
        _last_say.pop(key, None)


async def say_radar_event_line(
    device_ws: Any,
    device_id: str | None,
    event: str | None,
    *,
    chat: Any,
    bus_service: Any | None = None,
) -> bool:
    """对一次雷达动作事件说固定台词。返回是否真的下发了。

    ``chat``：`ChatService`（取 ``tts_cfg`` 与设备级 TTS 路由）。
    ``device_ws``：`DeviceWsService`（取连接、下发、轮代号）。
    """
    if not RADAR_EVENT_SAY_ENABLED:
        return False
    dev = str(device_id or "").strip()
    # 现读配置：文件里该项没开（或事件未知）→ 不说。
    text = event_line(event)
    if not dev or not text:
        return False

    ws = device_ws._get_ws(dev)
    if ws is None:
        logger.debug("[radar_event] 设备离线，跳过 event=%s device_id=%s", event, dev)
        return False

    now = time.monotonic()
    last = _last_say.get((dev, str(event)))
    if last is not None and (now - last) < RADAR_EVENT_SAY_COOLDOWN_SEC:
        logger.debug(
            "[radar_event] 节流跳过（距上次 %.1fs < %.1fs）event=%s device_id=%s",
            now - last,
            RADAR_EVENT_SAY_COOLDOWN_SEC,
            event,
            dev,
        )
        return False
    # 在**尝试之前**打点：TTS 偶发失败时也不要在冷却窗内反复重试。
    _last_say[(dev, str(event))] = now

    req_id = uuid.uuid4().hex[:16]
    try:
        sr, segs = await TtsService().synthesize_phoneme_segments(text, device_id=dev)
    except Exception:
        logger.warning("[radar_event] TTS 合成失败 event=%s device_id=%s", event, dev, exc_info=True)
        return False
    if not segs:
        logger.warning("[radar_event] TTS 无分片 event=%s device_id=%s text=%r", event, dev, text)
        return False

    pb_seq = PbService().build_pb_seq_from_tts(
        segs,
        chat.tts_cfg,
        request_id=req_id,
        device_id=dev,
        level=PB_LEVEL_TASK,
        action=PB_ACTION_REPLACE,
        sample_rate=sr,
    )
    if pb_seq is None:
        logger.warning("[radar_event] 组 pb 失败 event=%s device_id=%s", event, dev)
        return False

    delivered = await device_ws.send(dev, pb_seq, turn_epoch=device_ws.current_turn_epoch(dev))
    logger.info(
        "[radar_event] %s event=%s device_id=%s req=%s delivered=%d text=%r",
        "已下发" if delivered else "未下发（被新语音轮打断或链路异常）",
        event,
        dev,
        req_id,
        delivered,
        text,
    )
    if delivered and bus_service is not None:
        await bus_service.publish_auto_dispatch(
            dev,
            request_id=req_id,
            source="auto_radar_event",
            summary=f"雷达事件寒暄：{event}",
            status="ok",
        )
    return delivered > 0
