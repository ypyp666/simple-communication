"""所有工具集中在这个文件里。

约定：
1. 一个工具只做一件事，参数保持扁平，不用嵌套结构。
2. 工具注册表放在文件末尾（CHAT_TOOLS / VISION_TOOLS / TASK_TOOLS），
   agent.py 只从这里取列表，不在别处声明工具。
3. 有副作用的操作不在服务端执行，这里只负责「动作白名单 + 指令规整」，
   真正的执行由 Qt 客户端收到 task_execute 后完成。
4. 需要「当前登录用户是谁」的工具，不要把这个 id 当成工具参数让模型去猜，
   统一用 config 里的 user_id（见 resolve_own_id）。
5. 工具一律写成 async def。这里所有下游（requests、pymysql）都是同步阻塞的，
   一阻塞就把整个事件循环卡住，别的用户的 SSE 流会跟着一起停。
   做法统一是：同步逻辑原样写在普通函数里，工具里用 asyncio.to_thread 丢到线程池，
   事件循环只负责等，等的同时还能处理别的会话的心跳和 token。
"""
import asyncio
import datetime
import json

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


def post_search_request(headers: dict, payload: dict) -> dict:
    """同步发一次搜索请求，返回解析后的 JSON。只在线程池里跑。"""
    resp = requests.post(url=DEEPSEEK_SEARCH_URL, headers=headers,
                         json=payload, timeout=15)
    resp.raise_for_status()
    return resp.json()


# Deepseek搜索工具
@tool
async def web_searchquery(query: str, count: int = 5, ori_question: str | None = None,
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
        # requests 是同步库，最长会卡 15 秒，绝不能直接在事件循环里等
        resp_data = await asyncio.to_thread(post_search_request, headers, payload)
        raw_results = resp_data.get("results", [])
        return format_search_results(raw_results)
    except Exception as e:
        return f"【搜索工具调用失败】异常详情: {str(e)}"


# ---------------------------------------------------------------- 业务库工具
# 「帮某位好友恢复/转发消息」这类任务由多个工具串起来做，不是一个大工具。
# 拆开的理由：只读步骤和真正会改动数据的步骤风险不同，拆开后只有后者需要审批；
# 出问题时也能分清是「没查到人」「没拿到记录」还是「转发失败」。
#
# 每个工具「干什么、什么时候调、在链条的哪一环」都写在它自己的 docstring 里。
# 提示词里不重复这些细节：工具一多就会把上下文撑爆，而且两份说明还容易不一致。
# 没写完的工具不要加 @tool，也不要放进注册表，模型看不到就调不到。

# 两个存储过程的返回码约定一致：1 = 查到东西，2 = 没有，-1 = 执行出错
PROCEDURE_SQL_ERROR = -1

# 好友查询的返回码，来自存储过程 Agent_search_user
SEARCH_FRIEND_OK = 1
SEARCH_FRIEND_NOT_FOUND = 2
SEARCH_FRIEND_SQL_ERROR = PROCEDURE_SQL_ERROR

# 聊天记录的返回码，来自存储过程 Agent_load_message
LOAD_MESSAGE_OK = 1
LOAD_MESSAGE_EMPTY = 2
LOAD_MESSAGE_SQL_ERROR = PROCEDURE_SQL_ERROR


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

    注意这个函数是同步阻塞的，只能在 asyncio.to_thread 里调用。
    连接和查询的耗时靠 connect_timeout 兜底，不要指望取消 await 能打断它——
    线程已经发出去的 TCP 请求是收不回来的。
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


def call_procedure(sql: str, params: tuple):
    """调一个带 OUT retcode 的存储过程，返回 (retcode, rows)。

    存储过程里的 SELECT 会先回一个结果集，OUT 参数要等结果集读完之后
    再单独 SELECT @retcode 取。中间那个 nextset() 循环不能省，
    否则连接里还挂着没读完的结果，下一条 SQL 直接 out of sync。
    """
    connection = open_db()
    try:
        cursor = connection.cursor()
        try:
            cursor.execute(sql, params)
            rows = []
            # 存储过程里那句 SELECT 会先回一个结果集；
            # 中途被 EXIT HANDLER 打断或者没走到 SELECT 时，这里是 None
            if cursor.description is not None:
                rows = cursor.fetchall()
            while cursor.nextset():
                pass

            cursor.execute("SELECT @retcode")
            fetched = cursor.fetchone()
            if fetched is None:
                retcode = PROCEDURE_SQL_ERROR
            else:
                retcode = fetched[0]
            if retcode is None:
                retcode = PROCEDURE_SQL_ERROR
        finally:
            cursor.close()
    finally:
        connection.close()
    return retcode, rows


def call_search_user(own_id: int, target_name: str):
    """调存储过程 Agent_search_user，返回 (retcode, rows)。"""
    sql = "CALL Agent_search_user(%s, %s, @retcode)"
    params = (own_id, target_name)
    return call_procedure(sql, params)


def call_load_message(own_id: int, target_id: int):
    """调存储过程 Agent_load_message，返回 (retcode, rows)。

    取出的是两个人之间最近 10 条双向消息，已经按时间从早到晚排好。
    """
    sql = "CALL Agent_load_message(%s, %s, @retcode)"
    params = (own_id, target_id)
    return call_procedure(sql, params)


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
async def search_friend(target_name: str, config: RunnableConfig) -> str:
    """查好友账号：把用户嘴里的「张三」「老李」换成真实的账号ID。

    【用途】
    在当前登录用户的好友列表里，按用户名或备注查人。

    【在链条中的位置】
    「帮某位好友恢复/转发消息」这类任务靠多个工具接力完成，本工具是第一环：
        1. search_friend   拿到好友账号ID    <- 本工具，只读
        2. 读取这位好友的聊天记录
        3. 把拟好的内容交给服务器转发
    后面任何一步只要用到「账号ID」，都必须先调本工具把它拿到。
    本工具只读，不会改动任何数据，可以放心调用。

    【什么时候调】
    - 用户提到的是好友的名字或备注（比如「张三」「老李」），而不是一串数字ID；
    - 想先确认这个人到底在不在好友列表里。

    【返回结果怎么用】
    - 查到一个人：把返回的「账号ID」放进 command.params，不要把名字当账号用。
    - 查到多个人：不要自己挑，把候选列进 summary 让用户确认，command 留空 {}。
    - 查不到：如实说查不到，command 留空 {}，不要编一个账号出来。
    - 返回以「【好友查询工具调用失败】」开头：这是服务端的问题（没配库、
      拿不到当前用户身份、连不上库），原样转述给用户，不要当成「没有这个人」。

    Args:
        target_name: 用户在对话里说的好友名字或备注，原样传进来，不要自己补全或改写
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
        # pymysql 是同步阻塞库，直接 await 会卡住整个事件循环，
        # 这里丢到线程池，别的会话的 SSE 心跳和 token 照常走
        retcode, rows = await asyncio.to_thread(call_search_user, own_id, target_name)
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


# 存储过程最多只给 10 条，和里面的 LIMIT 10 对应
MAX_HISTORY_ROWS = 10
# 单条消息最多给模型 300 字，避免有人发了一整篇文章把上下文吃光
MAX_CONTEXT_CHARS = 300


def parse_account_id(raw):
    """把模型传进来的账号ID 规整成 int，规整不了返回 None。

    模型有时候会把 ID 写成字符串 "1001"，有时候会直接写 1001，两种都收；
    但如果它把名字（「张三」）当成了账号传进来，就返回 None 让它重调工具，
    绝对不在这里把名字猜成一个数字。
    """
    if isinstance(raw, bool):
        return None
    if isinstance(raw, int):
        if raw <= 0:
            return None
        return raw
    if not isinstance(raw, str):
        return None

    text = raw.strip()
    if text == "":
        return None
    if not text.isascii():
        return None
    if not text.isdigit():
        return None

    value = int(text)
    if value <= 0:
        return None
    return value


def format_sentime(value) -> str:
    """把 DATETIME 列格式化成好读的时间，拿不到就写「时间未知」。"""
    if value is None:
        return "时间未知"
    if hasattr(value, "strftime"):
        return value.strftime("%Y-%m-%d %H:%M:%S")
    return str(value)


def format_chat_rows(rows, own_id: int) -> str:
    """把聊天记录拼成模型好读的文本：一条一行，标出谁说的。"""
    lines = []
    for row in rows:
        send_id = row[0]
        context = row[2]
        sentime = row[3]

        if context is None:
            text = ""
        else:
            text = str(context)
        # 多行内容压成一行，免得模型把一条消息当成好几条
        text = text.replace("\r\n", " ")
        text = text.replace("\n", " ")
        text = text.strip()
        if text == "":
            text = "（空消息）"
        if len(text) > MAX_CONTEXT_CHARS:
            text = text[:MAX_CONTEXT_CHARS] + "…（内容过长，已截断）"

        if send_id == own_id:
            speaker = "我"
        else:
            speaker = "对方"

        when = format_sentime(sentime)
        lines.append(f"[{when}] {speaker}：{text}")

    body = "\n".join(lines)
    count = len(rows)
    if count >= MAX_HISTORY_ROWS:
        header = (
            f"最近 {count} 条聊天记录（按时间从早到晚；"
            f"存储过程只取最近 {MAX_HISTORY_ROWS} 条，更早的没取）："
        )
    else:
        header = f"最近 {count} 条聊天记录（按时间从早到晚）："
    return header + "\n" + body


@tool
async def get_chat_history(account_id: str | int, config: RunnableConfig) -> str:
    """读聊天记录：把当前用户和某位好友最近的对话拉出来看。

    【用途】
    读「我」和某位好友之间最近的聊天记录，最多 10 条，双向的都包含，
    按时间从早到晚排列。工具只读，不会改动任何数据。

    【在链条中的位置】
    「帮某位好友恢复/转发消息」这类任务靠多个工具接力完成，本工具是第二环：
        1. search_friend     拿到好友账号ID        只读
        2. get_chat_history  读这段会话的聊天记录  <- 本工具，只读
        3. 把拟好的内容交给服务器转发
    本工具吃的是账号ID，不是名字。所以调它之前必须先调 search_friend，
    把名字换成账号ID；名字直接传进来会失败。

    【什么时候调】
    - 需要先知道「我和这个人之前聊了什么」才能接着往下说的时候；
    - 用户说「按我们刚才聊的继续」「把之前那些话再发一遍」这类，
      得先看清上下文再拟内容的时候。
    只是想给人发一句新消息、不提历史的时候，不用调本工具。

    【返回结果怎么用】
    - 返回若干行，每行形如 [时间] 我：…… 或 [时间] 对方：……，按时间从早到晚；
    - 表头写了「只取最近 10 条，更早的没取」，就不要假装知道更早的内容；
    - 返回「你和这位好友还没有任何聊天记录」：如实说明，不要编内容；
    - 返回以「【聊天记录工具调用失败】」开头：这是服务端或参数的问题，
      原样转述给用户，不要当成「没有聊天记录」。

    Args:
        account_id: 好友的账号ID，必须是 search_friend 返回的那个数字，
            传数字或数字字符串都行，不要把用户说的名字或备注直接传进来
    """
    if not db_is_configured():
        return "【聊天记录工具调用失败】服务端没有配置数据库账号，请联系管理员"

    own_id = resolve_own_id(config)
    if own_id is None:
        return "【聊天记录工具调用失败】拿不到当前用户的账号ID，请让客户端在请求里带上 user_id"

    target_id = parse_account_id(account_id)
    if target_id is None:
        return (
            f"【聊天记录工具调用失败】好友账号ID必须是纯数字，收到的是「{account_id}」。"
            "请先调用 search_friend 按名字或备注查到账号ID，再调本工具"
        )

    try:
        # pymysql 是同步阻塞库，丢线程池，别占住事件循环
        retcode, rows = await asyncio.to_thread(call_load_message, own_id, target_id)
    except Exception as exc:
        return f"【聊天记录工具调用失败】数据库连接或执行异常: {str(exc)}"

    if retcode == LOAD_MESSAGE_EMPTY:
        return "你和这位好友还没有任何聊天记录。"
    if retcode == LOAD_MESSAGE_SQL_ERROR:
        return "【聊天记录工具调用失败】数据库执行出错，存储过程返回 -1"
    if retcode != LOAD_MESSAGE_OK:
        return f"【聊天记录工具调用失败】存储过程返回了未知返回码 {retcode}"

    if len(rows) == 0:
        return "【聊天记录工具调用失败】返回码是「有记录」，但结果集是空的"

    return format_chat_rows(rows, own_id)


# ---------------------------------------------------------------- 第三步：转发

# 交给服务器转发的包类型，C++ 后端的 CmdType 按这个名字认
AGENT_SEND_MESSAGE_TYPE = "agent_send_message"

# 单条转发内容的安全上限。超了不截断——截断等于替用户改了话，宁可让它重拟
MAX_FORWARD_CHARS = 1000


def build_send_packet(own_id: int, target_id: int, content: str,
                      target_name: str = "") -> dict:
    """拼出交给服务器转发的 agent_send_message 包。

    字段的来路分得很清楚：
    - targetId：search_friend 查出来的账号ID，**投递的唯一依据**；
    - targetName：好友的人名/备注，**只给审批框显示用**，服务器可以不看；
    - content：智能体拟好的内容；
    - sendId：当前登录用户，从 config 取，不由模型提供；
    - sendTime：工具自己取。模型没有时钟，让它填只会得到一个编的时间。
    账号一律写成字符串，和 C++ 后端、Qt 客户端的习惯保持一致。
    """
    now = datetime.datetime.now()
    packet = {
        "type": AGENT_SEND_MESSAGE_TYPE,
        "targetId": str(target_id),
        "sendId": str(own_id),
        "content": content,
        "sendTime": now.strftime("%Y-%m-%d %H:%M:%S"),
    }

    # 没人名就干脆不写这个字段：写一个空串进去，审批框会显示「对象：」后面空白，
    # 比直接显示账号ID 更让人困惑
    if isinstance(target_name, str):
        name = target_name.strip()
        if name != "":
            packet["targetName"] = name

    return packet


@tool
async def forward_message(target_account: str | int, content: str,
                          config: RunnableConfig, target_name: str = "") -> str:
    """准备转发包：把拟好的内容打包成服务器能直接转发的消息。

    【用途】
    把「发给谁、发什么、谁发的、什么时候发」拼成一个 `agent_send_message` 包，
    交给服务器去写库并推给对方。

    【在链条中的位置】
    「帮某位好友恢复/转发消息」这类任务靠多个工具接力完成，本工具是第三环：
        1. search_friend     拿到好友账号ID        只读
        2. get_chat_history  读这段会话的聊天记录  只读
        3. forward_message   把内容打包给服务器转发  <- 本工具
    本工具只备包，不直接发出去：包会跟着指令回到服务器，用户在审批环节
    点头之后才会真的发，用户拒了就不会发。

    【什么时候调】
    - 内容和对象都已经确定，就差「把这句话发出去」的时候；
    - 内容必须是你自己拟好的最终版本，不要先备包再问用户要不要改。

    【返回结果怎么用】
    - 正常返回一段 JSON，里面已经有 type / targetId / sendId / content / sendTime，
      这就是要交给服务器的包；你只需要在 command 里给出 action=agent_send_message，
      **不要自己再填 targetId / content / sendTime**，服务端会拿本工具的返回原样填充。
    - 返回以「【消息转发工具调用失败】」开头：包没备成，不要说已经发了，
      原样转述给用户，command 留空 {}。

    Args:
        target_account: 好友的账号ID，必须是 search_friend 返回的那个数字，
            不要把用户说的名字或备注直接传进来
        content: 要发给对方的完整内容，一句一句写清楚，不要带「以下是消息：」这类说明
    """
    if not db_is_configured():
        return "【消息转发工具调用失败】服务端没有配置数据库账号，请联系管理员"

    own_id = resolve_own_id(config)
    if own_id is None:
        return "【消息转发工具调用失败】拿不到当前用户的账号ID，请让客户端在请求里带上 user_id"

    target_id = parse_account_id(target_account)
    if target_id is None:
        return (
            f"【消息转发工具调用失败】好友账号ID必须是纯数字，收到的是「{target_account}」。"
            "请先调用 search_friend 按名字或备注查到账号ID"
        )

    if target_id == own_id:
        return "【消息转发工具调用失败】不能把消息发给自己"

    if not isinstance(content, str):
        return "【消息转发工具调用失败】消息内容必须是文本"
    content = content.strip()
    if content == "":
        return "【消息转发工具调用失败】消息内容不能为空"
    if len(content) > MAX_FORWARD_CHARS:
        return (
            f"【消息转发工具调用失败】消息内容 {len(content)} 字，超过 "
            f"{MAX_FORWARD_CHARS} 字上限，请拆短后再备包"
        )

    packet = build_send_packet(own_id, target_id, content, target_name)
    return json.dumps(packet, ensure_ascii=False)


# ---------------------------------------------------------------- 任务动作

# 动作白名单：action 名 -> 说明 + 必填参数名 + 可选的前置工具链。
# 想给客户端新增一个可执行动作，只改这里，提示词和校验会一起生效。
#
# "chain" 是可选的：一个动作在输出指令之前，如果必须先调几个工具把数据准备好，
# 就把工具名按执行顺序写进来。这里只写「用哪个工具、什么顺序」，
# 工具的详细说明写在工具自己的 docstring 里，不在这里重复。
TASK_ACTIONS = {
    "delete_friend": {
        "label": "删除好友",
        "desc": "删除好友",
        "params": ["name"],
        "chain": ["search_friend"],
    },
    "update_friend_remark": {
        "label": "修改好友备注",
        "desc": "修改好友备注",
        "params": ["name", "remark"],
        "chain": ["search_friend"],
    },
    "add_friend": {
        "label": "添加好友",
        "desc": "添加好友",
        "params": ["name"],
    },
    "agent_send_message": {
        # label 是给用户看的中文名，desc 是给模型看的说明，两者故意分开：
        # desc 里那些「params 留空、由谁备好」对用户是噪音
        "label": "发送消息给好友",
        "desc": "把内容转发给好友，params 留空，由 forward_message 备好",
        "params": [],
        "chain": ["search_friend", "forward_message"],
    },
}

# 风险等级的中文说法。审批框直接把这三个字显示给用户，
# 让他一眼看出这条指令是轻是重
RISK_LABELS = {
    "low": "低",
    "medium": "中",
    "high": "高",
}

# 校验用的合法取值。单独留一份而不是直接取 RISK_LABELS 的键，
# 是因为两者的用途不一样：一个管能不能存，一个管怎么显示，
# 将来加等级时很可能只改其中一个
RISK_LEVELS = ("low", "medium", "high")


def list_task_actions() -> str:
    """把动作白名单渲染成文本，直接嵌进任务专家的提示词。

    只输出「叫什么、干什么、要哪些参数、执行前先调哪个工具」这四件事。
    工具的详细说明故意不写进来：它们已经在各自的 docstring 里，
    模型加载工具时就能看到，重复一遍只会白白占上下文。
    """
    lines = []
    for action, spec in TASK_ACTIONS.items():
        need = "、".join(spec["params"])
        line = f"- {action}：{spec['desc']}"
        if need:
            line += f"，必填参数 {need}"

        chain = spec.get("chain")
        if isinstance(chain, list) and len(chain) > 0:
            names = " → ".join(chain)
            line += f"；执行前先按顺序调用 {names}"

        lines.append(line)
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


def describe_task_command(command) -> list:
    """把一条指令翻译成「给谁、做什么」的展示行，供审批框直接渲染。

    为什么不让模型自己写这几行：它写的 summary 常常是「帮你把消息发出去」
    这种不含对象的空话，用户在审批框上根本看不出要发给谁，那这个审批就
    白点了。这里全部从 command 的确定字段里取，模型换什么说法都影响不到展示。

    返回 [{"label": "操作", "value": "发送消息给好友"}, ...]，
    客户端按顺序两列渲染即可，不用自己拼中文字符串，新增动作也不用改客户端。
    """
    detail = []
    if not isinstance(command, dict):
        return detail

    action = command.get("action")
    if not isinstance(action, str):
        return detail

    spec = TASK_ACTIONS.get(action)
    action_label = action
    if isinstance(spec, dict):
        label = spec.get("label")
        if isinstance(label, str) and label != "":
            action_label = label
    detail.append({"label": "操作", "value": action_label})

    params = command.get("params")
    if not isinstance(params, dict):
        params = {}

    # 对象：能拿到人名就显示成「老李（账号 1001）」。
    # 宁可少显示，也绝不在这里编一个名字出来——用户是照着这行字点同意的
    target_name = ""
    raw_name = params.get("targetName")
    if isinstance(raw_name, str):
        target_name = raw_name.strip()
    if target_name == "":
        raw_name = params.get("name")
        if isinstance(raw_name, str):
            target_name = raw_name.strip()

    target_id = ""
    raw_id = params.get("targetId")
    if raw_id is not None:
        target_id = str(raw_id).strip()

    if target_name != "" and target_id != "":
        detail.append({"label": "对象", "value": f"{target_name}（账号 {target_id}）"})
    elif target_name != "":
        detail.append({"label": "对象", "value": target_name})
    elif target_id != "":
        detail.append({"label": "对象", "value": f"账号 {target_id}"})

    # 正文：发消息看 content，改备注看 remark。
    # 长内容不在工具层截断，完整交给客户端，由它决定换行还是折叠
    content = ""
    raw_content = params.get("content")
    if isinstance(raw_content, str):
        content = raw_content
    if content.strip() != "":
        detail.append({"label": "内容", "value": content})

    remark = ""
    raw_remark = params.get("remark")
    if isinstance(raw_remark, str):
        remark = raw_remark
    if remark.strip() != "":
        detail.append({"label": "新备注", "value": remark})

    send_time = ""
    raw_time = params.get("sendTime")
    if isinstance(raw_time, str):
        send_time = raw_time
    if send_time.strip() != "":
        detail.append({"label": "时间", "value": send_time})

    risk = command.get("risk")
    if risk in RISK_LABELS:
        detail.append({"label": "风险", "value": RISK_LABELS[risk]})

    return detail


# ---------------------------------------------------------------- 注册表

# 聊天专家可用的工具
CHAT_TOOLS = [web_searchquery]

# 图片理解专家：只描述看到的画面，不需要外部工具
VISION_TOOLS = []

# 任务专家：查好友、读聊天记录、备转发包这类工具可以自己调；
# 备好的包不会直接发出去——它只是指令的一部分，等 Qt 客户端收到 task_execute、
# 用户也已经在审批环节点过头之后，服务器才会真正把消息推给好友。
TASK_TOOLS = [search_friend, get_chat_history, forward_message]


