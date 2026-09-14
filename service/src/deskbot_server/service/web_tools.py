"""LLM 网络工具：webfetch / websearch。"""

from __future__ import annotations

import json
import logging
import os
import re
import urllib.error
import urllib.parse
import urllib.request
from html import unescape
from typing import Any

logger = logging.getLogger("deskbot-server")

_USER_AGENT = "OpenDesk-Deskbot/1.0"
_MAX_FETCH_BYTES = 120_000
_FETCH_TIMEOUT_SEC = 20
_MAX_SEARCH_RESULTS = 8

# 搜索超时：单独于 _FETCH_TIMEOUT_SEC。DuckDuckGo 在部分网络（如国内）不可达，
# 每次调用会一直挂到超时；20s × 2 次尝试 = 最坏 40s 端到端延迟。默认收敛到 5s。
_SEARCH_TIMEOUT_SEC = max(1.0, float(os.environ.get("SEARCH_TIMEOUT_SEC", "5")))

# 博查 Web Search API（国内可达，响应格式兼容 Bing Search API）。
# 配 BOCHA_API_KEY 后优先走它；未配置则沿用下面的 DuckDuckGo 无密钥路径。
# 控制台：https://open.bocha.cn
_BOCHA_URL = os.environ.get("BOCHA_SEARCH_URL", "https://api.bochaai.com/v1/web-search")

_DDG_TOPIC_RE = re.compile(
    r'<a class="result__a"[^>]*href="([^"]+)"[^>]*>(.*?)</a>'
    r'.*?<a class="result__snippet"[^>]*>(.*?)</a>',
    re.DOTALL | re.IGNORECASE,
)


def _http_get(
    url: str, *, timeout: int = _FETCH_TIMEOUT_SEC, max_bytes: int = _MAX_FETCH_BYTES
) -> tuple[int, str, bytes]:
    req = urllib.request.Request(
        url, headers={"User-Agent": _USER_AGENT, "Accept": "text/html,application/json,*/*"}, method="GET"
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        status = int(getattr(resp, "status", 200) or 200)
        chunks: list[bytes] = []
        total = 0
        while True:
            block = resp.read(min(8192, max_bytes - total))
            if not block:
                break
            chunks.append(block)
            total += len(block)
            if total >= max_bytes:
                break
        body = b"".join(chunks)
    return status, str(resp.headers.get("Content-Type") or ""), body


def _http_post_json(
    url: str,
    payload: dict[str, Any],
    *,
    headers: dict[str, str] | None = None,
    timeout: float = _SEARCH_TIMEOUT_SEC,
    max_bytes: int = 200_000,
) -> tuple[int, bytes]:
    hdrs = {
        "User-Agent": _USER_AGENT,
        "Content-Type": "application/json",
        "Accept": "application/json",
    }
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"), headers=hdrs, method="POST"
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        status = int(getattr(resp, "status", 200) or 200)
        body = resp.read(max_bytes)
    return status, body


def _bocha_api_key() -> str:
    for name in ("BOCHA_API_KEY", "BOCHA_KEY"):
        val = (os.environ.get(name) or "").strip()
        if val:
            return val
    return ""


def _websearch_bocha(q: str, limit: int, key: str) -> list[dict[str, str]]:
    """博查 Web Search API。返回 [{title,url,snippet}]，失败抛异常由调用方兜。

    响应兼容 Bing Search API：结果在 webPages.value[]，字段 name/url/snippet；
    博查实际返回外层多包一层 data，这里两种形态都兼容。
    """
    status, raw = _http_post_json(
        _BOCHA_URL,
        {"query": q, "count": limit, "summary": True},
        headers={"Authorization": f"Bearer {key}"},
    )
    if status != 200:
        raise urllib.error.HTTPError(_BOCHA_URL, status, f"bocha http {status}", None, None)
    data = json.loads(raw.decode("utf-8", errors="replace"))
    node = data.get("data") if isinstance(data.get("data"), dict) else data
    pages = ((node.get("webPages") or {}).get("value")) or []
    out: list[dict[str, str]] = []
    for page in pages[:limit]:
        if not isinstance(page, dict):
            continue
        title = str(page.get("name") or page.get("title") or "").strip()
        url = str(page.get("url") or "").strip()
        # summary 是博查的加长摘要，snippet 是 Bing 风格的短摘要
        snippet = str(page.get("summary") or page.get("snippet") or "").strip()
        if title or snippet:
            out.append({"title": title, "url": url, "snippet": snippet[:400]})
    return out


def webfetch(url: str) -> dict[str, Any]:
    raw = str(url or "").strip()
    if not raw:
        raise ValueError("url 不能为空")
    parsed = urllib.parse.urlparse(raw)
    if parsed.scheme not in ("http", "https"):
        raise ValueError("仅支持 http/https URL")
    if not parsed.netloc:
        raise ValueError("url 无效")
    try:
        status, content_type, body = _http_get(raw)
    except urllib.error.HTTPError as exc:
        err_body = exc.read(_MAX_FETCH_BYTES) if exc.fp else b""
        text = err_body.decode("utf-8", errors="replace")[:8000]
        return {
            "ok": False,
            "url": raw,
            "status": int(exc.code),
            "content_type": str(exc.headers.get("Content-Type") or ""),
            "error": str(exc.reason),
            "text": text,
        }
    except urllib.error.URLError as exc:
        return {"ok": False, "url": raw, "error": str(exc.reason)}
    text = body.decode("utf-8", errors="replace")
    if len(body) >= _MAX_FETCH_BYTES:
        text += "\n…(内容已截断)"
    return {
        "ok": True,
        "url": raw,
        "status": status,
        "content_type": content_type,
        "bytes": len(body),
        "text": text[:12000],
    }


def _strip_html(html: str) -> str:
    text = re.sub(r"<[^>]+>", " ", html or "")
    return unescape(re.sub(r"\s+", " ", text)).strip()


def websearch(query: str, *, max_results: int = _MAX_SEARCH_RESULTS) -> dict[str, Any]:
    q = str(query or "").strip()
    if not q:
        raise ValueError("query 不能为空")
    limit = max(1, min(int(max_results), _MAX_SEARCH_RESULTS))

    # 配了博查 Key 就走博查，且不再回退 DuckDuckGo——
    # DDG 在部分网络不可达，回退会让请求再挂一次 _SEARCH_TIMEOUT_SEC，
    # 把"搜索失败"拖成十几秒。宁可快速返回空结果，也不要阻塞对话。
    bocha_key = _bocha_api_key()
    if bocha_key:
        try:
            results = _websearch_bocha(q, limit, bocha_key)
            return {
                "ok": True,
                "query": q,
                "results": results[:limit],
                "abstract": None,
                "backend": "bocha",
            }
        except (urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError, OSError) as exc:
            logger.warning("[websearch] bocha 失败，返回空结果: %s", exc)
            return {
                "ok": True,
                "query": q,
                "results": [],
                "abstract": None,
                "backend": "bocha",
            }

    # ---- 以下为无密钥的 DuckDuckGo 兜底路径（未配 BOCHA_API_KEY 时使用）----
    # DuckDuckGo Instant Answer API（无密钥）
    api_url = "https://api.duckduckgo.com/?" + urllib.parse.urlencode(
        {"q": q, "format": "json", "no_html": 1, "skip_disambig": 1}
    )
    results: list[dict[str, str]] = []
    abstract = ""
    try:
        status, _ct, body = _http_get(api_url, timeout=_SEARCH_TIMEOUT_SEC, max_bytes=64_000)
        if status == 200:
            data = json.loads(body.decode("utf-8", errors="replace"))
            abstract = str(data.get("AbstractText") or "").strip()
            if abstract:
                results.append(
                    {
                        "title": str(data.get("Heading") or "摘要"),
                        "url": str(data.get("AbstractURL") or ""),
                        "snippet": abstract,
                    }
                )
            for topic in data.get("RelatedTopics") or []:
                if len(results) >= limit:
                    break
                if isinstance(topic, dict) and topic.get("Text"):
                    results.append(
                        {
                            "title": str(topic.get("Text") or "")[:120],
                            "url": str(topic.get("FirstURL") or ""),
                            "snippet": str(topic.get("Text") or "")[:400],
                        }
                    )
    except (urllib.error.URLError, json.JSONDecodeError, OSError):
        pass

    # HTML 备用检索
    if len(results) < limit:
        html_url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": q})
        try:
            _status, _ct, body = _http_get(html_url, timeout=_SEARCH_TIMEOUT_SEC, max_bytes=200_000)
            html = body.decode("utf-8", errors="replace")
            for m in _DDG_TOPIC_RE.finditer(html):
                if len(results) >= limit:
                    break
                href = unescape(m.group(1))
                title = _strip_html(m.group(2))
                snippet = _strip_html(m.group(3))
                if title:
                    results.append({"title": title, "url": href, "snippet": snippet})
        except (urllib.error.URLError, OSError):
            pass

    return {
        "ok": True,
        "query": q,
        "results": results[:limit],
        "abstract": abstract or None,
        "backend": "duckduckgo",
    }
