"""各设备最近一份雷达快照（进程内，供 LLM 的 ``get_heart_rate`` / ``get_breath_rate`` / ``get_radar_position`` 工具读取）。

数据来自设备 1Hz 上行的 ``radar_state``（固件侧见 ``radar/radar.h`` 与
``radar_config.h`` 的 ``DESKBOT_RADAR_UPLINK_ENABLE``）。

**为什么是模块级锁 + 普通 dict，而不是 DeviceWsService 的 ``_DeviceEntry``**：
读它的路径是同步的（工具执行器），而 ``_DeviceEntry`` 的访问要
``async with self._lock``，同步上下文 await 不了。这与
``face_snapshot_cache`` / ``voice_snapshot_cache`` 的选择一致。

三层保护各管一段：

1. **设备端**：心率/呼吸超 ``RADAR_VITAL_TTL_MS`` 未更新就不发该字段
   （服务端拿到的就是缺键）；
2. **本模块**：整帧超 ``RADAR_TTL_S`` 视为过期 → ``get_device_radar`` 返回 None；
3. **值域兜底**：明显越界的值当缺失（防固件 bug / 字段漂移）。
"""

from __future__ import annotations

import threading
import time
from typing import Any

# 整帧过期时间（秒）。设备 1Hz 上行 → 15s = 容忍连丢 14 帧：WiFi 偶发繁忙
# 不该让 LLM 看到「未知」来回抖动；又足够短，不会在人走开后还念旧方位。
RADAR_TTL_S = 15.0

# 值域兜底（R60ABD1 文档区间：心率 60~120、呼吸 0~35，两端各留余量）
_HEART_MIN, _HEART_MAX = 40, 140
_BREATH_MIN, _BREATH_MAX = 5, 35

# 方位可用的斜距区间（米）。太近 atan2 会剧烈跳变，太远超出 LD2450 的量程。
_MIN_DIST_M, _MAX_DIST_M = 0.1, 4.0

_lock = threading.Lock()
_snapshots: dict[str, dict[str, Any]] = {}
# 曾经上报过 radar_state 的设备（**不过期**）。用来决定 get_heart_rate / get_breath_rate / get_radar_position 工具
# 要不要注册 —— 没雷达硬件的设备不该看到一个注定查不出东西的工具。
_seen: set[str] = set()


def update_device_radar(device_id: str, payload: dict[str, Any]) -> None:
    """写入该设备的最新快照。纯内存写、无 await —— 可从 WS 上行分派里直接调。"""
    dev = str(device_id or "").strip()
    if not dev or not isinstance(payload, dict):
        return
    row = dict(payload)
    row["_mono"] = time.monotonic()  # TTL 判据用单调钟（wall-clock 可能跳）
    row["_ts"] = time.time()  # 展示/调试用
    with _lock:
        _snapshots[dev] = row
        _seen.add(dev)


def has_device_radar(device_id: str | None) -> bool:
    """该设备是否**曾经**上报过雷达数据。"""
    dev = str(device_id or "").strip()
    if not dev:
        return False
    with _lock:
        return dev in _seen


def get_device_radar(device_id: str | None, *, max_age_s: float = RADAR_TTL_S) -> dict[str, Any] | None:
    """最近一份雷达快照（已过值域兜底）。

    从未收到或已过期 → None，但**不删记录**：过期与「从未有过」必须区分 ——
    前者该告诉 LLM「现在读不到」，后者该让它根本不知道有这个能力。
    """
    dev = str(device_id or "").strip()
    if not dev:
        return None
    with _lock:
        row = _snapshots.get(dev)
    if not row:
        return None
    if (time.monotonic() - float(row.get("_mono") or 0.0)) > float(max_age_s):
        return None
    return _sanitize(row)


def clear_device(device_id: str | None) -> None:
    """清除该设备的快照与「见过」标记（设备清除云端数据时调用）。"""
    dev = str(device_id or "").strip()
    if not dev:
        return
    with _lock:
        _snapshots.pop(dev, None)
        _seen.discard(dev)


def _sanitize(row: dict[str, Any]) -> dict[str, Any]:
    """值域兜底：越界的值当缺失，不喂给 LLM。无有效字段时只返回 present。"""
    out: dict[str, Any] = {"present": bool(row.get("present"))}
    x_mm: int | None = None
    y_mm: int | None = None
    for key in ("x_mm", "y_mm"):
        try:
            val = int(row[key])
        except (KeyError, TypeError, ValueError):
            continue
        if key == "x_mm":
            x_mm = val
        else:
            y_mm = val
    if x_mm is not None and y_mm is not None and y_mm > 0:
        dist_m = ((x_mm / 1000.0) ** 2 + (y_mm / 1000.0) ** 2) ** 0.5
        if _MIN_DIST_M <= dist_m <= _MAX_DIST_M:
            out["x_mm"] = x_mm
            out["y_mm"] = y_mm
    for key, lo, hi in (
        ("heart_rate", _HEART_MIN, _HEART_MAX),
        ("breath_rate", _BREATH_MIN, _BREATH_MAX),
    ):
        try:
            val = int(row[key])
        except (KeyError, TypeError, ValueError):
            continue
        if lo <= val <= hi:
            out[key] = val
    return out
