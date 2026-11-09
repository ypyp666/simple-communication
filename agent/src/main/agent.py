"""多智能体路由图：Supervisor 分发 + 三个专家 + HITL 审批 + 结果播报。

拓扑（与架构图一致）：

    START → supervisor ─┬→ chat_expert ────────────────────────────────→ END
                        ├→ imagine_expert ─────────────────────────────→ END
                        └→ task_expert ─→ examine_expert(HITL) ─┬─批准─→ task_expert(执行)
                                              │   ▲              │              │
                                              │   └──────────────┘              │
                                              │ 拒绝                             │
                                              ▼                                 ▼
                                         report_expert ←───────────────────────┘
                                              │
                                              └──────────────────────────────→ END

要点：
1. supervisor 只做意图分类，自己不产出用户可见文本，流式输出时必须过滤掉。
2. task_expert 是两段式节点：
   - approval_status != "approved" → 规划阶段，只生成方案，不执行
   - approval_status == "approved" → 执行阶段，产出可交给客户端执行的指令
3. examine_expert 用 interrupt() 挂起，checkpointer 持久化 thread_id，
   客户端批准后通过 Command(resume=...) 恢复同一条会话。
4. report_expert 是任务链路的收尾节点：不管批准后执行、还是被拒绝，
   都要用一句人话告诉用户「刚才干了什么、下一步看哪里」，它产出的文本
   是可见的（在 VISIBLE_NODES 里），所以用户不会再看到任务链路静悄悄地结束。
"""
import json

import aiosqlite

from langchain.agents import create_agent
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage
from langchain_core.runnables import RunnableConfig
from langchain_openai import ChatOpenAI

from langgraph.checkpoint.sqlite.aio import AsyncSqliteSaver
from langgraph.graph import END, START, MessagesState, StateGraph
from langgraph.types import Command, interrupt

# 共享配置统一从 config 取，tool.py 也从这里取，避免循环导入
from main.config import (
    DEEPSEEK_API_KEY,
    DEEPSEEK_BASE_URL,
    DEEPSEEK_MODEL,
    DEEPSEEK_VISION_MODEL,
    RESOURCES_DIR,
    require,
)
from main.tool import (
    CHAT_TOOLS,
    TASK_TOOLS,
    VISION_TOOLS,
    list_task_actions,
    normalize_task_command,
)

MAX_RECURSION = 20


# ---------------------------------------------------------------- 模型

model = ChatOpenAI(
    model=DEEPSEEK_MODEL,
    api_key=require("DEEPSEEK_API_KEY", DEEPSEEK_API_KEY),
    base_url=DEEPSEEK_BASE_URL,
)

# 图片理解需要视觉模型；未单独配置时退回主模型（届时若模型不支持图片，
# imagine_expert 会返回明确的错误提示，而不是静默失败）。
vision_model = ChatOpenAI(
    model=DEEPSEEK_VISION_MODEL,
    api_key=require("DEEPSEEK_API_KEY", DEEPSEEK_API_KEY),
    base_url=DEEPSEEK_BASE_URL,
)


# ---------------------------------------------------------------- 状态

class AgentState(MessagesState):
    """messages 由 MessagesState 提供，自带 add_messages 合并规则。"""
    route: str = ""            # supervisor 分类结果：chat | imagine | task | end
    approval_status: str = ""  # "" 未提交 | "approved" 已批准 | "rejected" 已拒绝
    plan_summary: str = ""     # 给用户看的审批确认文案
    plan_steps: list[str] = [] # 计划执行的操作列表
    task_command: dict = {}    # 交给客户端真正执行的指令


# ---------------------------------------------------------------- checkpointer

_async_checkpointer = None


async def get_checkpointer():
    """返回可复用的 AsyncSqliteSaver 实例(首次调用时建表)。"""
    global _async_checkpointer
    if _async_checkpointer is None:
        db_path = str(RESOURCES_DIR / "personal_chief.db")
        connection = await aiosqlite.connect(db_path)
        _async_checkpointer = AsyncSqliteSaver(connection)
        if hasattr(_async_checkpointer, "setup"):
            await _async_checkpointer.setup()
    return _async_checkpointer


# ---------------------------------------------------------------- 提示词

CHAT_PROMPT = """
你是一个情感专家，你最擅长的就是与人沟通，你的沟通技巧贴合实际懂得安抚别人的情绪，别人说的话你都能接住，你是用户最喜爱的聊天助手。
工具使用规则：你手上挂着若干工具，每个工具能做什么、什么时候该调、返回什么，都写在工具自己的描述里；调之前先读描述，不要凭工具名猜。工具只读，不会改动任何数据。
###注意：你的回答必须贴合口语化贴合真人，但是遇到以下违规行为禁止回答：涉及色情、暴力、政治敏感、违法犯罪、诈骗、赌博、恐怖主义、极端主义、仇恨言论、歧视言论等内容,违反中国大陆法律法规的内容禁止回答
##此外补充涉及宗教敏感和政治敏感内容禁止回答，涉及歧视问题的内容（包括性别，地域，民族和群体）禁止回答，涉及未成年人不良信息的内容禁止回答，涉及医疗健康、金融投资、法律咨询等专业领域的内容禁止回答，涉及个人隐私和个人信息的内容禁止回答，涉及虚假信息和谣言的内容禁止回答，涉及广告营销和商业推广的内容禁止回答，涉及侵权盗版和知识产权的内容禁止回答，涉及网络安全和信息安全的内容禁止回答
#最后禁止回答的内容统一回复：对不起，我无法回答这个问题。请换一个话题。
"""

IMAGINE_PROMPT = """
你是图像理解专家。用户会发来图片，可能附带一句说明。
请完成：画面内容识别、文字/表格提取、关键信息归纳，并结合用户的问题给出结论。
如果图片模糊、信息不足或与问题无关，直接说明看不到什么，不要编造细节。
回答使用简体中文，先给结论，再给必要的细节，不要输出与图片无关的联想。
"""

# 动作白名单从 tool.py 生成，提示词里列出的动作就是客户端能执行的动作。
TASK_PROMPT = (
    "你是任务专家，负责理解用户想对软件执行的**有副作用的操作**（例如删除好友、修改关系、批量处理）。\n"
    "你的职责边界：\n"
    "- 只做意图识别与方案拆解，不要假装已经执行。\n"
    "- 如果用户意图含糊、缺少必要参数（对象、数量、范围），不要猜，直接在 summary 里向用户追问。\n"
    "- 高风险或不可逆操作（删除、清空、批量修改）标记 risk=\"high\"。\n"
    "\n可用动作只有下面这些，不要自己编造新的：\n"
    f"{list_task_actions()}\n"
    "\n工具使用规则：\n"
    "- 你身上挂着若干工具。每个工具能做什么、什么时候该调、返回什么、有什么限制，"
    "都写在工具自己的描述里；调之前先读描述，不要凭工具名猜。\n"
    "- 动作后面写了「执行前先按顺序调用 …」的，就按那个顺序先把数据拿到，再输出指令。\n"
    "- 工具只读，不会改动任何数据；真正的操作仍然只输出指令，由客户端执行。\n"
    "- 工具说查不到、或返回以「【…工具调用失败】」开头时，如实转述给用户，"
    "不要改写成成功，也不要自己编一个结果补上。\n"
    "- 需要的数据没拿到之前，command 留空 {}，不要给半成品指令。\n"
    "\n输出要求：只输出一个 JSON 对象，不要输出解释、不要加 markdown 代码块，字段如下：\n"
    '{"summary": "向用户确认这次要做什么，口语化一句话",\n'
    ' "steps": ["第一步", "第二步"],\n'
    ' "risk": "low|medium|high",\n'
    ' "command": {"action": "delete_friend", "params": {"name": "张三"}}}\n'
)

# 任务链路收尾播报。注意：真正的副作用在客户端执行，服务端只知道「指令已下发」，
# 所以这里明确禁止它宣称执行成功，避免用户以为已经落地。
REPORT_PROMPT = (
    "你是结果播报助手。任务流程刚刚走完，你要用一两句话向用户交代结果。\n"
    "要求：\n"
    "- 只依据给定的事实说话，不要补充没发生的事。\n"
    "- 批准并下发：说明准备执行什么操作，以及结果会由客户端执行后反馈。\n"
    "- 被拒绝：说明操作已取消，没有改动任何数据。\n"
    "- 缺参数/规划失败：说明卡在哪里，以及需要用户补充什么。\n"
    "- 不要声称操作已经成功完成，只能说已经下发/已经取消。\n"
    "- 口语化、简短，不要分点，不要用 markdown，不要重复字段名。\n"
)


# ---------------------------------------------------------------- 专家

# 工具列表统一从 tool.py 取，这里只选「用哪一组」，不直接声明工具
chat_expert = create_agent(
    model=model,
    tools=CHAT_TOOLS,
    system_prompt=CHAT_PROMPT,
    name="chat_expert",
)

imagine_expert = create_agent(
    model=vision_model,
    tools=VISION_TOOLS,
    system_prompt=IMAGINE_PROMPT,
    name="imagine_expert",
)

# 注意：本网关不支持 response_format=json_schema（with_structured_output 会直接 400），
# 所以结构化信息统一走「纯文本输出 + 自己解析」这条最稳的路。
supervisor_llm = model

# 任务专家要能自己调只读工具（比如查好友账号），才能把用户嘴里的名字
# 换成真实的账号ID。bind_tools 只是把工具清单挂给模型，真正的执行
# 由下面的 task_expert_node 手动控制，方便限制轮数、也方便拿到 config。
task_planner = model.bind_tools(TASK_TOOLS)

# 工具名 -> 工具对象，执行模型发起的工具调用时按名字查
TASK_TOOL_MAP = {}
for _tool in TASK_TOOLS:
    TASK_TOOL_MAP[_tool.name] = _tool

# 规划阶段最多允许模型连续调几轮工具，防止它反复查同一个名字陷入死循环。
# 三步链路（查好友 → 读记录 → 备包）恰好用掉三轮，再留一轮给收尾。
MAX_TOOL_ROUNDS = 4


# ---------------------------------------------------------------- 小工具

def _content_to_parts(content) -> list[dict]:
    """把 message.content 统一成 list[dict]，方便判断图片与取文本。"""
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    if isinstance(content, list):
        parts = []
        for piece in content:
            if isinstance(piece, dict):
                parts.append(piece)
            else:
                parts.append({"type": "text", "text": str(piece)})
        return parts
    return []


def _has_image(content) -> bool:
    parts = _content_to_parts(content)
    for piece in parts:
        if piece.get("type") == "image_url":
            return True
    return False


def _text_of(content) -> str:
    parts = _content_to_parts(content)
    text = ""
    for piece in parts:
        if piece.get("type") != "text":
            continue
        text += piece.get("text", "")
    return text


def _last_human(state: AgentState) -> HumanMessage | None:
    for msg in reversed(state["messages"]):
        if isinstance(msg, HumanMessage):
            return msg
    return None


def _keyword_route(question: str) -> str:
    """模型分类失败时的关键词兜底，避免整轮对话丢掉。"""
    if not question:
        return "chat"
    keywords = ("删除", "删掉", "移除", "修改", "批量", "添加", "设置", "清空")
    for word in keywords:
        if word in question:
            return "task"
    return "chat"


def _extract_route(text: str) -> str:
    """从模型输出里抠出 route，允许模型多话，取第一个命中的关键词。"""
    if not text:
        return ""
    low = text.lower()
    names = ("imagine", "task", "chat", "end")
    for name in names:
        if name in low:
            return name
    return ""


def _extract_json(text: str) -> dict:
    """从模型输出里抠出第一个 JSON 对象，抠不到返回空 dict。"""
    if not text:
        return {}

    raw = text.strip()

    # 去掉 ```json ... ``` 这种包裹
    if raw.startswith("```"):
        raw = raw.strip("`")
        lines = raw.split("\n", 1)
        if len(lines) == 2:
            raw = lines[1]

    start = raw.find("{")
    end = raw.rfind("}")
    if start == -1:
        return {}
    if end <= start:
        return {}

    try:
        data = json.loads(raw[start:end + 1])
    except json.JSONDecodeError:
        return {}

    if isinstance(data, dict):
        return data
    return {}


# ---------------------------------------------------------------- supervisor

async def supervisor_node(state: AgentState) -> dict:
    """LLM 意图分类，本身不产出用户可见文本。"""
    last = _last_human(state)
    if last is None:
        return {"route": "end"}

    has_img = _has_image(last.content)
    question = _text_of(last.content)

    fallback = _keyword_route(question)

    if has_img:
        img_hint = "包含"
    else:
        img_hint = "不包含"

    if question:
        human_text = question
    else:
        human_text = "（用户只发了图片，没有说话）"

    try:
        reply = await supervisor_llm.ainvoke([
            ("system",
             "你是路由助手。判断用户最新一条消息该交给哪个专家，只做分类，不要回答问题。\n"
             "可选值只有四个：chat / imagine / task / end\n"
             "- chat：日常闲聊、情绪倾诉、知识问答，不需要改动任何数据\n"
             "- imagine：用户提供了图片，需要看图才能回答\n"
             "- task：要改动数据或调用软件功能（删除好友、修改关系、批量操作等）\n"
             "- end：空消息或无法理解的乱码\n"
             f"当前消息{img_hint}图片。\n"
             "输出格式：只输出一行 `route: 值`，不要解释，不要输出其它任何内容。"),
            ("human", human_text),
        ])
        route = _extract_route(_text_of(reply.content))
        if not route:
            route = fallback
    except Exception:
        # 分类失败退化到关键词兜底，不让整轮对话失败
        route = fallback

    # 带图却没被判成 task 的一律走图片专家：用户既然发了图，就是要你看图
    if has_img and route == "chat":
        route = "imagine"

    return {"route": route}


def route_after_supervisor(state: AgentState) -> str:
    route = state.get("route")
    if not route:
        return END
    if route == "chat" or route == "imagine" or route == "task":
        return route
    return END


# ---------------------------------------------------------------- 专家节点

def make_expert_node(agent, name: str):
    """把 create_agent 编成的子图包成节点。

    用函数包一层而不是直接把子图当节点，是为了让状态更新可控：
    只把最后一条 AIMessage 合并回父图，避免中间工具消息污染会话历史。
    """
    async def node(state: AgentState) -> dict:
        result = await agent.ainvoke({"messages": state["messages"]})
        return {"messages": [result["messages"][-1]]}

    node.__name__ = f"{name}_node"
    return node


async def _plan_with_tools(history, config):
    """让模型做规划，期间允许它自己调只读工具。

    做成小循环而不是交给 create_agent，是为了三件事：
    1. 限制轮数，防止模型反复查同一个名字打转；
    2. 工具执行时的 config 由我们自己传，user_id 才能带到库里；
    3. 最后一定要收敛成纯文本 JSON，不能被工具调用半路截断。

    除了最终回复，还吐出「工具名 -> 最后一次返回」的字典。上层要用它做
    确定性校验：有副作用的指令只能基于工具真的备好的数据生成，不能让模型
    凭记忆重写一遍。
    """
    messages = list(history)
    tool_results = {}

    for _round in range(MAX_TOOL_ROUNDS):
        reply = await task_planner.ainvoke(messages, config)
        tool_calls = getattr(reply, "tool_calls", None)
        if not tool_calls:
            return reply, tool_results

        messages.append(reply)
        for call in tool_calls:
            name = call.get("name")
            args = call.get("args")
            call_id = call.get("id")
            if not isinstance(args, dict):
                args = {}

            target = TASK_TOOL_MAP.get(name)
            if target is None:
                content = f"没有名为 {name} 的工具，不要再用它。"
            else:
                try:
                    content = await target.ainvoke(args, config)
                except Exception as exc:  # noqa: BLE001
                    content = f"工具 {name} 执行失败：{exc}"
                if not isinstance(content, str):
                    content = str(content)
                tool_results[name] = content

            messages.append(ToolMessage(content=content, tool_call_id=call_id))

    # 轮数用完还没收敛，最后强制要一次纯文本，不给它再调工具的机会
    return await model.ainvoke(messages), tool_results


def _attach_forward_packet(command: dict, tool_results: dict) -> dict:
    """把 forward_message 备好的包原样塞进指令，包不齐就整条作废。

    为什么覆盖而不是让模型自己填：sendTime 必须由工具取（模型没有时钟，
    填出来的时间是编的），sendId 必须由服务端取（模型不该知道当前用户是谁）。
    让模型转抄一遍，它随时可能把时间改掉、把账号抄错，而这种错误会直接
    发到真实好友那里。所以包只从工具结果里取，取不到就不弹审批框。
    """
    if not command:
        return command
    if command.get("action") != "agent_send_message":
        return command

    raw = tool_results.get("forward_message")
    if not isinstance(raw, str):
        return {}

    try:
        packet = json.loads(raw)
    except ValueError:
        return {}
    if not isinstance(packet, dict):
        return {}
    if packet.get("type") != "agent_send_message":
        return {}

    command["params"] = packet
    return command


async def task_expert_node(state: AgentState, config) -> dict:
    """任务专家，两段式：先规划，批准后才产出可执行指令。"""
    if state.get("approval_status") == "approved":
        # 第二段：用户已批准。真正的副作用由客户端收到 task_execute 后执行，
        # 服务端只负责把指令和确认文案回写进会话。
        summary = state.get("plan_summary")
        if not summary:
            summary = "操作已提交执行。"
        command = state.get("task_command")
        if not command:
            command = {}
        return {
            "messages": [AIMessage(content=summary)],
            "task_command": command,
        }

    # 第一段：规划，不执行。只把人类与助手的对话给模型，不递工具消息。
    history = [("system", TASK_PROMPT)]
    for msg in state["messages"]:
        if isinstance(msg, HumanMessage):
            history.append(msg)
        elif isinstance(msg, AIMessage):
            history.append(msg)

    try:
        reply, tool_results = await _plan_with_tools(history, config)
        raw = _text_of(reply.content)
        plan = _extract_json(raw)

        summary = str(plan.get("summary", ""))
        if not summary:
            summary = raw.strip()
        if not summary:
            summary = "请确认是否执行该操作。"

        steps = []
        raw_steps = plan.get("steps", [])
        if isinstance(raw_steps, list):
            for step in raw_steps:
                steps.append(str(step))

        # 动作名与风险等级由 tool.py 白名单校验，模型编的动作会直接作废
        command = normalize_task_command(plan.get("command"), plan.get("risk"))
        # 带副作用的指令只认工具真的备好的数据，不给模型转抄的机会
        command = _attach_forward_packet(command, tool_results)

        return {
            "messages": [AIMessage(content=summary)],
            "plan_summary": summary,
            "plan_steps": steps,
            "task_command": command,
            "approval_status": "",
        }
    except Exception as exc:  # noqa: BLE001
        return {
            "messages": [AIMessage(content=f"抱歉，我暂时没法整理这个操作：{exc}")],
            "approval_status": "rejected",
        }


def route_after_task(state: AgentState) -> str:
    """未表态就送去审批；已批准（执行完）或已拒绝都要去做结果播报。

    还有一种情况：规划阶段根本没收敛出可执行指令（比如查不到好友、或者
    好友同名需要用户先确认）。这时候没什么可审批的，硬弹审批框只会让用户
    对一个空任务点「同意」，所以直接去播报，把追问的话交给用户。
    """
    status = state.get("approval_status")
    if status == "approved" or status == "rejected":
        return "report_expert"

    command = state.get("task_command")
    if not command:
        return "report_expert"

    return "examine_expert"


async def examine_expert_node(state: AgentState) -> dict:
    """HITL 审批节点：interrupt() 挂起，等客户端回传决定后恢复。

    interrupt 的入参会原样出现在 StateSnapshot 的 interrupts 里，同时被
    checkpointer 持久化；客户端断开重连后仍能用同一 thread_id 续上。
    """
    decision = interrupt({
        "event": "approval_required",
        "summary": state.get("plan_summary", ""),
        "steps": state.get("plan_steps", []),
        "command": state.get("task_command", {}),
    })

    if isinstance(decision, dict):
        approved = bool(decision.get("approved"))
    else:
        approved = bool(decision)

    if approved:
        return {"approval_status": "approved"}
    return {"approval_status": "rejected"}


def route_after_examine(state: AgentState) -> str:
    if state.get("approval_status") == "approved":
        return "task_expert"
    return "report_expert"


def _describe_command(command: dict) -> str:
    """把指令转成一句人话，模型不可用时给播报节点兜底。"""
    if not isinstance(command, dict):
        return ""

    action = command.get("action")
    if not isinstance(action, str):
        action = ""

    params = command.get("params")
    if not isinstance(params, dict):
        params = {}

    params_text = ""
    for key, value in params.items():
        if params_text:
            params_text += "，"
        params_text += str(key) + "=" + str(value)

    if not action:
        return params_text
    if not params_text:
        return action
    return action + "（" + params_text + "）"


async def report_expert_node(state: AgentState) -> dict:
    """任务结果播报：把「干了什么、现在到哪一步」说给用户听。

    以前任务链路跑完只有一条 task_execute 事件，用户界面上什么都没有，
    所以这里补一个纯文本节点。它不碰任何数据，只负责把状态翻译成一句人话。
    """
    status = state.get("approval_status")

    summary = state.get("plan_summary")
    if not summary:
        summary = ""

    steps = state.get("plan_steps")
    if not steps:
        steps = []

    command = state.get("task_command")
    if not isinstance(command, dict):
        command = {}

    command_text = _describe_command(command)

    if status == "approved" and command:
        status_text = "已按计划下发执行，实际结果等客户端执行后反馈"
    elif status == "approved":
        status_text = "已批准，但没能生成可执行的指令，这次什么也没改"
    elif status == "rejected":
        status_text = "已取消，没有改动任何数据"
    else:
        status_text = "流程结束，没有下发任何操作"

    facts = "审批结果：" + status_text + "\n"
    facts += "原计划：" + (summary if summary else "（无）") + "\n"
    facts += "执行步骤：" + ("；".join(steps) if steps else "（无）") + "\n"
    facts += "指令内容：" + (command_text if command_text else "（无）")

    text = ""
    try:
        reply = await model.ainvoke([
            ("system", REPORT_PROMPT),
            ("human", facts),
        ])
        text = _text_of(reply.content).strip()
    except Exception:
        # 模型不可用也不能让用户什么都收不到
        text = ""

    if not text:
        text = "这次任务" + status_text + "。"
        if command_text:
            text += "操作内容是 " + command_text + "。"

    return {"messages": [AIMessage(content=text)]}


# ---------------------------------------------------------------- 建图

builder = StateGraph(AgentState)
builder.add_node("supervisor", supervisor_node)
builder.add_node("chat_expert", make_expert_node(chat_expert, "chat_expert"))
builder.add_node("imagine_expert", make_expert_node(imagine_expert, "imagine_expert"))
builder.add_node("task_expert", task_expert_node)
builder.add_node("examine_expert", examine_expert_node)
builder.add_node("report_expert", report_expert_node)

builder.add_edge(START, "supervisor")
builder.add_conditional_edges(
    "supervisor",
    route_after_supervisor,
    {"chat": "chat_expert", "imagine": "imagine_expert", "task": "task_expert", END: END},
)
builder.add_edge("chat_expert", END)
builder.add_edge("imagine_expert", END)
builder.add_conditional_edges(
    "task_expert",
    route_after_task,
    {"examine_expert": "examine_expert", "report_expert": "report_expert"},
)
builder.add_conditional_edges(
    "examine_expert",
    route_after_examine,
    {"task_expert": "task_expert", "report_expert": "report_expert"},
)
builder.add_edge("report_expert", END)

_compiled_graph = None


async def get_supervisor_graph():
    global _compiled_graph  # 声明成全局变量
    if _compiled_graph is None:
        cp = await get_checkpointer()  # await 不能在顶层使用
        _compiled_graph = builder.compile(checkpointer=cp)
    return _compiled_graph



# ---------------------------------------------------------------- 流式输出

# 允许把 token 吐给客户端的节点。
# - supervisor：只输出路由判断，不能给用户看
# - task_expert：输出的是内部规划 JSON，用户只看 approval_required 里的 summary
# - report_expert：任务链路的人话收尾，必须给用户看
VISIBLE_NODES = {"chat_expert", "imagine_expert", "report_expert"}


def _event_node(event: dict) -> str | None:
    """取出事件所属的父图节点名。
`langgraph_node` 只会返回最内层的 `model`，三个专家都叫 `model`，你根本不知道当前 token 是谁输出的。用命名空间前缀是官方推荐的最稳方式。
    metadata.langgraph_node 在嵌套子图里只给出最内层节点（如 "model"），
    多个专家就分不清了；而 langgraph_checkpoint_ns 形如

        chat_expert:uuid|model:uuid

    第一段正是父图里的节点名，用它判别最稳。
    """#LangGraph 嵌套子图执行时，会生成一个命名空间字段 `langgraph_checkpoint_ns`，格式是：父图节点名:uuid|子图节点名:uuid|孙图节点名:uuid
    #比如聊天专家生成 token 时，这个字段长这样：chat_expert:abc123|model:def456
    md = event.get("metadata")
    if not md:
        md = {}

    ns = md.get("langgraph_checkpoint_ns")
    if ns:
        head = ns.split("|", 1)[0]
        return head.split(":", 1)[0]

    return md.get("langgraph_node")


def _chunk_text(chunk) -> str:
    """从模型流式块里取纯文本。
  作用：从模型流式块（chunk）里把纯文本抠出来。
- 普通对话：`content` 是字符串，直接返回。
- 多模态对话：`content` 是 `[{"type":"text"}, {"type":"image_url"}]` 列表，只提取文本部分，忽略图片。
- 容错兜底：遇到奇怪类型，转成字符串拼进去，绝不报错。"""
    content = getattr(chunk, "content", None)
    if not content:
        return ""
    if isinstance(content, str):
        return content

    text = ""
    for piece in content:
        if isinstance(piece, dict):
            text += piece.get("text", "")
        else:
            text += str(piece)
    return text


# route 名 → 节点名，保证 token / done 事件里的 agent 字段用同一套词汇
ROUTE_TO_NODE = {
    "chat": "chat_expert",
    "imagine": "imagine_expert",
    "task": "task_expert",
}


def _config_for(thread_id: str, user_id=None) -> dict:
    """每次执行图都必须带的两个配置：
- `thread_id`：Checkpoint 的会话标识，同一个 id 共享状态、中断可恢复。
- `recursion_limit`：递归深度限制，防止图死循环（比如任务→审批→任务的循环）把服务跑挂。

额外的 `user_id` 不属于 LangGraph，是我们自己塞进去的：
工具里需要知道「当前登录用户是谁」才能查库，而模型是不可能知道这个值的，
所以从请求一路带到这里，工具再用 resolve_own_id 取出来。
"""
    configurable = {"thread_id": thread_id}
    if user_id is not None:
        configurable["user_id"] = user_id

    return {
        "configurable": configurable,
        "recursion_limit": MAX_RECURSION,
    }


def _collect_interrupts(snapshot):
    """把 snapshot 上的 interrupt 收成一个列表。
    不同版本可能挂在 snapshot.interrupts 上，也可能挂在每个 task 上，两种都兜住。
    LangGraph 不同版本里，中断信息可能挂在 `snapshot.interrupts`，也可能挂在每个 `task.interrupts` 上。
    """
    found = []

    top = getattr(snapshot, "interrupts", None)
    if top:
        for item in top:
            found.append(item)

    if found:
        return found

    tasks = getattr(snapshot, "tasks", None)
    if not tasks:
        return found

    for task in tasks:
        task_interrupts = getattr(task, "interrupts", None)
        if not task_interrupts:
            continue
        for item in task_interrupts:
            found.append(item)
    return found


async def _emit_state_effects(graph, config):
    """图跑完后回读状态，把「待审批」「待执行」交给客户端。记住一个原则：**所有非流式的业务信号，都在图跑完后通过读状态来生成。
    执行到interrupt() 时，图会立刻暂停，astream_events的迭代也就到此为止.它不会主动抛出一个「我中断了」的事件，需要你等流式结束后，主动回读一次状态快照，才能知道图是不是停在了审批点。"""
    snapshot = await graph.aget_state(config)
    values = snapshot.values
    if not values:
        values = {}

    interrupts = _collect_interrupts(snapshot)
    if interrupts:
        payload = interrupts[0].value
        if isinstance(payload, dict):
            event = {"event": "approval_required"}
            for key, value in payload.items():
                event[key] = value
            yield event
        else:
            yield {"event": "approval_required", "summary": str(payload)}
        return

    status = values.get("approval_status")
    if status == "approved":
        command = values.get("task_command")
        if command:
            yield {"event": "task_execute", "command": command}
        return
    if status == "rejected":
        yield {"event": "task_rejected"}


async def _run_graph(graph, graph_input, config):
    """跑一遍图，产出 token 事件 + 收尾事件。"""
    # report_expert 的播报先攒着：如果它先吐出来，客户端就会先看到
    # 「指令已经下发」，之后才收到真正的 task_execute，顺序是反的。
    report_tokens = []

    async for event in graph.astream_events(graph_input, config=config, version="v2"):
        if event.get("event") != "on_chat_model_stream":#**事件类型过滤**：只留 `on_chat_model_stream`（模型生成 token 的事件），工具调用、节点开始 / 结束全部丢掉。
            continue
        node = _event_node(event)#用 `_event_node` 算出属于哪个父图节点。
        if node not in VISIBLE_NODES:#不在 `VISIBLE_NODES` 里的直接丢弃（比如 supervisor、task_expert 的内部模型调用）。
            continue

        data = event.get("data")
        if not data:
            continue
        text = _chunk_text(data.get("chunk"))#从 chunk 里抠出纯文本，空内容跳过。
        if not text:
            continue

        if node == "report_expert":
            report_tokens.append({"event": "token", "agent": node, "content": text})
            continue
        yield {"event": "token", "agent": node, "content": text}

    async for item in _emit_state_effects(graph, config):#所有模型 token 吐完、图执行暂停 / 结束后，调用 `_emit_state_effects`，把审批、执行这类状态型事件吐出去。
        yield item

    # 执行类事件发完之后再补上任务播报
    for item in report_tokens:
        yield item


def build_human_content(message: str, images: list[str] | None = None):
    """把文本 + 图片组装成 HumanMessage 支持的 content。"""
    if not images:
        return message

    parts = []
    if message:
        parts.append({"type": "text", "text": message})

    for img in images:
        if img.startswith("http://") or img.startswith("https://") or img.startswith("data:"):
            url = img
        else:
            url = "data:image/jpeg;base64," + img
        parts.append({"type": "image_url", "image_url": {"url": url}})

    return parts


async def chat_stream(thread_id: str, user_message: str, images: list[str] | None = None,
                      user_id: int | None = None):#新消息入口 `chat_stream`
    """处理一条新消息，逐 token 产出 SSE 事件字典。

    事件类型：token / approval_required / task_execute / task_rejected / done
    """
    graph = await get_supervisor_graph()
    config = _config_for(thread_id, user_id)
# 1. 组装用户消息
    content = build_human_content(user_message, images)
    payload = {"messages": [HumanMessage(content=content)]}
# 2. 跑图，产出 token + 业务事件
    # 一边转发一边记住最后是哪个节点在说话，done 事件据此收尾
    last_agent = ""
    async for item in _run_graph(graph, payload, config):
        if item.get("event") == "token":
            last_agent = item.get("agent", "")
        yield item
# 3. 最终发出 done 事件
    snapshot = await graph.aget_state(config)
    values = snapshot.values
    if values:
        route = values.get("route", "")
    else:
        route = ""

    if last_agent:
        agent_name = last_agent
    elif route in ROUTE_TO_NODE:
        agent_name = ROUTE_TO_NODE[route]
    else:
        agent_name = route

    yield {"event": "done", "agent": agent_name}


async def resume_stream(thread_id: str, approved: bool, note: str = "",
                        user_id: int | None = None):
    """审批后续跑：用 Command(resume=...) 恢复被 interrupt 挂起的会话。
    **前置校验**：先读状态，确认这个会话确实停在审批点，避免重复提交、无效调用。
    **`Command(resume=...)`**：LangGraph 的标准恢复方式，把审批结果传给 `interrupt()` 的返回值，图就会从挂起的位置继续往下走。
     **复用 `_run_graph`**：恢复执行的流式输出、收尾事件逻辑和新消息完全一致，不用写两套。"""
    graph = await get_supervisor_graph()
    config = _config_for(thread_id, user_id)

# 1. 前置校验：有没有待审批的任务
    snapshot = await graph.aget_state(config)
    has_next = bool(getattr(snapshot, "next", None))
    has_interrupt = bool(_collect_interrupts(snapshot))
    if not has_next and not has_interrupt:
        yield {"event": "error", "message": "该会话没有待审批的任务"}
        return
# 2. 用 Command 恢复执行
    command = Command(resume={"approved": approved, "note": note})
    last_agent = ""
    async for item in _run_graph(graph, command, config):
        if item.get("event") == "token":
            last_agent = item.get("agent", "")
        yield item

    if not last_agent:
        last_agent = "task_expert"
    yield {"event": "done", "agent": last_agent}