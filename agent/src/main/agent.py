import aiosqlite

from langchain.agents import create_agent
from langchain_core.messages import HumanMessage
from langchain_openai import ChatOpenAI

from langgraph.checkpoint.sqlite.aio import AsyncSqliteSaver
from langgraph.graph import END, START, MessagesState, StateGraph

# 共享配置统一从 config 取，tool.py 也从这里取，避免循环导入
from main.config import (
    DEEPSEEK_API_KEY,
    DEEPSEEK_BASE_URL,
    DEEPSEEK_MODEL,
    RESOURCES_DIR,
    require,
)
from main.tool import web_searchquery


model = ChatOpenAI(
    model=DEEPSEEK_MODEL,
    api_key=require("DEEPSEEK_API_KEY", DEEPSEEK_API_KEY),
    base_url=DEEPSEEK_BASE_URL,
)


_asnyc_checkpointer = None
async def get_checkpointer():
    """返回可复用的 AsyncSqliteSaver 实例(首次调用时建表)。"""
    global _asnyc_checkpointer
    if _asnyc_checkpointer is None:
        db_path = str(RESOURCES_DIR / "personal_chief.db")
        connection = await aiosqlite.connect(db_path)
        _asnyc_checkpointer = AsyncSqliteSaver(connection)
        if hasattr(_asnyc_checkpointer, "setup"):
            await _asnyc_checkpointer.setup()
    return _asnyc_checkpointer

class SupervisorState(MessagesState):
    # 继承后自动拥有 messages 字段及其 add_messages 合并规则。
    # 这里只额外增加管理 agent 自己需要的 next 字段。
    next: str = ""  # 下一个要执行的 agent 名字,或 "FINISH"

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

def make_worker_node(agent_name, agent):
    async def node(state):
        # 用 ainvoke 而不是 invoke，避免同步阻塞 uvicorn 的事件循环
        result = await agent.ainvoke({"messages": state["messages"]})
        # 这里返回的是"状态更新",不是完整的新状态。
        # 因为 messages 字段绑定了 add_messages 归约器,LangGraph 会把这条新消息
        # 合并到原有历史末尾;我们只返回最后一条,避免把 worker 的内部中间消息重复写回。
        return {"messages": [result["messages"][-1]]}

    return node

# 注意：checkpointer 由外层 builder.compile() 统一挂载，这里不要再传，
# 否则内外两层都会做 checkpoint，thread_id 状态会错乱。
chat_agent = create_agent(
    model=model,
    tools=[web_searchquery],
)

builder = StateGraph(SupervisorState)
builder.add_node("chat_agent", make_worker_node("chat_agent", chat_agent))
builder.add_edge(START, "chat_agent")
builder.add_edge("chat_agent", END)

_compiled_supervisor_graph = None
async def get_supervisor_graph():
    global _compiled_supervisor_graph#声明成全局变量
    if _compiled_supervisor_graph is None:
        cp = await get_checkpointer()#await被禁止在顶层使用
        _compiled_supervisor_graph = builder.compile(checkpointer=cp)
    return _compiled_supervisor_graph



def _route_for(agent) -> str:
    """根据专家名给出用户可读的显示名。"""
    return {
        "chat": "chat",
    }.get(agent or "FINISH", str(agent or "FINISH"))

async def chat_stream(thread_id: str, user_message: str):
    """异步流式处理一条消息。

    逐 token 产出 {'event': ..., 'content': ...} 之类的字典:
      - {"event": "meta", "agent": ...}         开头的路由信息
      - {"event": "token", "content": "..."}    专家回复的片段
      - {"event": "done"}                       本轮结束
    """
    config = {
        "configurable": {"thread_id": thread_id},
        "recursion_limit": 10,
    }
    graph = await get_supervisor_graph()#获取全局唯一实例
    agent = "chat_agent"
    # 只有一个专家时，直接先告诉客户端是谁在回答
    yield {"event": "meta", "agent": _route_for(agent)}
    #异步跑事件，每个节点跑完会对外抛出事件接收
    async for event in graph.astream_events(
        {"messages": [HumanMessage(content=user_message)]},
        config=config,
        version="v2",
    ):
    #event的大概结构，event = {
    # "event": "on_chain_end",   # ✅【事件类型】这件事是什么
    #"metadata": {              # ✅【元信息】这件事发生在哪里
    #    "langgraph_node": "supervisor_node", # 来自哪个节点
    #    "thread_id": "xxx"
    #},
    #"data": {                  # ✅【这件事携带的数据】输出、返回值、token块
    #   "output": {"next":"chef"}
    #  }
    #}

        node = event.get("metadata", {}).get("langgraph_node") if event.get("metadata") else None
        name = event.get("event")
        if name == "on_chat_model_stream":
            # 注意：create_agent 内部调模型的节点名是 "model"，不是外层图里的 "chat_agent"。
            # 以后若加 supervisor 路由节点，这里再排除 supervisor 的判断输出即可。
            if node == "supervisor":
                continue
            chunk = event.get("data", {}).get("chunk")
            #从事件 data 拿到 `AIMessageChunk` 对象，这是 LangChain 封装的流式分片对象，不是普通字符串。
            content = getattr(chunk, "content", None) if chunk is not None else None
            #1. `content`存在两种格式：普通文本：`content = "番茄炒蛋怎么做"` 字符串 多模态返回：`content = [{"type":"text","text":"xxx"}, ...]` list 字典数组
            if content:
                text = content if isinstance(content, str) else "".join(
                    piece.get("text", "") if isinstance(piece, dict) else str(piece)
                    for piece in content
                #格式永远：【对每一项做计算的表达式】写在最前面，`for … in …`写在后面。
                )#如果是普通字符串，直接赋值给 text。
                #如果是多模态 list，遍历列表每一个 piece，取出 text 字段，全部拼接成普通字符串。
                if text:
                    yield {"event": "token", "content": text}

    yield {"event": "done", "agent": _route_for(agent)}