import requests
from langchain_core.tools import tool

from main.config import DEEPSEEK_API_KEY, DEEPSEEK_SEARCH_URL

def format_search_results(raw_results: list) -> str:
    output = ""
    for item in raw_results:
        score = item.get("rerank_score", 0)
        # 按文档要求，只保留>=1.5的优质结果
        if score < 1.5:
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
        "Content-Type": "application/json"
    }
    payload = {
        "query": query,
        "count": count,
        "need_content": need_content
    }
    try:
        resp = requests.post(url=DEEPSEEK_SEARCH_URL, headers=headers, json=payload, timeout=15)
        resp.raise_for_status()
        resp_data = resp.json()
        raw_results = resp_data.get("results", [])
        return format_search_results(raw_results)
    except Exception as e:
        return f"【搜索工具调用失败】异常详情: {str(e)}"

