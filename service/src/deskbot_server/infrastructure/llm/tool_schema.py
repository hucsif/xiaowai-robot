"""OpenAI 原生 tools（function calling）schema 定义。

参数键与 ``llm_tool_runner.execute_llm_tools`` / 各 service 工具读取的**平铺键一一对应**：
原生调用解析后 ``raw = {"tool": name, **arguments}`` 可直接进现有执行器，执行层零改动。
description 用中文承载触发规则（行为约束）；本地小模型对嵌套 oneOf 遵循率差，
``schedule_task`` 采用平铺 action+enum+条件字段全 optional 的宽松建模。
"""

from __future__ import annotations

from typing import Any


def _fn(name: str, description: str, required: list[str], properties: dict[str, Any]) -> dict[str, Any]:
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": description,
            "parameters": {
                "type": "object",
                "properties": properties,
                "required": required,
            },
        },
    }


# ───────────────────── 第一批：纯函数工具 ─────────────────────

def _batch1_schemas() -> list[dict[str, Any]]:
    return [
        _fn(
            "memory_add",
            "把值得长期记住的用户信息存入长期记忆（名字/喜好/事实/约定等）。"
            "text 写用户原意的完整短句；随口闲聊不要存。",
            ["text"],
            {"text": {"type": "string", "description": "要记住的内容，完整短句"}},
        ),
        _fn(
            "memory_delete",
            "删除一条长期记忆。id 必须取自 system 提示中长期记忆清单方括号中的 id，禁止编造。",
            ["id"],
            {"id": {"type": "string", "description": "记忆 id（见 system 长期记忆方括号）"}},
        ),
        _fn(
            "schedule_task",
            "定时/提醒任务增删改查（北京时间东八区）。**用户要求定时/提醒时必须调用，禁止仅口头答应。**"
            "action=create 时用 task + task_kind(once/recurring) + cron 或 delay_minutes；"
            "cron 为「分 时 日 月 周」五段，如每天8点 \"0 8 * * *\"、明天9点 \"0 9 <明日日期> <明日月份> *\"；"
            "其余 action 用 id（update 可带 task/cron/enabled）。创建无需 session_id。",
            ["action"],
            {
                "action": {"type": "string", "enum": ["create", "list", "get", "update", "delete"], "description": "操作类型"},
                "task": {"type": "string", "description": "提醒内容（create/update）"},
                "task_kind": {"type": "string", "enum": ["once", "recurring"], "description": "once 一次性 / recurring 周期性（create 必填）"},
                "cron": {"type": "string", "description": "五段 cron（分 时 日 月 周），与 delay_minutes 二选一"},
                "delay_minutes": {"type": "integer", "description": "相对延迟分钟数（如「两分钟」→ 2），与 cron 二选一"},
                "id": {"type": "string", "description": "任务 id（get/update/delete 用）"},
                "enabled": {"type": "boolean", "description": "update 时是否启用"},
            },
        ),
        _fn(
            "webfetch",
            "抓取指定网页并返回正文文本（结果可能被截断）。仅当需要读取某个具体网址内容时使用。",
            ["url"],
            {"url": {"type": "string", "description": "http/https 网址"}},
        ),
        _fn(
            "websearch",
            "网络搜索获取摘要。仅当问题需要实时/外部信息（新闻、天气、股价、赛事等）时调用；"
            "query 用简洁中文关键词，如「今天北京天气」。",
            ["query"],
            {
                "query": {"type": "string", "description": "搜索关键词（中文）"},
                "max_results": {"type": "integer", "description": "返回条数，默认 5，上限 10"},
            },
        ),
        _fn(
            "session",
            "查询当前与最近对话 session（10 分钟无对话自动开新 session）。",
            ["action"],
            {
                "action": {"type": "string", "enum": ["current", "list", "get"], "description": "current 当前 / list 列表 / get 详情"},
                "limit": {"type": "integer", "description": "list 条数，默认 10"},
                "session_id": {"type": "string", "description": "get 指定 session；省略读当前"},
            },
        ),
    ]


# ───────────────────── 第二批（A2 阶段启用）─────────────────────

def _batch2_schemas(*, device_id: str | None = None, quest_tasks: list[dict] | None = None) -> list[dict[str, Any]]:
    """register_face / register_voiceprint + 剧情任务工具 complete_task。

    ``quest_tasks`` 为 ``[{"task_id", "type"}]``（进行中任务），非空才产出
    complete_task；任务 id 与类型动态注入 description，不进 parameters enum。
    """
    out: list[dict[str, Any]] = [
        _fn(
            "register_face",
            "把当前画面中的人脸注册/更新到档案。省略 face_id = 当前画面唯一人脸；"
            "多人画面必须从每轮 user 消息「图像识别」行取 faceid= 指定，或先向用户澄清。",
            ["name"],
            {
                "name": {"type": "string", "description": "姓名"},
                "face_id": {"type": "integer", "description": "画面人脸编号（见 user 消息图像识别）"},
            },
        ),
        _fn(
            "register_voiceprint",
            "记住刚说话的人的声音（注册样本来自最近一次对话语音）。用户说「记住我的声音/我叫xx」时必须调用；"
            "若返回样本不足的提示，请引导用户先对机器人说一句完整的话再重新注册。",
            ["name"],
            {"name": {"type": "string", "description": "姓名"}},
        ),
    ]
    if quest_tasks:
        from deskbot_server.service.quest_service import TYPE_LABELS

        ids_text = ", ".join(f"{t['task_id']}({TYPE_LABELS.get(t.get('type', 'once'), '一次性')})" for t in quest_tasks)
        out.append(
            _fn(
                "complete_task",
                f"完成/记录进行中剧情任务的推进（当前可用任务：{ids_text}）。"
                "一次性(once)：本次对话目标达成才调用，reason 写达成内容，完成后自动接续其后继任务；"
                "日常(daily)：对当前用户今日完成一次调一次（若服务端返回 deduped 表示今日已记录，勿重复调）；"
                "长期(long_term)：有实质新进展才调用，可对不同用户/不同时间多次累计记录。"
                "user 填当前对话用户（daily/long_term 必填，once 可空）。",
                ["task_id", "reason"],
                {
                    "task_id": {"type": "string", "description": "目标任务 id（见上方可用任务）"},
                    "user": {"type": "string", "description": "当前对话用户（日常/长期任务必填）"},
                    "reason": {"type": "string", "description": "完成内容/达成结果/新进展（口语转述，必填）"},
                },
            )
        )
    return out


# ───────────────────── 第三批：用户社交（按人归档）工具 ─────────────────────

def _user_social_schemas() -> list[dict[str, Any]]:
    """update_user_info / update_daily_task：识别到具体用户时按人归档与记账。

    独立于 batch1（不破坏 NATIVE_TOOL_NAMES_BATCH1 的既有精确断言），
    随 batch2 开关默认启用。
    """
    return [
        _fn(
            "update_user_info",
            "用户当面告知姓名/性别/年龄/家庭/住址/爱好/职业等个人信息时调用，"
            "按人归档到该用户资料文件（该用户在场被识别到时才会被参考）。"
            "只写用户刚新披露的事实短句；纠正旧信息写成「更正:…」新行；"
            "不要整段重述已写过的内容。参数键**必须**为 user_name 与 chat_message："
            "user_name 写被识别到的人名（中文/字母/数字），chat_message 写原意的"
            "完整短句（如「我住在北京市海淀区」），不需要带时间；不要拆成 user/"
            "location/age 等散键。",
            ["user_name", "chat_message"],
            {
                "user_name": {"type": "string", "description": "用户姓名（须为中文/字母/数字）"},
                "chat_message": {"type": "string", "description": "用户透露的信息，原意短句"},
            },
        ),
        _fn(
            "update_daily_task",
            "主动任务记账：早上/中午/晚上第一次见到认识的人时主动问候、饭点询问吃饭、"
            "或距上次对话较久表达思念——开口**前**必须先调用本工具记账一次，再开口说话；"
            "用户当面交代的饮食/日程等完成事项也可记。参数键**必须**为 user_name 与 "
            "message：user_name 写被问候/关心的用户姓名，message 用第一人称一句话写这次"
            "主动互动的内容（如「我跟小明说了早上好」「我问了小明中午吃了什么」），"
            "不用自带时间，服务端自动补；同一意图已记录则不重复（同时段问候只记一次、"
            "思念 30 分钟内不重复）。",
            ["user_name", "message"],
            {
                "user_name": {"type": "string", "description": "被问候/关心的用户姓名"},
                "message": {"type": "string", "description": "本次主动互动内容，第一人称一句话"},
            },
        ),
    ]


# ───────────────────── 雷达状态（按设备能力注册）─────────────────────

# 三个雷达工具共用的「笼统提问」补丁。
# 拆成三个单值工具解决了「问心率却把呼吸方位一起念」和「问呼吸却报心率」，
# 但带来了相反方向的失效：笼统地问「我现在怎么样」时，模型只挑一个匹配度最高的
# 调用（实测挑中心率），因为三个工具里**没有「全都要」这个选项**。
# 「用户问的是某项还是笼统问整体」这个判断只能在 prompt 里引导 —— 结构上锁不住。
_RADAR_BROAD_HINT = (
    "用户**笼统**地问整体状态/身体情况、没指明具体哪一项时（如「我现在怎么样」「我的状态」），"
    "三个雷达工具都要调用（本工具 + 另外两个），不要只调本工具报一项。"
)


def _radar_schemas(*, device_id: str | None = None) -> list[dict[str, Any]]:
    """雷达感知：**每个工具只回答一件事，按「用户问了什么」拆开**。

    ``get_heart_rate`` / ``get_breath_rate`` / ``get_radar_position`` 三个零参工具，
    而不是一个（或两个）工具返回多个字段。实测过两轮，这条经验很硬：

    - 一个工具返回 ``{where, distance_m, heart_rate, breath_rate}`` → 模型把四个
      数值**一起念出来**；
    - 缩成一个返回心率+呼吸的 ``get_radar_vitals`` → 模型**问呼吸却报心率**
      （两个值都在上下文里，它挑了错的那个）。

    拆开之后「用户问了什么」被编码进「调了哪个工具」：没被问到的数据**根本不进
    上下文**，模型既没法顺带报、也没法挑错。这比在 description 里写「别全说」
    硬得多 —— 后者只是软约束，本地小模型经常不遵守。

    只在**该设备确实上报过** ``radar_state`` 时才产出 —— 没接雷达（或固件侧
    ``DESKBOT_RADAR_UPLINK_ENABLE=0``）的设备不该看到一堆注定查不出东西的工具。
    判定见 ``radar_snapshot_cache.has_device_radar``。

    零参数建模：文件头注释提到的「本地小模型对嵌套/多参 schema 遵循率差」同样适用，
    这里干脆零参，模型只需判断「该不该查、查哪一个」。description 里还刻意互相
    点名（「呼吸率是另一个工具，不要拿它替代」），堵住拿近邻数值顶替的路。
    """
    from deskbot_server.service.application.radar_snapshot_cache import has_device_radar

    if not has_device_radar(device_id):
        return []
    return [
        _fn(
            "get_heart_rate",
            "读取用户当前的心率（次/分，雷达生理感知）。"
            "**用户问自己的心跳/心率时必须调用本工具**，禁止凭记忆或猜测回答。"
            "呼吸率是另一个工具 get_breath_rate —— 用户问呼吸时不要用本工具、"
            "也不要用这里的心率数值去顶替。"
            "返回里缺 heart_rate 表示当前测不到，如实说明即可，不要编造数值。"
            + _RADAR_BROAD_HINT,
            [],  # 无参数
            {},
        ),
        _fn(
            "get_breath_rate",
            "读取用户当前的呼吸率（次/分，雷达生理感知）。"
            "**用户问自己的呼吸/呼吸频次/喘气时必须调用本工具**，禁止凭记忆或猜测回答。"
            "心率是另一个工具 get_heart_rate —— 用户问心率时不要用本工具、"
            "也不要用这里的呼吸数值去顶替。"
            "返回里缺 breath_rate 表示当前测不到，如实说明即可，不要编造数值。"
            + _RADAR_BROAD_HINT,
            [],  # 无参数
            {},
        ),
        _fn(
            "get_radar_position",
            "读取用户相对机器人的方位与距离（雷达运动追踪）。"
            "**用户问自己在哪里/在什么方位/离机器人多远时必须调用本工具**，禁止凭记忆或猜测回答。"
            "**只回答方位与距离**，不要顺带报心率/呼吸。"
            "返回里缺 where 表示雷达暂时没检到人（人极静时运动雷达可能丢），如实说明。"
            + _RADAR_BROAD_HINT,
            [],  # 无参数
            {},
        ),
    ]


def _say_schema() -> dict[str, Any]:
    """工具轮过渡语工具：与其它工具**同时**调用，服务端顺带播报一句口语。

    只在首轮工具轮注入（``_is_tool_round=True``）——见 ``build_native_tool_schemas``：
    模型看不到本工具时自然不会调用，无需服务端额外拦截。
    """
    return _fn(
        "say",
        "执行其它工具时顺带告诉用户你要做什么的一句话，让等待不那么干。"
        "必须与其它工具**同时**调用（单独调用没有任何效果）；只在确实要让用户等一会儿时才用。"
        "text 写即将要做的事，像「我帮你查一下」，15 字以内口语；"
        "禁止预报还没发生的结果（没查完不许说「查到了」），禁止解释动作本身（别说「我要调用搜索」）。",
        ["text"],
        {"text": {"type": "string", "description": "对用户说的过渡语，15 字以内口语"}},
    )


def build_native_tool_schemas(
    *,
    device_id: str | None = None,
    include_batch2: bool = True,
    is_tool_round: bool = False,
) -> list[dict[str, Any]]:
    """输出当前启用的原生工具 schema（供每轮 tools 参数）。

    batch1 = 纯函数六工具；batch2 = 人脸/声纹注册 + 剧情任务（无 running 任务时
    quest 工具不产出；任务 id/类型动态注入 description，不进 parameters enum）；
    batch3（随 batch2 开关）= 用户社交按人归档两工具，恒在；
    批次内还有三个雷达感知工具（get_heart_rate / get_breath_rate / get_radar_position，
    仅该设备上报过雷达数据时产出）；
    batch4 = ``say`` 过渡语工具，仅首轮工具轮产出（``is_tool_round=True``），
    恒排在末尾——batch1 前缀顺序与集合不受影响。

    ⚠️ 顺序约束（有测试盯着）：新工具一律追加在 ``_batch1_schemas()`` 之外、
    ``include_batch2`` 之内、``say`` 的 append **之前**——batch1 是被精确断言的
    六个名字，而 ``say`` 必须保持末位。
    """
    schemas = _batch1_schemas()
    if include_batch2:
        quest_tasks: list[dict] | None = None
        if device_id:
            from deskbot_server.service.quest_service import QuestService

            calls = QuestService().get_tool_calls(str(device_id))
            if calls:
                quest_tasks = calls[0].get("tasks") or []
        schemas += _batch2_schemas(device_id=device_id, quest_tasks=quest_tasks)
        schemas += _user_social_schemas()
        schemas += _radar_schemas(device_id=device_id)
    if is_tool_round:
        schemas.append(_say_schema())
    return schemas


NATIVE_TOOL_NAMES_BATCH1 = [s["function"]["name"] for s in _batch1_schemas()]
