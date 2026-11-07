"""集中管理路径与运行时配置。

单独成模块，避免 agent.py 与 tool.py 互相 import 造成循环导入。
"""
import os
from pathlib import Path

from dotenv import load_dotenv

ROOT_DIR = Path(__file__).resolve().parent.parent   # src/
PROJECT_DIR = ROOT_DIR.parent                       # 项目根目录
RESOURCES_DIR = ROOT_DIR / "resources"
RESOURCES_DIR.mkdir(parents=True, exist_ok=True)

# 先加载项目根目录的 .env（与从哪个目录启动无关），再兼容 cwd 下的 .env
load_dotenv(PROJECT_DIR / ".env")
load_dotenv()

DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY")
DEEPSEEK_BASE_URL = os.getenv("DEEPSEEK_BASE_URL")
DEEPSEEK_SEARCH_URL = os.getenv("DEEPSEEK_SEARCH_URL")
DEEPSEEK_MODEL = os.getenv("DEEPSEEK_MODEL", "deepseek-flash")
# 图片理解专家用的视觉模型；没有单独配置时退回主模型
DEEPSEEK_VISION_MODEL = os.getenv("DEEPSEEK_VISION_MODEL") or DEEPSEEK_MODEL

# 服务监听参数，Qt 客户端与后端约定这两个值即可
HOST = os.getenv("AGENT_HOST", "127.0.0.1")
PORT = int(os.getenv("AGENT_PORT", "8000"))


def require(name: str, value: str | None) -> str:
    if not value:
        raise RuntimeError(f"缺少环境变量 {name}，请在 {PROJECT_DIR / '.env'} 中配置")
    return value
