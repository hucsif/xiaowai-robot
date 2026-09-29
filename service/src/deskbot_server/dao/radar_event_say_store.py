"""雷达动作事件台词的开关与文案（``data/radar_event_say.json``）。

**这个文件就是给使用者改的**：单独开关「挥手 / 入座 / 离座」三句台词，也能改词。

- 文件**不存在**时首次读取会自动按下面 ``DEFAULT_LINES`` 生成一份，改它即可；
- 文件**损坏**（JSON 解析失败）或某一项缺失 → 该项回默认值，不影响另外两项；
- 每次事件都**现读**（事件很稀疏，一次磁盘读可忽略）—— 改完不用重启服务。

```json
{
  "wave":   { "enabled": true, "text": "你好呀！" },
  "seated": { "enabled": true, "text": "你回来啦～" },
  "away":   { "enabled": true, "text": "那我先歇会儿，需要我就叫我。" }
}
```

与 ``.env`` 的 ``RADAR_EVENT_SAY_ENABLED`` 是**两级开关**：那是总闸（整块关掉），
这里是逐项开关。两个都开才会说话。
"""

from __future__ import annotations

import logging
import os
from typing import Any

from deskbot_server.constants import RADAR_EVENT_SAY_FILE
from deskbot_server.dao._json_store import load_json_file, save_json_file

logger = logging.getLogger("deskbot-server")

# 三类事件的默认台词与开关。**改文案请改 data/radar_event_say.json，不是这里** ——
# 这里只是「文件不存在时用来生成初始内容」以及「某项缺失时的回退值」。
DEFAULT_LINES: dict[str, dict[str, Any]] = {
    "wave": {"enabled": True, "text": "你好呀！"},
    "seated": {"enabled": True, "text": "你回来啦～"},
    "away": {"enabled": True, "text": "那我先歇会儿，需要我就叫我。"},
}


def _as_bool(value: Any, default: bool) -> bool:
    """宽松解析布尔：真 bool 直接用；字符串按 ``0/false/no/off`` 认假（与 .env 同规则）。"""
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() not in ("0", "false", "no", "off")
    if isinstance(value, (int, float)):
        return bool(value)
    return default


def normalize_radar_event_say_cfg(raw: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    """归一化：逐项补默认值。未知键忽略（允许文件里留注释性的额外字段）。"""
    src = raw if isinstance(raw, dict) else {}
    out: dict[str, dict[str, Any]] = {}
    for event, dflt in DEFAULT_LINES.items():
        entry = src.get(event)
        if not isinstance(entry, dict):
            # 缺这一项 → 整项用默认（含开关与文案），另外两项不受影响
            out[event] = dict(dflt)
            continue
        text = str(entry.get("text", dflt["text"]) or "").strip()
        out[event] = {
            "enabled": _as_bool(entry.get("enabled"), bool(dflt["enabled"])),
            # 文案被清空 → 回默认，避免出现「开着但没话说」的半残状态
            "text": text or str(dflt["text"]),
        }
    return out


def load_radar_event_say_cfg(*, seed: bool = True) -> dict[str, dict[str, Any]]:
    """读配置；文件不存在时（``seed=True``）先写出一份默认文件再返回。"""
    path = RADAR_EVENT_SAY_FILE
    raw = load_json_file(path, default=None)
    if raw is None:
        cfg = normalize_radar_event_say_cfg(None)
        if seed and not os.path.isfile(path):
            try:
                save_json_file(path, cfg)
                logger.info(
                    "[radar_event] 已生成默认配置 %s —— 改它即可逐项开关/改词（改完不用重启）", path
                )
            except OSError:
                logger.warning("[radar_event] 生成默认配置失败 path=%s", path, exc_info=True)
        return cfg
    return normalize_radar_event_say_cfg(raw)


def event_line(event: str, *, cfg: dict[str, dict[str, Any]] | None = None) -> str | None:
    """取该事件要说的台词；未开启/未知事件 → ``None``（= 不要说）。"""
    key = str(event or "").strip()
    table = cfg if cfg is not None else load_radar_event_say_cfg()
    entry = table.get(key)
    if not entry or not entry.get("enabled"):
        return None
    text = str(entry.get("text") or "").strip()
    return text or None
