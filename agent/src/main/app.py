import asyncio
import json
from collections.abc import AsyncIterable, AsyncIterator
from typing import Any
from uuid import uuid4

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

from .agent import chat_stream

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
    message: str = Field(min_length=1, max_length=4000)
    thread_id: str | None = None


async def with_keepalive(
    source: AsyncIterable[dict[str, Any]],
    interval: float = KEEPALIVE_SEC,
) -> AsyncIterator[dict[str, Any] | None]:
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

        done, _ = await asyncio.wait({pending}, timeout=interval)

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
    thread_id = request.thread_id or str(uuid4())#如果客户端没有传递 thread_id，就自动生成一个 uuid随机数

    async def event_generator():
        # 先发送会话标识,前端据此更新状态
        yield sse({"event": "session", "thread_id": thread_id})
        try:
            # 这里套一跳心跳：模型思考、联网搜索期间一个 token 都不会有，
            # 没有心跳的话 C++ 侧的低速超时会把这种「正常的安静」判成卡死。
            async for item in with_keepalive(chat_stream(thread_id, request.message)):
                if item is None:
                    # SSE 注释行：规范规定客户端必须忽略，只用来「表示这条连接还活着」。
                    # 它走的是已经建好的那条连接，不经过 graph、不调模型、不花 token。
                    yield ": ping\n\n"
                else:
                    yield sse(item)
        except Exception as exc:  # noqa: BLE001
            yield sse({"event": "error", "message": str(exc)})

    return StreamingResponse(
        event_generator(),
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