"""后端服务启动入口。

用法（项目根目录下）：
    uv run python -m main.server          # 需要 PYTHONPATH=src
    uv run python run_server.py           # 推荐，自动处理路径
"""
import uvicorn

from main.app import app
from main.config import HOST, PORT


def main() -> None:
    uvicorn.run(
        app,
        host=HOST,
        port=PORT,
        # 只监听本机时不要开 reload；日志级别按需调整
        log_level="info",
        # SSE 长连接：关闭 keep-alive 超时限制
        timeout_keep_alive=75,
    )


if __name__ == "__main__":
    main()
