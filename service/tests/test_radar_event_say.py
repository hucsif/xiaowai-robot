"""雷达动作事件（挥手/入座/离座）→ 说一句固定台词。

这条链路**刻意不过 LLM**：直接 TTS 合成 → 组 pb → 下发（与聊天共用播放链路）。
测试重点：三类事件都有台词、逐项开关/离线/关闭/节流时不说话、下发要带轮代号（可被打断），
以及 data/radar_event_say.json 这个配置文件本身的解析与生成。
"""

from __future__ import annotations

import asyncio
import json

import pytest

from deskbot_server.dao import radar_event_say_store
from deskbot_server.dao.radar_event_say_store import (
    DEFAULT_LINES,
    event_line,
    load_radar_event_say_cfg,
    normalize_radar_event_say_cfg,
)
from deskbot_server.service.application import radar_event_say
from deskbot_server.service.application.radar_event_say import _clear_throttle, say_radar_event_line


class _FakeDeviceWs:
    """冒充 `DeviceWsService`：`_get_ws` 判在线、`current_turn_epoch` 取轮代、`send` 下发。"""

    def __init__(self, *, online: bool = True, epoch: int = 7, delivered: int = 1):
        self._online = online
        self.epoch = epoch
        self.delivered = delivered
        self.sent: list[tuple] = []

    def _get_ws(self, device_id):  # noqa: ARG002
        return object() if self._online else None

    def current_turn_epoch(self, device_id):  # noqa: ARG002
        return self.epoch

    async def send(self, device_id, pb_seq, *, turn_epoch=None):
        self.sent.append((device_id, pb_seq, turn_epoch))
        return self.delivered


class _FakeChat:
    tts_cfg = {"sample_rate": 24000}


class _FakeBus:
    def __init__(self):
        self.calls: list[dict] = []

    async def publish_auto_dispatch(self, device_id, **kw):
        self.calls.append({"device_id": device_id, **kw})


@pytest.fixture(autouse=True)
def _tmp_cfg(tmp_path, monkeypatch):
    """配置文件指向临时路径 —— 测试不碰真实的 data/radar_event_say.json。"""
    monkeypatch.setattr(
        radar_event_say_store, "RADAR_EVENT_SAY_FILE", str(tmp_path / "radar_event_say.json")
    )
    _clear_throttle()
    yield
    _clear_throttle()


@pytest.fixture()
def tts_called(monkeypatch):
    """拦截 TTS 与组 pb，返回记录列表。"""

    calls: list[str] = []

    class _Tts:
        async def synthesize_phoneme_segments(self, text, *, device_id=None):  # noqa: ARG002
            calls.append(text)
            return 24000, [{"phoneme": "n", "ms": 100, "pcm": b"\x00" * 100}]

    class _Pb:
        def build_pb_seq_from_tts(self, segs, tts_cfg, **kw):  # noqa: ARG002
            return object()

    monkeypatch.setattr(radar_event_say, "TtsService", _Tts)
    monkeypatch.setattr(radar_event_say, "PbService", _Pb)
    return calls


def _say(device_ws, event, *, bus=None):
    return asyncio.run(say_radar_event_line(device_ws, "dev_x", event, chat=_FakeChat(), bus_service=bus))


def _write_cfg(content: dict) -> None:
    with open(radar_event_say_store.RADAR_EVENT_SAY_FILE, "w", encoding="utf-8") as f:
        json.dump(content, f, ensure_ascii=False)


# ───────────────────── 三类事件 ─────────────────────

@pytest.mark.parametrize("event", ["wave", "seated", "away"])
def test_each_event_speaks_its_line(tts_called, event):
    dev = _FakeDeviceWs()
    assert _say(dev, event) is True
    assert tts_called == [DEFAULT_LINES[event]["text"]]  # 合成的正是该类事件的默认台词
    assert len(dev.sent) == 1


def test_send_carries_turn_epoch_so_user_can_interrupt(tts_called):
    """下发要带当前轮代号 —— 用户一开口新语音轮 +1，本条未播完的 pb 会被丢弃。"""
    dev = _FakeDeviceWs(epoch=7)
    _say(dev, "wave")
    device_id, _pb_seq, turn_epoch = dev.sent[0]
    assert device_id == "dev_x"
    assert turn_epoch == 7


def test_unknown_event_is_ignored(tts_called):
    dev = _FakeDeviceWs()
    assert _say(dev, "sneeze") is False
    assert tts_called == [] and dev.sent == []


# ───────────────────── 不该说话的情形 ─────────────────────

def test_offline_device_is_skipped(tts_called):
    assert _say(_FakeDeviceWs(online=False), "wave") is False
    assert tts_called == []


def test_master_switch_says_nothing(tts_called, monkeypatch):
    monkeypatch.setattr(radar_event_say, "RADAR_EVENT_SAY_ENABLED", False)
    dev = _FakeDeviceWs()
    assert _say(dev, "wave") is False
    assert tts_called == [] and dev.sent == []


def test_throttle_blocks_repeat_within_cooldown(tts_called):
    """5s 冷却：连挥几次只念叨一句（挥手 FSM 判据较松，这道兜底是必要的）。"""
    dev = _FakeDeviceWs()
    assert _say(dev, "wave") is True
    assert _say(dev, "wave") is False
    assert len(tts_called) == 1


def test_throttle_is_per_event(tts_called):
    """冷却按 (设备, 事件) 分开 —— 挥手不该把入座那句也吞掉。"""
    dev = _FakeDeviceWs()
    assert _say(dev, "wave") is True
    assert _say(dev, "seated") is True
    assert tts_called == [DEFAULT_LINES["wave"]["text"], DEFAULT_LINES["seated"]["text"]]


def test_delivery_failure_is_reported(tts_called):
    assert _say(_FakeDeviceWs(delivered=0), "wave") is False


# ───────────────────── 配置文件：逐项开关与改词 ─────────────────────

def test_cfg_file_is_seeded_on_first_load():
    cfgs = load_radar_event_say_cfg()
    assert set(cfgs) == set(DEFAULT_LINES)
    assert all(c["enabled"] for c in cfgs.values())
    # 真的落盘了 —— 使用者要能找得到这个文件
    with open(radar_event_say_store.RADAR_EVENT_SAY_FILE, encoding="utf-8") as f:
        assert set(json.load(f)) == set(DEFAULT_LINES)


def test_per_event_disable_only_affects_that_event(tts_called):
    """用户要的正是这个：单独关掉挥手，入座/离座照常。"""
    _write_cfg(
        {
            "wave": {"enabled": False, "text": "你好呀！"},
            "seated": {"enabled": True, "text": "你回来啦～"},
            "away": {"enabled": True, "text": "那我先歇会儿。"},
        }
    )
    dev = _FakeDeviceWs()
    assert _say(dev, "wave") is False
    assert _say(dev, "seated") is True
    assert tts_called == ["你回来啦～"]  # 挥手那句没合成


def test_custom_text_is_used(tts_called):
    _write_cfg({"wave": {"enabled": True, "text": "嗨，看到你招手啦"}})
    assert _say(_FakeDeviceWs(), "wave") is True
    assert tts_called == ["嗨，看到你招手啦"]


def test_empty_text_falls_back_to_default():
    """文案被清空 → 回默认，避免「开着但没话说」的半残状态。"""
    assert normalize_radar_event_say_cfg({"wave": {"enabled": True, "text": "   "}})["wave"]["text"] == (
        DEFAULT_LINES["wave"]["text"]
    )


def test_missing_entry_falls_back_to_default():
    """文件里只写了一项 —— 另两项回默认，不被一起清掉。"""
    cfgs = normalize_radar_event_say_cfg({"wave": {"enabled": False}})
    assert cfgs["wave"]["enabled"] is False
    assert cfgs["seated"] == dict(DEFAULT_LINES["seated"])
    assert cfgs["away"] == dict(DEFAULT_LINES["away"])


def test_broken_json_falls_back_to_defaults():
    with open(radar_event_say_store.RADAR_EVENT_SAY_FILE, "w", encoding="utf-8") as f:
        f.write("{ 这不是合法 JSON ")
    cfgs = load_radar_event_say_cfg()
    assert cfgs == normalize_radar_event_say_cfg(None)


@pytest.mark.parametrize("raw", ["0", "false", "no", "off", " FALSE "])
def test_enabled_accepts_env_style_falsy_strings(raw):
    """allowed 写成字符串也认（与 .env 的解析规则一致，手改文件不容易踩坑）。"""
    assert normalize_radar_event_say_cfg({"wave": {"enabled": raw}})["wave"]["enabled"] is False


def test_event_line_returns_none_when_disabled():
    _write_cfg({"wave": {"enabled": False, "text": "你好呀！"}})
    assert event_line("wave") is None
    assert event_line("nope") is None
    assert event_line("seated") == DEFAULT_LINES["seated"]["text"]


# ───────────────────── 记账 ─────────────────────

def test_auto_dispatch_recorded_on_success(tts_called):
    bus = _FakeBus()
    assert _say(_FakeDeviceWs(), "wave", bus=bus) is True
    assert len(bus.calls) == 1
    assert bus.calls[0]["source"] == "auto_radar_event"


def test_auto_dispatch_skipped_on_failure(tts_called):
    bus = _FakeBus()
    assert _say(_FakeDeviceWs(delivered=0), "wave", bus=bus) is False
    assert bus.calls == []
