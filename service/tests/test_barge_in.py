"""barge-in：用户说话打断机器人正在播的回答（轮代门控）。

机制：新语音轮把 ``_DeviceEntry.turn_epoch`` +1，旧轮（语音轮 + 主动轮）
持有的旧代号即失效；``DeviceWsService.send(turn_epoch=...)`` 在校验不过时
丢弃不下发，从根上止住「旧轮算完 LLM 后把新回答抢回去」（串台）。

**最核心的不变量**：任何「轮代过期」的丢弃路径都必须 set ``pb_seq._done`` ——
否则 ``_run_pb_playback`` 里 ``await send(wait=True)`` 会永久挂起。
本文件第一条测试就是守它。
"""

from __future__ import annotations

import asyncio

from deskbot_server.model.pb_seq import PbAction, PbBlock, PbSeq, PbType
from deskbot_server.service.device_ws_service import DeviceWsService, _DeviceEntry


def _blocks(req: str, n: int) -> tuple[PbBlock, ...]:
    """n 片链：pb_start(idx=0) → pb_chunk(1..n-2) → pb_end(idx=n-1)。"""
    return tuple(
        PbBlock(
            type=PbType.START if i == 0 else (PbType.END if i == n - 1 else PbType.CHUNK),
            req=req,
            idx=i,
            chunk_ms=100,
        )
        for i in range(n)
    )


def _seq(req: str, n: int = 2, level: int = 1) -> PbSeq:
    return PbSeq(req=req, entries=_blocks(req, n), level=level, action=PbAction.REPLACE)


def _make_svc(monkeypatch):
    DeviceWsService.reset_instance()
    svc = DeviceWsService()
    entry = _DeviceEntry(device_id="d1")
    svc._devices["d1"] = entry
    sent: list[tuple[str, PbType, str, int]] = []

    async def fake_send(device_id: str, block: PbBlock, **_kw) -> bool:
        sent.append((device_id, block.type, block.req, block.idx))
        return True

    monkeypatch.setattr(svc, "_do_send_to_device", fake_send)
    return svc, entry, sent


# ---------------------------------------------------------------------------
# 核心不变量：过期丢弃必须 set _done（否则永久挂起）
# ---------------------------------------------------------------------------


def test_stale_epoch_dropped_and_done_set(monkeypatch):
    """【最关键】轮代过期 → 返回 0、_done 被 set、不下发任何内容。

    漏掉 ``_done.set()`` 会让 ``_run_pb_playback`` 的 ``await send(wait=True)``
    永久挂起（比串台更严重：整轮卡死）。这里用 wait_for 兜住超时。
    """
    svc, entry, sent = _make_svc(monkeypatch)
    entry.turn_epoch = 5
    seq = _seq("stale1")

    async def _go() -> int:
        return await asyncio.wait_for(
            svc.send("d1", seq, wait=True, turn_epoch=4), timeout=1.0
        )

    n = asyncio.run(_go())
    assert n == 0, "过期轮代必须返回 0（丢弃）"
    assert seq._done.is_set(), "_done 必须被 set，否则 wait=True 永久挂起"
    assert sent == [], "过期轮代不得下发任何内容"


# ---------------------------------------------------------------------------
# 正常路径与不误伤
# ---------------------------------------------------------------------------


def test_fresh_epoch_enqueued(monkeypatch):
    """轮代一致 → 正常入队（返回 1）。"""
    svc, entry, _sent = _make_svc(monkeypatch)
    entry.turn_epoch = 5
    seq = _seq("ok1")

    n = asyncio.run(svc.send("d1", seq, wait=False, turn_epoch=5))
    assert n == 1


def test_none_epoch_not_gated(monkeypatch):
    """``turn_epoch=None`` → 不门控。

    待机动画 / 开机问候 / 调试台 / 人脸跟随等 6 处调用点都不传该参数，
    这条保证改动不会误伤它们（即使设备轮代是个很大的值）。
    """
    svc, entry, _sent = _make_svc(monkeypatch)
    entry.turn_epoch = 99  # 故意设个大值
    seq = _seq("idle1", level=0)

    n = asyncio.run(svc.send("d1", seq, wait=False, turn_epoch=None))
    assert n == 1, "None 不应受轮代影响"


def test_missing_device_not_crashing(monkeypatch):
    """设备不存在时也不该抛异常（send 既有语义是返回 0）。"""
    svc, _entry, _sent = _make_svc(monkeypatch)
    seq = _seq("gone1")
    n = asyncio.run(svc.send("nope", seq, wait=True, turn_epoch=0))
    assert n == 0
    assert seq._done.is_set()


# ---------------------------------------------------------------------------
# 轮代语义
# ---------------------------------------------------------------------------


def test_current_turn_epoch_reads_without_bump(monkeypatch):
    """``current_turn_epoch`` 只读不递增。

    主动轮（定时提醒/剧情推进/社交问候）用它把「被新语音打断」接进来，
    但自己【不】递增 —— 否则主动轮会反向杀掉在跑的语音轮。
    """
    svc, entry, _sent = _make_svc(monkeypatch)
    entry.turn_epoch = 7

    assert svc.current_turn_epoch("d1") == 7
    assert svc.current_turn_epoch("d1") == 7, "重复读不应改变值"
    assert entry.turn_epoch == 7, "绝不能顺带递增"
    assert svc.current_turn_epoch("不存在") is None
    assert svc.current_turn_epoch(None) is None


def test_bump_invalidates_previous_epoch(monkeypatch):
    """递增后：旧代号的 send 被丢弃、新代号的放行 —— 模拟「用户插话」。"""
    svc, entry, _sent = _make_svc(monkeypatch)

    old_epoch = entry.turn_epoch      # 旧轮起轮时读到的代号
    entry.turn_epoch += 1             # ← barge-in：新语音轮递增
    new_epoch = entry.turn_epoch

    old_seq = _seq("old1")
    new_seq = _seq("new1")

    async def _go() -> tuple[int, int]:
        n_old = await asyncio.wait_for(
            svc.send("d1", old_seq, wait=True, turn_epoch=old_epoch), timeout=1.0
        )
        n_new = await svc.send("d1", new_seq, wait=False, turn_epoch=new_epoch)
        return n_old, n_new

    n_old, n_new = asyncio.run(_go())
    assert n_old == 0, "旧轮必须被丢弃（不再抢回回答）"
    assert old_seq._done.is_set(), "旧轮的 _done 也要 set，否则旧轮卡在 send 上"
    assert n_new == 1, "新轮正常下发"
