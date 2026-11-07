"""所有工具集中在这个文件里。

约定：
1. 一个工具只做一件事，参数保持扁平，不用嵌套结构。
2. 工具注册表放在文件末尾（CHAT_TOOLS / VISION_TOOLS / TASK_TOOLS），
   agent.py 只从这里取列表，不在别处声明工具。
3. 有副作用的操作不在服务端执行，这里只负责「动作白名单 + 指令规整」，
   真正的执行由 Qt 客户端收到 task_execute 后完成。
"""
import requests
from langchain_core.tools import tool

from main.config import DEEPSEEK_API_KEY, DEEPSEEK_SEARCH_URL

# 搜索结果的过滤阈值：低于这个分数的条目一律丢弃
MIN_RERANK_SCORE = 1.5


def format_search_results(raw_results: list) -> str:
    """把搜索接口返回的原始条目拼成模型好读的文本块。"""
    output = ""
    for item in raw_results:
        score = item.get("rerank_score", 0)
        # 按文档要求，只保留 >= 1.5 的优质结果
        if score < MIN_RERANK_SCORE:
            continue

        title = item.get("title", "")
        url = item.get("url", "")
        summary = item.get("summary", "")
        date = item.get("date", "")
        authority = item.get("authority", -1)

        block = (
            f"【标题】{title}\n"
            f"【发布时间】{date}\n"
            f"【权威分】{authority}，相关性分数：{score}\n"
            f"【摘要】{summary}\n"
            f"【来源链接】{url}\n"
            "=====\n"
        )
        output += block

    if not output:
        return "本次搜索没有找到足够相关的高质量资料。"
    return output


# Deepseek搜索工具
@tool
def web_searchquery(query: str, count: int = 5, ori_question: str | None = None,
                    need_content: bool = True) -> str:
    """
    联网搜索工具，查询互联网实时信息、新闻、资料、最新事件。
    当模型内部知识库过时，需要外部最新资料时，调用此工具。

    Args:
        query: 搜索查询词，需要简洁清晰，适合网页检索
        count: 返回结果数量，默认5条，范围建议1~10
        ori_question: 触发搜索的用户原始问题，1～128 个字符，有助于更好地理解搜索意图，传 null 视为未提供。
        need_content: 是否拉取网页完整正文片段。True返回正文，消耗更多token；False仅返回摘要
    """
    if not DEEPSEEK_SEARCH_URL:
        return "【搜索工具调用失败】未配置 DEEPSEEK_SEARCH_URL"

    headers = {
        "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
        "Content-Type": "application/json",
    }
    payload = {
        "query": query,
        "count": count,
        "need_content": need_content,
    }
    try:
        resp = requests.post(url=DEEPSEEK_SEARCH_URL, headers=headers,
                             json=payload, timeout=15)
        resp.raise_for_status()
        resp_data = resp.json()
        raw_results = resp_data.get("results", [])
        return format_search_results(raw_results)
    except Exception as e:
        return f"【搜索工具调用失败】异常详情: {str(e)}"


# ---------------------------------------------------------------- 任务动作

# 动作白名单：action 名 -> 说明 + 必填参数名。
# 想给客户端新增一个可执行动作，只改这里，提示词和校验会一起生效。
TASK_ACTIONS = {
    "delete_friend": {
        "desc": "删除好友",
        "params": ["name"],
    },
    "update_friend_remark": {
        "desc": "修改好友备注",
        "params": ["name", "remark"],
    },
    "add_friend": {
        "desc": "添加好友",
        "params": ["name"],
    },
    "send_message": {
        "desc": "给好友发送消息",
        "params": ["name", "content"],
    },
}

RISK_LEVELS = ("low", "medium", "high")


def list_task_actions() -> str:
    """把动作白名单渲染成文本，直接嵌进任务专家的提示词。"""
    lines = []
    for action, spec in TASK_ACTIONS.items():
        need = "、".join(spec["params"])
        lines.append(f"- {action}：{spec['desc']}，必填参数 {need}")
    return "\n".join(lines)


def normalize_task_command(raw_command, fallback_risk=None) -> dict:
    """把模型给的 command 收敛成客户端能直接执行的结构。

    模型偶尔会编出不存在的动作名，这里统一兜住：
    - 动作名不在白名单里，整个指令作废（返回空 dict，让上层提示用户）
    - 参数缺失不在这里报错，留给用户在审批环节发现并拒绝
    - 模型常常把 risk 写在最外层，这时用 fallback_risk 补进来
    """
    if not isinstance(raw_command, dict):
        return {}

    action = raw_command.get("action")
    if not isinstance(action, str):
        return {}
    action = action.strip()
    if action not in TASK_ACTIONS:
        return {}

    params = raw_command.get("params")
    if not isinstance(params, dict):
        params = {}

    risk = raw_command.get("risk")
    if risk not in RISK_LEVELS:
        risk = fallback_risk
    if risk not in RISK_LEVELS:
        risk = "medium"

    command = {
        "action": action,
        "params": params,
        "risk": risk,
    }
    return command


# ---------------------------------------------------------------- 注册表

# 聊天专家可用的工具
CHAT_TOOLS = [web_searchquery]

# 图片理解专家：只描述看到的画面，不需要外部工具
VISION_TOOLS = []

# 任务专家：副作用由客户端执行，服务端不挂工具
TASK_TOOLS = []

