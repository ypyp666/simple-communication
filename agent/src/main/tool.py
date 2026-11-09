"""所有工具集中在这个文件里。

约定：
1. 一个工具只做一件事，参数保持扁平，不用嵌套结构。
2. 工具注册表放在文件末尾（CHAT_TOOLS / VISION_TOOLS / TASK_TOOLS），
   agent.py 只从这里取列表，不在别处声明工具。
3. 有副作用的操作不在服务端执行，这里只负责「动作白名单 + 指令规整」，
   真正的执行由 Qt 客户端收到 task_execute 后完成。
4. 需要「当前登录用户是谁」的工具，不要把这个 id 当成工具参数让模型去猜，
   统一用 config 里的 user_id（见 resolve_own_id）。
"""
import pymysql
import requests
from pymysql.constants import CLIENT
from langchain_core.runnables import RunnableConfig
from langchain_core.tools import tool

from main.config import DEEPSEEK_API_KEY, DEEPSEEK_SEARCH_URL
from main.config import DB_HOST, DB_PORT, DB_USER, DB_PASSWORD, DB_NAME
from main.config import DB_CONNECT_TIMEOUT, db_is_configured

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


# ---------------------------------------------------------------- 业务库工具
# 「帮某位好友恢复/转发消息」这条链路一共三步：
#   1. search_friend     拿到好友账号                    <- 本次实现
#   2. get_chat_history  拿到这位好友的聊天记录           <- 待实现
#   3. forward_message   把拟好的内容交给服务器转发出去   <- 待实现
#
# 为什么拆成三个工具而不是一个大工具：
# - 三步的风险等级不一样。第 1、2 步只读，第 3 步会真的把消息发出去；
#   拆开之后只有第 3 步需要走审批，前两步可以放心让模型自己跑。
# - 出问题时能明确知道是「没查到人」「没拿到记录」还是「转发失败」，
#   合成一个大工具就只剩一句「失败」，用户和日志都没法定位。

# 好友查询的返回码，来自存储过程 Agent_search_user
SEARCH_FRIEND_OK = 1
SEARCH_FRIEND_NOT_FOUND = 2
SEARCH_FRIEND_SQL_ERROR = -1


def user_id_from_thread_id(thread_id: str):
    """从 thread_id 里抠出 user_id，抠不出来返回 None。

    服务器和 Qt 约定 thread_id = 会话id + "n" + user_id。
    用 rpartition 从右边切一刀：左半段是会话id，随便它含什么字符都不影响，
    右半段必须是纯 ASCII 数字，否则认为格式不对。
    """
    if not isinstance(thread_id, str):
        return None
    if thread_id == "":
        return None

    head, sep, tail = thread_id.rpartition("n")
    if sep == "":
        return None
    if head == "" or tail == "":
        return None
    if not tail.isascii():
        return None
    if not tail.isdigit():
        return None
    return int(tail)


def resolve_own_id(config: RunnableConfig):
    """取出「当前登录用户自己的 user_id」，取不到返回 None。

    两条来源，显式的优先：
    1. 请求里直接带 user_id，服务端放进 config["configurable"]["user_id"]；
    2. 没带就回退到解析 thread_id，兼容还没改的老客户端。

    两条都失败时返回 None，让工具给出明确的失败文案——
    绝对不在这里随便猜一个 id 去查库，那会把用户 A 的好友查给用户 B。
    """
    if not isinstance(config, dict):
        return None

    configurable = config.get("configurable")
    if not isinstance(configurable, dict):
        return None

    raw_user_id = configurable.get("user_id")
    if raw_user_id is not None:
        try:
            return int(raw_user_id)
        except (TypeError, ValueError):
            return None

    thread_id = configurable.get("thread_id")
    return user_id_from_thread_id(thread_id)


def open_db():
    """打开业务库连接，调用方负责 close。

    client_flag 必须带 MULTI_RESULTS：存储过程先回结果集、再回输出参数，
    不带这个标志 pymysql 会抛 "Commands out of sync"。
    """
    connection = pymysql.connect(
        host=DB_HOST,
        port=DB_PORT,
        user=DB_USER,
        password=DB_PASSWORD,
        database=DB_NAME,
        charset="utf8mb4",
        connect_timeout=DB_CONNECT_TIMEOUT,
        client_flag=CLIENT.MULTI_RESULTS,
    )
    return connection


def call_search_user(own_id: int, target_name: str):
    """调存储过程 Agent_search_user，返回 (retcode, rows)。"""
    connection = open_db()
    try:
        cursor = connection.cursor()
        try:
            cursor.execute(
                "CALL Agent_search_user(%s, %s, @retcode)",
                (own_id, target_name),
            )
            rows = []
            # 存储过程里那句 SELECT 会先回一个结果集
            if cursor.description is not None:
                rows = cursor.fetchall()
            # 把剩下的结果集读干净，否则下一条 SQL 会 out of sync
            while cursor.nextset():
                pass

            cursor.execute("SELECT @retcode")
            fetched = cursor.fetchone()
            if fetched is None:
                retcode = SEARCH_FRIEND_SQL_ERROR
            else:
                retcode = fetched[0]
            if retcode is None:
                retcode = SEARCH_FRIEND_SQL_ERROR
        finally:
            cursor.close()
    finally:
        connection.close()
    return retcode, rows


def format_friend_rows(rows) -> str:
    """把查询结果拼成模型好读的文本。"""
    lines = []
    for row in rows:
        target_id = row[0]
        user_name = row[1]
        mark = row[2]
        if mark is None:
            mark = ""
        if mark == "":
            mark = "（没设备注）"
        lines.append(f"- 账号ID：{target_id}，用户名：{user_name}，备注：{mark}")

    body = "\n".join(lines)
    if len(rows) == 1:
        return f"共匹配到 1 个好友：\n{body}"
    if len(rows) > 1:
        return (
            f"共匹配到 {len(rows)} 个好友，说明用户说的是哪个还没定下来：\n"
            f"{body}\n"
            "请先把这几个人列给用户，让他确认是哪一位，不要自己挑一个。"
        )
    return ""


@tool
def search_friend(target_name: str, config: RunnableConfig) -> str:
    """按好友的用户名或备注查好友账号，查到后才有账号ID做后续操作。

    用户说「给张三发消息」「看看老李的聊天记录」时，先用这个工具把
    「张三」「老李」换成真实的账号ID。用户名和备注都会匹配，所以可能
    一次查回好几个人，这时要把候选列给用户确认。

    Args:
        target_name: 用户在对话里说的好友名字或备注，原样传进来，不要自己补全
    """
    if not db_is_configured():
        return "【好友查询工具调用失败】服务端没有配置数据库账号，请联系管理员"

    own_id = resolve_own_id(config)
    if own_id is None:
        return "【好友查询工具调用失败】拿不到当前用户的账号ID，请让客户端在请求里带上 user_id"

    if not isinstance(target_name, str):
        return "【好友查询工具调用失败】好友名字必须是文本"
    target_name = target_name.strip()
    if target_name == "":
        return "【好友查询工具调用失败】好友名字不能为空"

    try:
        retcode, rows = call_search_user(own_id, target_name)
    except Exception as exc:
        return f"【好友查询工具调用失败】数据库连接或执行异常: {str(exc)}"

    if retcode == SEARCH_FRIEND_NOT_FOUND:
        return f"你的好友里没有用户名或备注是「{target_name}」的人。"
    if retcode == SEARCH_FRIEND_SQL_ERROR:
        return "【好友查询工具调用失败】数据库执行出错，存储过程返回 -1"
    if retcode != SEARCH_FRIEND_OK:
        return f"【好友查询工具调用失败】存储过程返回了未知返回码 {retcode}"

    if len(rows) == 0:
        return "【好友查询工具调用失败】返回码是「查到」，但结果集是空的"

    return format_friend_rows(rows)


# ---------------------------------------------------------------- 待实现的两步
# 下面两个函数还不是正式工具（没有加 @tool），也故意不放进注册表，
# 模型看不到、也调不到，避免它在第 3 步还没做好的情况下乱承诺。
#
# get_chat_history(account_id)：
#   输入第 1 步拿到的账号ID，返回这段会话的聊天记录文本。
#   约定的存储过程还没定，定下来之后照 search_friend 的样子写：
#   参数扁平、走 CALL、返回字符串、异常自己吞掉转成失败文案。
#
# forward_message(account_id, content)：
#   把模型拟好的回复内容和账号ID交给服务器，由服务器解析后转发。
#   这一步有副作用，必须挂到审批链路上：先在 examine_expert 里让用户确认，
#   批准之后才允许真正发给服务器。
#
# 三个都齐了以后，任务专家就能自己串起来：
#   search_friend -> get_chat_history -> 拟内容 -> 审批 -> forward_message


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

# 任务专家：查好友这类只读工具可以自己调；
# 真正有副作用的操作仍然只输出指令，等 Qt 客户端收到 task_execute 再执行。
TASK_TOOLS = [search_friend]


