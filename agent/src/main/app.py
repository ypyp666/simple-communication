import asyncio
import json
from collections.abc import AsyncIterable, AsyncIterator
from typing import Any
from uuid import uuid4

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

from .agent import chat_stream, resume_stream

# 心跳间隔（秒）。必须明显小于 C++ 侧的低速超时窗口（30 秒），
# 留 6 倍余量：哪怕网络抖动、事件循环被别的请求挤住，也来得及补上下一拍。
KEEPALIVE_SEC = 5.0

app = FastAPI(title="Agent Backend", version="0.1.0")

# Qt 走 QNetworkAccessManager，本来不受同源策略限制；
# 这里留着是为了将来加 WebEngine/WebView 时不用再改。
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


class ChatRequest(BaseModel):
    message: str = Field(default="", max_length=4000)
    thread_id: str | None = None
    # 图片：支持 https URL、data:image/... 或裸 base64
    images: list[str] = Field(default_factory=list, max_length=4)
    # 当前登录用户的账号ID。工具查库时需要用，模型猜不出来。
    # 不传也不报错：服务端会从 thread_id（会话id + "n" + user_id）里解析。
    user_id: int | None = None


class ApproveRequest(BaseModel):
    thread_id: str
    approved: bool
    note: str = ""
    user_id: int | None = None


"""
异步迭代器解释：异步__aiter__()+ __anext__() + StopAsyncIteration  source.__aiter__()输入：一个**异步可迭代对象（AsyncIterable）
asyncio.Future：一个 “占位盒子”。盒子有三种状态：1. 未完成（pending）：结果还没来，等着2. 已完成（done）：已经有返回值，或者已经抛异常3. 取消（cancelled）：被手动 cancel () 了，你可以 await future，如果它还没完成，当前协程挂起，
asyncio.Task 是 Future 的子类，专门用来包装**协程**，丢进事件循环后台跑。- 一旦创建 Task，协程 `it.__anext__()` 就开始在事件循环里跑了！你可以在别的地方，后面再 `await task` 拿结果
asyncio.wait_for(aw, timeout)作用：等待单个可等待对象完成。规则：如果在 timeout 时间内完成：正常拿到结果，如果超时：自动调用 `aw` 对应的 task.cancel ()，然后抛出 TimeoutError**致命问题：我们的 `aw = it.__anext__()`，里面跑着 Agent、模型、搜索、HITL 等待。一旦超时，`wait_for` 直接 cancel 这个 Task → `__anext__()` 被打断 → LangGraph 的事件流直接终止，后面再也拿不到 token。
asyncio.wait(fs, timeout)：fs：一组 Future/Task 的集合（{task1, task2}）返回二元组 (done: 已完成任务集合, pending: 还在跑的任务集合)重点：超时的时候，它不会 cancel 任何 Task！它只是：“时间到了，我不等了，把还没跑完的任务还给你，它继续在后台跑。”
"""

async def with_keepalive(
    source: AsyncIterable[dict[str, Any]],
    interval: float = KEEPALIVE_SEC,
) -> AsyncIterator[dict[str, Any] | None]:#后面写的是这个函数执行完、往外产出什么东西。它**只是给人、给类型检查工具看的注释，
    #AsyncIterator[dict[str, Any] | None]意思是注解这个会返回的变量是异步迭代器包装的字典值，这个字典值的key为字符存，value为任何值也可能是None
    """给一个「黑盒」异步流套心跳：静默超过 interval 秒就吐一个 None。

    为什么叫黑盒：这个函数完全不认识 langgraph、不认识 SSE、也不认识 chat_stream，
    它只要求对方是个 async iterator。所以下游换模型、换框架、加节点，这里一行都不用改。

    要点一：不能用 asyncio.wait_for
        wait_for 超时时会 **cancel 掉** 里面那个 __anext__() 协程，
        而 async generator 一旦在 await 点被取消就直接终止了 —— 心跳还没发出去，
        整条回答先被我们自己的看门狗掐死了。

    要点二：task 必须跨轮存活
        所以这里把 __anext__() 包成 task 存起来，超时只是「这一轮先不等了」，
        task 本身继续跑，下一轮接着等同一个 task。绝不能丢，丢了就等于提前结束流。

    要点三：产出 None 而不是伪造事件
        心跳格式（SSE 注释行还是空 data 行）是传输层的事，不该由这里决定；
        这里只负责说「该发心跳了」，具体发什么交给调用方。
    """
    iterator = source.__aiter__()
    pending: asyncio.Future | None = None

    while True:
        if pending is None:
            # 只在这里创建一次：同一个 task 会被下面反复 await，直到它真的有结果
            pending = asyncio.ensure_future(iterator.__anext__())

        done, _ = await asyncio.wait({pending}, timeout=interval)#done,_的写法:序列解包（unpack)result = await asyncio.wait(...);done = result[0];_    = result[1]
#拿元组第一个元素（已经完成的任务集合），我们后面代码要用到它，判断if not done,  _：拿元组第二个元素，但是我们不打算使用这个值,

        if not done:
            # 业务流还没吐东西，但底层 task 仍在正常推进 —— 报一拍心跳，不动它
            yield None
            continue

        # 有结果了，先把 task 摘掉，下一轮才会去取下一个元素
        finished, pending = pending, None
        try:
            yield finished.result()
        except StopAsyncIteration:
            return


@app.get("/health")
async def health() -> dict:
    """Qt 客户端启动时探活，确认后端已就绪。"""
    return {"status": "ok"}


@app.post("/chat")
async def create_chat(request: ChatRequest) -> StreamingResponse:
    """以 SSE(Server-Sent Events)格式流式返回回复。"""
    if not request.message.strip() and not request.images:
        raise HTTPException(status_code=422, detail="message 与 images 至少提供一个")
    thread_id = request.thread_id or str(uuid4())#如果客户端没有传递 thread_id，就自动生成一个 uuid随机数

    async def event_generator():
        # 先发送会话标识,前端据此更新状态
        yield sse({"event": "session", "thread_id": thread_id})
        try:
            # 这里套一跳心跳：模型思考、联网搜索期间一个 token 都不会有，
            # 没有心跳的话 C++ 侧的低速超时会把这种「正常的安静」判成卡死。
            async for item in with_keepalive(
                chat_stream(thread_id, request.message, request.images, request.user_id)
            ):
                if item is None:
                    # SSE 注释行：规范规定客户端必须忽略，只用来「表示这条连接还活着」。
                    # 它走的是已经建好的那条连接，不经过 graph、不调模型、不花 token。
                    yield ": ping\n\n"
                else:
                    yield sse(item)
        except Exception as exc:  # noqa: BLE001
            yield sse({"event": "error", "message": str(exc)})

    return _sse_response(event_generator())


@app.post("/approve")
async def approve(request: ApproveRequest) -> StreamingResponse:
    """HITL 审批：批准则继续跑被挂起的 task_expert，拒绝则终止。"""

    async def event_generator():
        yield sse({"event": "session", "thread_id": request.thread_id})
        try:
            async for item in with_keepalive(
                resume_stream(request.thread_id, request.approved, request.note,
                              request.user_id)
            ):
                if item is None:
                    yield ": ping\n\n"
                else:
                    yield sse(item)
        except Exception as exc:  # noqa: BLE001
            yield sse({"event": "error", "message": str(exc)})

    return _sse_response(event_generator())


def _sse_response(gen) -> StreamingResponse:
    return StreamingResponse(
        gen,
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",  # 有反向代理时关掉缓冲
        },
    )


def sse(data: dict) -> str:
    """将字典数据转换为 SSE 格式的字符串。"""
    return f"data: {json.dumps(data, ensure_ascii=False)}\n\n"