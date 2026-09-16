"""执行 LLM JSON 中的 ``tools`` 指令。"""

from __future__ import annotations

import logging
import math
from typing import Any

from deskbot_server.dao.device_memory_mapper import add_memory, delete_memory
from deskbot_server.dao.device_session_mapper import execute_session_tool
from deskbot_server.dao.user_social_store import append_daily_task_line, append_user_info_line
from deskbot_server.service.application.face_registration import register_face_for_device
from deskbot_server.service.application.radar_snapshot_cache import get_device_radar
from deskbot_server.service.application.voice_registration import register_voice_for_device
from deskbot_server.service.camera_face_service import capture_camera_for_device_async
from deskbot_server.service.miot_tools import execute_miot_tool
from deskbot_server.service.quest_service import QuestService
from deskbot_server.service.scheduled_task_service import execute_schedule_task_tool
from deskbot_server.service.web_tools import webfetch, websearch

logger = logging.getLogger("deskbot-server")

# 工具参数键名容错（模型常把 user_name/chat_message/reason 写成近似键）：
# 前者按序取第一个非空，后者用于识别「身份键」以把剩余事实键拼成可读行
_NAME_KEYS = ("user_name", "person_name", "user", "person", "name")
_MESSAGE_KEYS = ("chat_message", "message", "content", "text", "msg")
_REASON_KEYS = ("reason", "result", "cause", "note", "description", "text", "msg")
_IDENT_KEYS = frozenset({"tool", "type", "function", "id", "arguments", "tool_call_id"}) | set(_NAME_KEYS) | set(_MESSAGE_KEYS)


# 雷达感知工具：工具名 → 快照里对应的字段键（方位工具字段不止一个，走单独分支）。
# 三个工具各自**只返回一项** —— 合并返回时实测模型会「问呼吸却报心率」，
# 详见 tool_schema._radar_schemas 的说明。
_RADAR_TOOLS: dict[str, str] = {
    "get_heart_rate": "heart_rate",
    "get_breath_rate": "breath_rate",
    "get_radar_position": "",  # 单独分支，不走 key 映射
}
_RADAR_TOOLS_CN: dict[str, str] = {
    "get_heart_rate": "心率",
    "get_breath_rate": "呼吸率",
    "get_radar_position": "方位",
}

# 方位措辞的角度分档（与固件 look_angle_provider 用同一套 atan2(|x|, y) 几何）
_RADAR_FRONT_DEG = 15.0
_RADAR_SIDE_DEG = 45.0


def _radar_where_text(x_mm: Any, y_mm: Any) -> str | None:
    """``(x_mm, y_mm)`` → 「正前方 / 左前方 / 左侧」；坐标不可用返回 None。

    线上契约：**x_mm 为负 = 机器人左侧**（固件已按 ``DESKBOT_RADAR_X_SIGN``
    规范化后才上行）。若实测左右说反了，去改固件那个宏，**不要在这里翻** ——
    否则线上契约就变成两处各自为政了。
    """
    try:
        x = int(x_mm)
        y = int(y_mm)
    except (TypeError, ValueError):
        return None
    if y <= 0:
        return None  # 目标在正侧/身后：此时 atan2 会给出无意义的大角，不猜
    ang = math.degrees(math.atan2(abs(x), y))
    if ang <= _RADAR_FRONT_DEG:
        return "正前方"
    side = "左" if x < 0 else "右"
    return f"{side}前方" if ang <= _RADAR_SIDE_DEG else f"{side}侧"


def _require_quest_playbook(device_id: str) -> str:
    """解析设备绑定剧本（playbook 参数从 devices.quest_id 来，LLM 可不传）。"""
    playbook = QuestService().get_bound_playbook(device_id)
    if not playbook:
        raise ValueError("设备未绑定剧情剧本，任务工具不可用")
    return playbook


async def execute_llm_tools(
    tools: list[dict[str, Any]],
    *,
    device_id: str | None = None,
    session_id: str | None = None,
    device_ws: Any = None,
) -> list[dict[str, Any]]:
    """逐条执行工具，返回结果摘要（供日志与 pipeline 事件）。"""
    results: list[dict[str, Any]] = []
    dev = str(device_id or "").strip()
    for raw in tools or []:
        if not isinstance(raw, dict):
            continue
        tool = str(raw.get("tool") or raw.get("name") or "").strip()
        if not tool:
            continue
        try:
            if tool == "say":
                # 过渡语工具：本身不下发、不阻塞，只把要说的文本回传调用方
                # （chat_flow 取 reply 后播报）。哨兵形态与普通工具一致，避免模型
                # 看出「说了但没生效」而重试。单独调用不产生任何动作（防走神），
                # 返回错误让模型下一轮补上真正的工具调用。
                if not any(
                    str(t.get("tool") or t.get("name") or "").strip().lower() not in ("", "say")
                    for t in tools
                    if isinstance(t, dict)
                ):
                    results.append({"tool": "say", "ok": False, "error": "单独调用 say 没有效果，请同时调用要执行的工具"})
                else:
                    results.append({"tool": "say", "ok": True, "reply": str(raw.get("text") or "")})
            elif tool == "register_face":
                name = str(raw.get("name") or raw.get("person_name") or "").strip()
                fid_raw = raw.get("face_id")
                face_id = int(fid_raw) if fid_raw is not None else None
                out = register_face_for_device(dev, name, face_id=face_id)
                results.append(
                    {
                        "tool": tool,
                        "ok": True,
                        "profile_id": out["profile"].get("id"),
                        "name": out["profile"].get("name"),
                        "face_id": out.get("face_id"),
                    }
                )
            elif tool == "register_voiceprint":
                name = str(raw.get("name") or raw.get("person_name") or "").strip()
                out = register_voice_for_device(dev, name)
                results.append(
                    {
                        "tool": tool,
                        "ok": True,
                        "profile_id": out["profile"].get("id"),
                        "name": out["profile"].get("name"),
                    }
                )
                cap = await capture_camera_for_device_async(dev, hub=device_ws)
                if not cap.get("ok"):
                    results.append({"tool": tool, "ok": False, "error": cap.get("error")})
                else:
                    results.append({"tool": tool, **cap})
            elif tool == "update_user_info":
                name = next((str(raw.get(k) or "").strip() for k in _NAME_KEYS if str(raw.get(k) or "").strip()), "")
                message = next(
                    (str(raw.get(k) or "").strip() for k in _MESSAGE_KEYS if str(raw.get(k) or "").strip()), ""
                )
                if not message:
                    # 模型把事实拆成散键（location/age/hobby…）时拼成可读行归档
                    facts = [f"{k}: {v}" for k, v in raw.items() if k not in _IDENT_KEYS and str(v or "").strip()]
                    message = "，".join(facts)
                if not name:
                    raise ValueError("update_user_info 需要 user_name")
                if not message:
                    raise ValueError("update_user_info 需要 chat_message")
                if not dev:
                    raise ValueError("update_user_info 需要 device_id")
                out = append_user_info_line(dev, name, message)
                results.append({"tool": tool, "ok": True, "user_name": name, **out})
            elif tool == "update_daily_task":
                name = next((str(raw.get(k) or "").strip() for k in _NAME_KEYS if str(raw.get(k) or "").strip()), "")
                message = next(
                    (str(raw.get(k) or "").strip() for k in _MESSAGE_KEYS if str(raw.get(k) or "").strip()), ""
                )
                if not name:
                    raise ValueError("update_daily_task 需要 user_name")
                if not message:
                    raise ValueError("update_daily_task 需要 message")
                if not dev:
                    raise ValueError("update_daily_task 需要 device_id")
                out = append_daily_task_line(dev, name, message)
                results.append({"tool": tool, "ok": True, "user_name": name, **out})
            elif tool == "memory_add":
                text = str(raw.get("text") or raw.get("value") or "").strip()
                if not text:
                    raise ValueError("memory_add 需要 text")
                entry = add_memory(text, device_id=dev or None)
                results.append({"tool": tool, "ok": True, "id": entry["id"], "text": entry["text"]})
            elif tool == "memory_delete":
                eid = str(raw.get("id") or "").strip()
                if not eid:
                    raise ValueError("memory_delete 需要 id")
                ok = delete_memory(eid, device_id=dev or None)
                if not ok:
                    raise ValueError(f"未找到记忆 id={eid}")
                results.append({"tool": tool, "ok": True, "id": eid})
            elif tool in ("schedule_task", "scheduled_task"):
                out = execute_schedule_task_tool(raw, device_id=dev, default_session_id=session_id)
                results.append(out)
            elif tool == "session":
                out = execute_session_tool(raw, device_id=dev)
                results.append(out)
            elif tool in _RADAR_TOOLS:
                # 三个工具**刻意各自只返回一项**，让「用户问了什么」决定上下文里
                # 有什么 —— 模型既没法顺带报、也没法拿近邻数值顶替（见
                # tool_schema._radar_schemas 的说明与那里记录的两次实测）。
                if not dev:
                    raise ValueError(f"{tool} 需要 device_id")
                snap = get_device_radar(dev)
                if snap is None:
                    results.append(
                        {"tool": tool, "ok": False, "error": "雷达数据已过期或不可用（设备可能离线）"}
                    )
                elif tool == "get_radar_position":
                    row: dict[str, Any] = {"tool": tool, "ok": True, "present": bool(snap.get("present"))}
                    where = _radar_where_text(snap.get("x_mm"), snap.get("y_mm"))
                    if where:
                        row["where"] = where
                        row["distance_m"] = round(
                            math.hypot(snap["x_mm"], snap["y_mm"]) / 1000.0, 1
                        )
                    results.append(row)
                else:
                    # 生命体征与方位无关：LD2450 是运动追踪雷达，人坐得极静时可能
                    # 丢目标，而 R60 的生命体征仍然有效 —— 所以不看 present。
                    # 这个 dict 会被 chat_flow 用 json.dumps 原样塞进 role="tool"
                    # 消息 —— 就是 LLM 看到的原文，字段名要自解释。
                    key = _RADAR_TOOLS[tool]
                    row = {"tool": tool, "ok": True}
                    if snap.get(key):
                        row[key] = snap[key]
                    else:
                        # 显式说明「测不到」，别只留个空对象 —— 否则模型容易拿
                        # 上下文里的另一个数值来凑答案。
                        row["note"] = f"当前测不到{_RADAR_TOOLS_CN[tool]}，请如实告诉用户"
                    results.append(row)
            elif tool == "complete_task":
                playbook = _require_quest_playbook(dev)
                task_id = str(raw.get("task_id") or "").strip()
                if not task_id:
                    raise ValueError("complete_task 需要 task_id")
                user = next(
                    (str(raw.get(k) or "").strip() for k in _NAME_KEYS if str(raw.get(k) or "").strip()), ""
                )
                reason = next(
                    (str(raw.get(k) or "").strip() for k in _REASON_KEYS if str(raw.get(k) or "").strip()), ""
                )
                out = QuestService().complete_task(dev, playbook, task_id, user=user, reason=reason)
                results.append({"tool": tool, "ok": True, **out})
            elif tool in ("miot", "mihome", "mijia"):
                if not dev:
                    raise ValueError("miot 需要 device_id")
                out = execute_miot_tool(raw, device_id=dev)
                results.append(out)
            elif tool == "webfetch":
                url = str(raw.get("url") or "").strip()
                out = webfetch(url)
                results.append({"tool": tool, **out})
            elif tool == "websearch":
                query = str(raw.get("query") or raw.get("q") or "").strip()
                max_results = raw.get("max_results") or raw.get("limit") or 5
                out = websearch(query, max_results=int(max_results))
                results.append({"tool": tool, **out})
            else:
                results.append({"tool": tool, "ok": False, "error": f"未知工具: {tool}"})
        except Exception as exc:
            logger.warning("[LLM tools] %s 失败: %s", tool, exc)
            results.append({"tool": tool, "ok": False, "error": str(exc)})
    return results
