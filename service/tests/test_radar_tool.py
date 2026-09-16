"""雷达感知工具：按设备能力注册 + 每个工具只返回一件事 + 方位措辞分档。

两条核心约束：

1. **按设备能力注册** —— 没接过雷达的设备不该看到一堆注定查不出东西的工具，
   否则存量设备的 prompt 会被凭空多出三个工具；
2. **每个工具只返回自己那一项**（心率工具不含呼吸、方位工具不含生命体征）——
   这是「问什么答什么」的结构性保证：没被问到的数据根本不进上下文，模型既没法
   顺带报、也没法挑错。合并返回时实测过两轮：一个工具返回全部字段 → 模型把数值
   一起念出来；缩成「心率+呼吸」两个字段 → 模型**问呼吸却报心率**。
"""

from __future__ import annotations

import asyncio

import pytest

from deskbot_server.infrastructure.llm.tool_schema import (
    NATIVE_TOOL_NAMES_BATCH1,
    build_native_tool_schemas,
)
from deskbot_server.service.application.llm_tool_runner import _radar_where_text, execute_llm_tools
from deskbot_server.service.application.radar_snapshot_cache import (
    clear_device,
    get_device_radar,
    has_device_radar,
    update_device_radar,
)

_DEV = "dev_radar_test"
_RADAR_TOOLS = ("get_heart_rate", "get_breath_rate", "get_radar_position")


def _names(**kw: object) -> list[str]:
    return [s["function"]["name"] for s in build_native_tool_schemas(**kw)]  # type: ignore[arg-type]


def _run(tool: str, *, device_id: str | None = _DEV) -> dict:
    return asyncio.run(execute_llm_tools([{"tool": tool}], device_id=device_id))[0]


@pytest.fixture(autouse=True)
def _clean_cache():
    clear_device(_DEV)
    yield
    clear_device(_DEV)


# ───────────────────── schema 注册 ─────────────────────

def test_tools_absent_without_radar():
    """没上报过 radar_state 的设备不注册任何雷达工具。"""
    assert has_device_radar(_DEV) is False
    names = _names(device_id=_DEV)
    assert not any(t in names for t in _RADAR_TOOLS)


def test_tools_present_after_uplink_keeps_batch1_prefix_and_say_last():
    """上报过之后才出现，且不破坏 batch1 前缀与 say 末位（回归保护）。"""
    update_device_radar(_DEV, {"type": "radar_state", "present": True, "x_mm": -450, "y_mm": 1000})
    names = _names(device_id=_DEV, is_tool_round=True)
    assert all(t in names for t in _RADAR_TOOLS)
    assert names[:6] == NATIVE_TOOL_NAMES_BATCH1
    assert names[-1] == "say"


def test_tools_absent_when_batch2_disabled():
    """关掉 batch2 时名单必须恰好等于 batch1（既有测试也在盯这条）。"""
    update_device_radar(_DEV, {"type": "radar_state", "present": True, "x_mm": -450, "y_mm": 1000})
    assert _names(device_id=_DEV, include_batch2=False) == list(NATIVE_TOOL_NAMES_BATCH1)


def test_schemas_are_zero_argument():
    update_device_radar(_DEV, {"type": "radar_state", "present": True, "x_mm": -450, "y_mm": 1000})
    got = {
        s["function"]["name"]: s["function"]
        for s in build_native_tool_schemas(device_id=_DEV)
        if s["function"]["name"] in _RADAR_TOOLS
    }
    assert set(got) == set(_RADAR_TOOLS)
    for fn in got.values():
        assert fn["parameters"]["required"] == []
        assert fn["parameters"]["properties"] == {}
        assert "必须调用" in fn["description"]
    # 两个生命体征工具必须互相点名，堵住「拿近邻数值顶替」的路
    assert "get_breath_rate" in got["get_heart_rate"]["description"]
    assert "get_heart_rate" in got["get_breath_rate"]["description"]
    assert "只回答方位与距离" in got["get_radar_position"]["description"]
    # 笼统提问要三个都调 —— 否则「我当前状态」只会报一项（实测只报心率）
    for fn in got.values():
        assert "笼统" in fn["description"]


# ───────────────────── 方位措辞分档 ─────────────────────

def test_where_text_tiers():
    assert _radar_where_text(-100, 1000) == "正前方"   # 5.7°
    assert _radar_where_text(-450, 1000) == "左前方"   # 24.2°
    assert _radar_where_text(450, 1000) == "右前方"
    assert _radar_where_text(-2000, 1000) == "左侧"    # 63.4°
    assert _radar_where_text(2000, 1000) == "右侧"


def test_where_text_rejects_unusable():
    assert _radar_where_text(None, 1000) is None
    assert _radar_where_text(-450, None) is None
    assert _radar_where_text(-450, 0) is None      # 正侧/身后：不猜
    assert _radar_where_text(-450, -100) is None


# ───────────────────── 「问什么答什么」的结构性保证 ─────────────────────

def _seed_full():
    """两个生命体征都有值 —— 复刻用户实测到的那个场景（breath=14 heart=69）。"""
    update_device_radar(
        _DEV,
        {"type": "radar_state", "present": True, "x_mm": -450, "y_mm": 1000, "heart_rate": 69, "breath_rate": 14},
    )


def test_breath_tool_never_returns_heart_rate():
    """问呼吸时心率**不进上下文** —— 这是「问呼吸却报心率」那个 bug 的回归测试。"""
    _seed_full()
    row = _run("get_breath_rate")
    assert row["ok"] is True
    assert row["breath_rate"] == 14
    assert "heart_rate" not in row


def test_heart_tool_never_returns_breath_rate():
    """反过来也一样：问心率时呼吸不进上下文。"""
    _seed_full()
    row = _run("get_heart_rate")
    assert row["ok"] is True
    assert row["heart_rate"] == 69
    assert "breath_rate" not in row


def test_position_tool_never_returns_vitals():
    """只问方位时，心率/呼吸**不进上下文**。"""
    _seed_full()
    row = _run("get_radar_position")
    assert row["ok"] is True and row["present"] is True
    assert row["where"] == "左前方"
    assert row["distance_m"] == 1.1
    assert "heart_rate" not in row and "breath_rate" not in row


# ───────────────────── 缺失与错误 ─────────────────────

def test_unmeasured_gives_note():
    """测不到时给一句显式说明，而不是空对象 —— 否则模型容易拿上下文里
    另一个数值来凑答案。"""
    update_device_radar(_DEV, {"type": "radar_state", "present": True, "x_mm": -450, "y_mm": 1000})
    for tool, cn in (("get_heart_rate", "心率"), ("get_breath_rate", "呼吸率")):
        row = _run(tool)
        assert row["ok"] is True
        assert f"心跳" not in row and f"呼吸" not in row
        assert cn in row["note"] and "测不到" in row["note"]


def test_vitals_independent_of_present():
    """LD2450 丢目标 ≠ 无人：心率/呼吸来自 R60，不能跟着 present=False 一起消失。"""
    update_device_radar(_DEV, {"type": "radar_state", "present": False, "heart_rate": 69, "breath_rate": 14})
    assert _run("get_heart_rate")["heart_rate"] == 69
    assert _run("get_breath_rate")["breath_rate"] == 14


def test_position_no_target():
    update_device_radar(_DEV, {"type": "radar_state", "present": False, "heart_rate": 69})
    row = _run("get_radar_position")
    assert row["ok"] is True and row["present"] is False
    assert "where" not in row


def test_runner_never_received_is_error():
    """从未收到 → 回错误（不是编造数据）。"""
    row = _run("get_heart_rate")
    assert row["ok"] is False
    assert "过期或不可用" in row["error"]


def test_runner_without_device_id_is_error():
    """走 execute_llm_tools 的异常兜底：不抛到调用方，转成 ok=False。"""
    row = _run("get_radar_position", device_id=None)
    assert row["ok"] is False
    assert "device_id" in row["error"]


# ───────────────────── 缓存层 ─────────────────────

def test_cache_ttl_expiry_does_not_delete_record():
    _seed_full()
    assert get_device_radar(_DEV, max_age_s=0.0) is None   # 过期 → None
    assert has_device_radar(_DEV) is True                  # 但「见过」保留：工具仍注册


def test_cache_range_guard():
    """越界的值当缺失，不喂给 LLM。"""
    update_device_radar(
        _DEV,
        {"type": "radar_state", "present": True, "x_mm": -450, "y_mm": 9000, "heart_rate": 900, "breath_rate": 16},
    )
    snap = get_device_radar(_DEV)
    assert snap is not None
    assert "x_mm" not in snap and "y_mm" not in snap   # 9m 超出上限
    assert "heart_rate" not in snap                    # 900 次/分不合理
    assert snap["breath_rate"] == 16


def test_clear_device_drops_seen():
    _seed_full()
    clear_device(_DEV)
    assert has_device_radar(_DEV) is False
    assert get_device_radar(_DEV) is None
