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
PORT = int(os.getenv("AGENT_PORT", "9000"))

# ---------------------------------------------------------------- 业务数据库
# 智能体要用业务库里的数据才能干活（查好友、拉聊天记录等）。
# 这些账号只被授予了 EXECUTE 存储过程的权限，没有直接 SELECT 表，
# 所以工具里一律走 CALL，不要自己拼 SELECT。
DB_HOST = os.getenv("AGENT_DB_HOST", "192.168.20.128")
DB_PORT = int(os.getenv("AGENT_DB_PORT", "3306"))
DB_USER = os.getenv("AGENT_DB_USER", "dbuser")
DB_PASSWORD = os.getenv("AGENT_DB_PASSWORD", "")
DB_NAME = os.getenv("AGENT_DB_NAME", "User")
# 连不上库时不要卡住整个对话流，超时给短一点
DB_CONNECT_TIMEOUT = int(os.getenv("AGENT_DB_CONNECT_TIMEOUT", "5"))


def db_is_configured() -> bool:
    """工具开跑前先问一句：库的账号配全了吗。

    没配就返回明确的失败文案，而不是抛异常把整条对话流打断。
    """
    if not DB_HOST or not DB_USER or not DB_PASSWORD or not DB_NAME:
        return False
    return True


def require(name: str, value: str | None) -> str:
    if not value:
        raise RuntimeError(f"缺少环境变量 {name}，请在 {PROJECT_DIR / '.env'} 中配置")
    return value
