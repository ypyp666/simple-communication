"""多智能体路由图：Supervisor 分发 + 三个专家 + HITL 审批。

拓扑（与架构图一致）：

    START → supervisor ─┬→ chat_expert    ─────────────────→ END
                        ├→ imagine_expert ─────────────────→ END
                        └→ task_expert ─→ examine_expert(HITL) ─┬→ task_expert(执行) → END
                                                  ↑              └→ END（拒绝/超时）

要点：
1. supervisor 只做意图分类，自己不产出用户可见文本，流式输出时必须过滤掉。
2. task_expert 是两段式节点：
   - approval_status != "approved" → 规划阶段，只生成方案，不执行
   - approval_status == "approved" → 执行阶段，产出可交给客户端执行的指令
3. examine_expert 用 interrupt() 挂起，checkpointer 持久化 thread_id，
   客户端批准后通过 Command(resume=...) 恢复同一条会话。
"""
from typing import Literal

import json

import aiosqlite

from langchain.agents import create_agent
from langchain_core.messages import AIMessage, HumanMessage
from langchain_openai import ChatOpenAI
from pydantic import BaseModel, Field

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
from main.tool import web_searchquery

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


class RouteDecision(BaseModel):
    """supervisor 的结构化输出。"""
    route: Literal["chat", "imagine", "task", "end"] = Field(
        description="chat=普通闲聊/情绪对话；imagine=需要看图片；"
                    "task=要改动数据或调用软件功能；end=无需回复"
    )
    reason: str = Field(default="", description="一句话说明判断依据")


class TaskPlan(BaseModel):
    """task_expert 规划阶段的结构化输出。"""
    summary: str = Field(description="向用户确认这次要做什么，口语化一句话")
    steps: list[str] = Field(default_factory=list, description="将执行的操作，按顺序")
    risk: Literal["low", "medium", "high"] = Field(
        default="medium", description="风险等级，high 表示不可逆操作"
    )
    command: dict = Field(
        default_factory=dict,
        description='交给客户端执行的指令，如 {"action":"delete_friend","params":{...}}',
    )


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

chat_expert=create_agent(
    model=model,
    tools=[web_searchquery],
    system_prompt="""
你是一个情感专家，你最擅长的就是与人沟通，你的沟通技巧贴合实际懂得安抚别人的情绪，别人说的话你都能接住，你是用户最喜爱的聊天助手，当遇到不理解的东西时你会
使用 Deepseek 搜索工具
###注意：你的回答必须贴合口语化贴合真人，但是遇到以下违规行为禁止回答：涉及色情、暴力、政治敏感、违法犯罪、诈骗、赌博、恐怖主义、极端主义、仇恨言论、歧视言论等内容,违反中国大陆法律法规的内容禁止回答
##此外补充涉及宗教敏感和政治敏感内容禁止回答，涉及歧视问题的内容（包括性别，地域，民族和群体）禁止回答，涉及未成年人不良信息的内容禁止回答，涉及医疗健康、金融投资、法律咨询等专业领域的内容禁止回答，涉及个人隐私和个人信息的内容禁止回答，涉及虚假信息和谣言的内容禁止回答，涉及广告营销和商业推广的内容禁止回答，涉及侵权盗版和知识产权的内容禁止回答，涉及网络安全和信息安全的内容禁止回答
#最后禁止回答的内容统一回复：对不起，我无法回答这个问题。请换一个话题。
"""
)

IMAGINE_PROMPT = """
你是图像理解专家。用户会发来图片，可能附带一句说明。
请完成：画面内容识别、文字/表格提取、关键信息归纳，并结合用户的问题给出结论。
如果图片模糊、信息不足或与问题无关，直接说明看不到什么，不要编造细节。
回答使用简体中文，先给结论，再给必要的细节，不要输出与图片无关的联想。
"""

TASK_PROMPT = """
你是任务专家，负责理解用户想对软件执行的**有副作用的操作**（例如删除好友、修改关系、批量处理）。
你的职责边界：
- 只做意图识别与方案拆解，不要假装已经执行。
- 如果用户意图含糊、缺少必要参数（对象、数量、范围），不要猜，直接在 summary 里向用户追问。
- 高风险或不可逆操作（删除、清空、批量修改）标记 risk="high"。
- command 里放结构化指令，动作名用小写下划线，参数放在 params 中。

输出要求：只输出一个 JSON 对象，不要输出解释、不要加 markdown 代码块，字段如下：
{"summary": "向用户确认这次要做什么，口语化一句话",
 "steps": ["第一步", "第二步"],
 "risk": "low|medium|high",
 "command": {"action": "delete_friend", "params": {"name": "张三"}}}
"""


# chat_expert 的定义见下方（沿用原有 system_prompt）
imagine_expert = create_agent(
    model=vision_model,
    system_prompt=IMAGINE_PROMPT,
    name="imagine_expert",
)

# 注意：本网关不支持 response_format=json_schema（with_structured_output 会直接 400），
# 所以结构化信息统一走「纯文本输出 + 自己解析」这条最稳的路。
supervisor_llm = model
task_planner = model


# ---------------------------------------------------------------- 小工具

def _content_to_parts(content) -> list[dict]:
    """把 message.content 统一成 list[dict]，方便判断图片与取文本。"""
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    if isinstance(content, list):
        return [p if isinstance(p, dict) else {"type": "text", "text": str(p)} for p in content]
    return []


def _has_image(content) -> bool:
    return any(p.get("type") == "image_url" for p in _content_to_parts(content))


def _text_of(content) -> str:
    return "".join(p.get("text", "") for p in _content_to_parts(content) if p.get("type") == "text")


def _last_human(state: AgentState) -> HumanMessage | None:
    for msg in reversed(state["messages"]):
        if isinstance(msg, HumanMessage):
            return msg
    return None


def _keyword_route(question: str) -> str:
    """模型分类失败时的关键词兜底，避免整轮对话丢掉。"""
    q = question or ""
    if any(k in q for k in ("删除", "删掉", "移除", "修改", "批量", "添加", "设置", "清空")):
        return "task"
    return "chat"


def _extract_route(text: str) -> str:
    """从模型输出里抠出 route，允许模型多话，取第一个命中的关键词。"""
    low = (text or "").lower()
    for name in ("imagine", "task", "chat", "end"):
        if name in low:
            return name
    return ""


def _extract_json(text: str) -> dict:
    """从模型输出里抠出第一个 JSON 对象，抠不到返回空 dict。"""
    raw = (text or "").strip()
    if raw.startswith("```"):  # 去掉 ```json ... ``` 包裹
        raw = raw.strip("`")
        raw = raw.split("\n", 1)[-1] if raw.lower().startswith("json") else raw
    start, end = raw.find("{"), raw.rfind("}")
    if start == -1 or end <= start:
        return {}
    try:
        data = json.loads(raw[start:end + 1])
        return data if isinstance(data, dict) else {}
    except json.JSONDecodeError:
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
    try:
        reply = await supervisor_llm.ainvoke([
            ("system",
             "你是路由助手。判断用户最新一条消息该交给哪个专家，只做分类，不要回答问题。\n"
             "可选值只有四个：chat / imagine / task / end\n"
             "- chat：日常闲聊、情绪倾诉、知识问答，不需要改动任何数据\n"
             "- imagine：用户提供了图片，需要看图才能回答\n"
             "- task：要改动数据或调用软件功能（删除好友、修改关系、批量操作等）\n"
             "- end：空消息或无法理解的乱码\n"
             f"当前消息{'包含' if has_img else '不包含'}图片。\n"
             "输出格式：只输出一行 `route: 值`，不要解释，不要输出其它任何内容。"),
            ("human", question or "（用户只发了图片，没有说话）"),
        ])
        route = _extract_route(_text_of(reply.content)) or fallback
    except Exception:
        # 分类失败退化到关键词兜底，不让整轮对话失败
        route = fallback

    # 带图却没被判成 task 的一律走图片专家：用户既然发了图，就是要你看图
    if has_img and route == "chat":
        route = "imagine"

    return {"route": route}


def route_after_supervisor(state: AgentState) -> str:
    route = state.get("route") or "end"
    return route if route in {"chat", "imagine", "task"} else END


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


async def task_expert_node(state: AgentState) -> dict:
    """任务专家，两段式：先规划，批准后才产出可执行指令。"""
    if state.get("approval_status") == "approved":
        # 第二段：用户已批准。真正的副作用由客户端收到 task_execute 后执行，
        # 服务端只负责把指令和确认文案回写进会话。
        return {
            "messages": [AIMessage(content=state.get("plan_summary") or "操作已提交执行。")],
            "task_command": state.get("task_command") or {},
        }

    # 第一段：规划，不执行
    history = [m for m in state["messages"] if isinstance(m, (HumanMessage, AIMessage))]
    try:
        reply = await task_planner.ainvoke([("system", TASK_PROMPT), *history])
        raw = _text_of(reply.content)
        plan = _extract_json(raw)
        summary = str(plan.get("summary") or raw.strip() or "请确认是否执行该操作。")
        steps = [str(s) for s in (plan.get("steps") or [])]
        risk = str(plan.get("risk") or "medium")
        command = plan.get("command") if isinstance(plan.get("command"), dict) else {}
        return {
            "messages": [AIMessage(content=summary)],
            "plan_summary": summary,
            "plan_steps": steps,
            "task_command": {**command, "risk": risk},
            "approval_status": "",
        }
    except Exception as exc:  # noqa: BLE001
        return {
            "messages": [AIMessage(content=f"抱歉，我暂时没法整理这个操作：{exc}")],
            "approval_status": "rejected",
        }


def route_after_task(state: AgentState) -> str:
    """未表态就送去审批；已批准（执行完）或已拒绝都直接收尾。"""
    if state.get("approval_status") in {"approved", "rejected"}:
        return END
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

    return {"approval_status": "approved" if approved else "rejected"}


def route_after_examine(state: AgentState) -> str:
    return "task_expert" if state.get("approval_status") == "approved" else END


# ---------------------------------------------------------------- 建图

builder = StateGraph(AgentState)
builder.add_node("supervisor", supervisor_node)
builder.add_node("chat_expert", make_expert_node(chat_expert, "chat_expert"))
builder.add_node("imagine_expert", make_expert_node(imagine_expert, "imagine_expert"))
builder.add_node("task_expert", task_expert_node)
builder.add_node("examine_expert", examine_expert_node)

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
    {"examine_expert": "examine_expert", END: END},
)
builder.add_conditional_edges(
    "examine_expert",
    route_after_examine,
    {"task_expert": "task_expert", END: END},
)

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
VISIBLE_NODES = {"chat_expert", "imagine_expert"}


def _event_node(event: dict) -> str | None:
    """取出事件所属的父图节点名。

    metadata.langgraph_node 在嵌套子图里只给出最内层节点（如 "model"），
    多个专家就分不清了；而 langgraph_checkpoint_ns 形如

        chat_expert:uuid|model:uuid

    第一段正是父图里的节点名，用它判别最稳。
    """
    md = event.get("metadata") or {}
    ns = md.get("langgraph_checkpoint_ns") or ""
    if ns:
        return ns.split("|", 1)[0].split(":", 1)[0]
    return md.get("langgraph_node")


def _chunk_text(chunk) -> str:
    content = getattr(chunk, "content", None)
    if not content:
        return ""
    if isinstance(content, str):
        return content
    return "".join(
        piece.get("text", "") if isinstance(piece, dict) else str(piece)
        for piece in content
    )


# route 名 → 节点名，保证 token / done 事件里的 agent 字段用同一套词汇
ROUTE_TO_NODE = {
    "chat": "chat_expert",
    "imagine": "imagine_expert",
    "task": "task_expert",
}


def _config_for(thread_id: str) -> dict:
    return {
        "configurable": {"thread_id": thread_id},
        "recursion_limit": MAX_RECURSION,
    }


async def _emit_state_effects(graph, config):
    """图跑完后回读状态，把「待审批」「待执行」交给客户端。"""
    snapshot = await graph.aget_state(config)
    values = snapshot.values or {}

    # interrupt 在 1.x 里可能挂在 snapshot 上，也可能挂在每个 task 上，两种都兜住
    interrupts = getattr(snapshot, "interrupts", None) or tuple(
        i for task in (getattr(snapshot, "tasks", None) or ())
        for i in getattr(task, "interrupts", ())
    )
    if interrupts:
        payload = interrupts[0].value
        if isinstance(payload, dict):
            yield {**payload, "event": "approval_required"}
        else:
            yield {"event": "approval_required", "summary": str(payload)}
        return

    status = values.get("approval_status")
    if status == "approved" and values.get("task_command"):
        yield {"event": "task_execute", "command": values["task_command"]}
    elif status == "rejected":
        yield {"event": "task_rejected"}


async def _run_graph(graph, graph_input, config):
    """跑一遍图，产出 token 事件 + 收尾事件。"""
    async for event in graph.astream_events(graph_input, config=config, version="v2"):
        if event.get("event") != "on_chat_model_stream":
            continue
        node = _event_node(event)
        if node not in VISIBLE_NODES:
            continue
        text = _chunk_text((event.get("data") or {}).get("chunk"))
        if text:
            yield {"event": "token", "agent": node, "content": text}

    async for item in _emit_state_effects(graph, config):
        yield item


def build_human_content(message: str, images: list[str] | None = None):
    """把文本 + 图片组装成 HumanMessage 支持的 content。"""
    if not images:
        return message
    parts: list[dict] = []
    if message:
        parts.append({"type": "text", "text": message})
    for img in images:
        url = img if img.startswith(("http://", "https://", "data:")) else f"data:image/jpeg;base64,{img}"
        parts.append({"type": "image_url", "image_url": {"url": url}})
    return parts


async def chat_stream(thread_id: str, user_message: str, images: list[str] | None = None):
    """处理一条新消息，逐 token 产出 SSE 事件字典。

    事件类型：token / approval_required / task_execute / task_rejected / done
    """
    graph = await get_supervisor_graph()
    config = _config_for(thread_id)
    payload = {"messages": [HumanMessage(content=build_human_content(user_message, images))]}

    async for item in _run_graph(graph, payload, config):
        yield item

    snapshot = await graph.aget_state(config)
    route = (snapshot.values or {}).get("route", "")
    yield {"event": "done", "agent": ROUTE_TO_NODE.get(route, route)}


async def resume_stream(thread_id: str, approved: bool, note: str = ""):
    """审批后续跑：用 Command(resume=...) 恢复被 interrupt 挂起的会话。"""
    graph = await get_supervisor_graph()
    config = _config_for(thread_id)

    snapshot = await graph.aget_state(config)
    if not (getattr(snapshot, "next", None) or getattr(snapshot, "interrupts", None)):
        yield {"event": "error", "message": "该会话没有待审批的任务"}
        return

    command = Command(resume={"approved": approved, "note": note})
    async for item in _run_graph(graph, command, config):
        yield item
    yield {"event": "done", "agent": "task_expert"}