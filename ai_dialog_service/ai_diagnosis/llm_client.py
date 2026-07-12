"""大模型调用（OpenAI 兼容 API）。API Key 仅来自环境变量或本地配置文件，禁止写入 HTML。"""
from __future__ import annotations

import copy
import json
import os
import re
import urllib.error
import urllib.request
from collections.abc import Iterator
from typing import Any


class LlmConfigError(Exception):
    pass


class LlmCallError(Exception):
    pass


PROVIDER_ALIASES: dict[str, str] = {
    "gpt": "openai",
    "chatgpt": "openai",
    "openai_gpt": "openai",
    "claude": "vectorengine",
    "anthropic": "vectorengine",
    "vector_engine": "vectorengine",
    "vectorengine_claude": "vectorengine",
}

PROVIDER_PRESETS: dict[str, dict[str, Any]] = {
    "deepseek": {
        "label": "DeepSeek",
        "base_url": "https://api.deepseek.com",
        "model": "deepseek-chat",
        "provider": "deepseek",
        "api_style": "openai",
        "timeout_sec": 90,
        "temperature": 0.2,
        "max_tokens": 6000,
        "use_json_mode": True,
    },
    "openai": {
        "label": "OpenAI GPT",
        "base_url": "https://api.openai.com/v1",
        "model": "gpt-4o-mini",
        "provider": "openai",
        "api_style": "openai",
        "timeout_sec": 120,
        "temperature": 0.2,
        "max_tokens": 6000,
        "use_json_mode": True,
    },
    "vectorengine": {
        "label": "Vector Engine (GPT)",
        "base_url": "https://api.vectorengine.ai/v1",
        "model": "gpt-5.4",
        "provider": "vectorengine",
        "api_style": "openai",
        "timeout_sec": 120,
        "temperature": 0.2,
        "max_tokens": 6000,
        "use_json_mode": True,
    },
}

DIAGNOSIS_MIN_MAX_TOKENS = 6000

_FLAT_KEYS = (
    "api_key",
    "base_url",
    "model",
    "provider",
    "api_style",
    "timeout_sec",
    "temperature",
    "max_tokens",
    "use_json_mode",
)


def _normalize_provider_id(provider: str | None) -> str:
    p = (provider or "").strip().lower()
    p = PROVIDER_ALIASES.get(p, p)
    return p if p in PROVIDER_PRESETS else "deepseek"


def _read_config_file() -> dict[str, Any]:
    path = os.environ.get("TNA_AI_CONFIG_JSON", "").strip()
    if not path or not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    return raw if isinstance(raw, dict) else {}


def _providers_from_file(raw: dict[str, Any]) -> tuple[dict[str, dict[str, Any]], str]:
    """将 ai.local.json 规范为 providers 字典；兼容旧版扁平配置。"""
    default_provider = _normalize_provider_id(raw.get("default_provider") or "deepseek")
    providers: dict[str, dict[str, Any]] = {}
    if isinstance(raw.get("providers"), dict):
        for pid, cfg in raw["providers"].items():
            norm = _normalize_provider_id(str(pid))
            if norm in PROVIDER_PRESETS and isinstance(cfg, dict):
                providers[norm] = {k: v for k, v in cfg.items() if v is not None}
    else:
        flat = {k: raw[k] for k in _FLAT_KEYS if k in raw and raw[k] is not None}
        if flat:
            providers["deepseek"] = flat
    return providers, default_provider


def _apply_env_overrides(provider_id: str, cfg: dict[str, Any]) -> dict[str, Any]:
    out = dict(cfg)
    if provider_id == "deepseek":
        if os.environ.get("TNA_AI_API_KEY"):
            out["api_key"] = os.environ["TNA_AI_API_KEY"].strip()
        elif os.environ.get("DEEPSEEK_API_KEY"):
            out["api_key"] = os.environ["DEEPSEEK_API_KEY"].strip()
        if os.environ.get("TNA_AI_BASE_URL"):
            out["base_url"] = os.environ["TNA_AI_BASE_URL"].strip().rstrip("/")
        if os.environ.get("TNA_AI_MODEL"):
            out["model"] = os.environ["TNA_AI_MODEL"].strip()
        if os.environ.get("TNA_AI_FAST_MODEL"):
            out["model"] = os.environ["TNA_AI_FAST_MODEL"].strip()
    elif provider_id == "openai":
        if os.environ.get("OPENAI_API_KEY"):
            out["api_key"] = os.environ["OPENAI_API_KEY"].strip()
        elif os.environ.get("TNA_OPENAI_API_KEY"):
            out["api_key"] = os.environ["TNA_OPENAI_API_KEY"].strip()
        if os.environ.get("TNA_OPENAI_BASE_URL"):
            out["base_url"] = os.environ["TNA_OPENAI_BASE_URL"].strip().rstrip("/")
        if os.environ.get("TNA_OPENAI_MODEL"):
            out["model"] = os.environ["TNA_OPENAI_MODEL"].strip()
    elif provider_id == "vectorengine":
        if os.environ.get("VECTORENGINE_API_KEY"):
            out["api_key"] = os.environ["VECTORENGINE_API_KEY"].strip()
        elif os.environ.get("TNA_VECTORENGINE_API_KEY"):
            out["api_key"] = os.environ["TNA_VECTORENGINE_API_KEY"].strip()
        elif os.environ.get("ANTHROPIC_API_KEY"):
            out["api_key"] = os.environ["ANTHROPIC_API_KEY"].strip()
        if os.environ.get("TNA_VECTORENGINE_BASE_URL"):
            out["base_url"] = os.environ["TNA_VECTORENGINE_BASE_URL"].strip().rstrip("/")
        if os.environ.get("TNA_VECTORENGINE_MODEL"):
            out["model"] = os.environ["TNA_VECTORENGINE_MODEL"].strip()
    if os.environ.get("TNA_AI_TIMEOUT_SEC"):
        out["timeout_sec"] = int(os.environ["TNA_AI_TIMEOUT_SEC"])
    if os.environ.get("TNA_AI_MAX_TOKENS"):
        out["max_tokens"] = int(os.environ["TNA_AI_MAX_TOKENS"])
    return out


def resolve_llm_config(provider: str | None = None) -> dict[str, Any]:
    """按 provider 解析 LLM 配置。provider: deepseek | openai | gpt | vectorengine | claude。"""
    raw = _read_config_file()
    providers, default_provider = _providers_from_file(raw)
    pid = _normalize_provider_id(
        provider
        or os.environ.get("TNA_AI_LLM_PROVIDER")
        or raw.get("default_provider")
        or default_provider
    )
    cfg = copy.deepcopy(PROVIDER_PRESETS.get(pid, PROVIDER_PRESETS["deepseek"]))
    cfg.update(providers.get(pid, {}))
    cfg = _apply_env_overrides(pid, cfg)
    cfg["provider"] = pid
    cfg["provider_id"] = pid
    cfg["label"] = cfg.get("label") or PROVIDER_PRESETS[pid]["label"]
    if cfg.get("base_url"):
        cfg["base_url"] = str(cfg["base_url"]).rstrip("/")
    return cfg


def load_llm_config() -> dict[str, Any]:
    """默认 provider 的配置（兼容旧调用）。"""
    return resolve_llm_config(None)


def list_llm_providers() -> list[dict[str, Any]]:
    """供前端下拉：已配置与可选模型列表。"""
    raw = _read_config_file()
    providers, default_provider = _providers_from_file(raw)
    default_id = _normalize_provider_id(
        os.environ.get("TNA_AI_LLM_PROVIDER") or raw.get("default_provider") or default_provider
    )
    out: list[dict[str, Any]] = []
    for pid, preset in PROVIDER_PRESETS.items():
        cfg = resolve_llm_config(pid)
        out.append(
            {
                "id": pid,
                "label": preset["label"],
                "model": cfg.get("model"),
                "base_url": cfg.get("base_url"),
                "configured": bool(cfg.get("api_key")),
                "is_default": pid == default_id,
            }
        )
    return out


def require_api_key(cfg: dict[str, Any] | None = None) -> dict[str, Any]:
    c = cfg or load_llm_config()
    if not c.get("api_key"):
        pid = c.get("provider_id") or c.get("provider") or "deepseek"
        hint = {
            "deepseek": "TNA_AI_API_KEY 或 secrets/ai.local.json → providers.deepseek.api_key",
            "openai": "OPENAI_API_KEY 或 secrets/ai.local.json → providers.openai.api_key",
            "vectorengine": "VECTORENGINE_API_KEY 或 secrets/ai.local.json → providers.vectorengine.api_key",
        }.get(pid, "TNA_AI_CONFIG_JSON")
        raise LlmConfigError(
            f"未配置 {pid} 大模型 API Key。请设置 {hint}。"
            "参见 docs/ai-status-diagnosis-report设计与使用-2026-05-29.md"
        )
    return c


def _extract_json_object(text: str) -> dict[str, Any]:
    text = (text or "").strip()
    if not text:
        raise LlmCallError("模型返回为空")
    m = re.search(r"```(?:json)?\s*([\s\S]*?)\s*```", text, re.IGNORECASE)
    if m:
        text = m.group(1).strip()
    start = text.find("{")
    end = text.rfind("}")
    if start >= 0 and end > start:
        text = text[start : end + 1]
    try:
        obj = json.loads(text)
    except json.JSONDecodeError as e:
        raise LlmCallError(f"无法解析模型 JSON 输出: {e}") from e
    if not isinstance(obj, dict):
        raise LlmCallError("模型输出不是 JSON 对象")
    return obj


def _diagnosis_cfg(cfg: dict[str, Any] | None) -> dict[str, Any]:
    """诊断建议输出是结构化 JSON，不能沿用过小的通用输出上限。"""
    c = require_api_key(cfg)
    out = dict(c)
    out["max_tokens"] = max(int(out.get("max_tokens") or 0), DIAGNOSIS_MIN_MAX_TOKENS)
    out["timeout_sec"] = max(int(out.get("timeout_sec") or 0), 180)
    return out


def _compact_raw_for_repair(raw: str, limit: int = 12000) -> str:
    raw = (raw or "").strip()
    if len(raw) <= limit:
        return raw
    # JSON 截断通常发生在尾部，保留开头即可让修复模型补齐结构。
    return raw[:limit] + "\n/* 内容在此处被截断，请基于已有内容补全为合法 JSON */"


def _repair_json_sections(raw_text: str, parse_error: Exception | str, cfg: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    """模型返回半截 JSON 时，二次请求只做格式修复，不重新分析。"""
    repair_system = (
        "你是 JSON 修复器。只输出一个合法 JSON 对象，不要 Markdown，不要解释。"
        "对象字段必须包含 executive_summary, status_analysis, problems, recommendations, gis_operations。"
        "如果原文中某个数组或对象被截断，请保留已能确定的信息，并用简洁中文补齐或闭合结构。"
        "不要新增原文没有依据的数据。"
    )
    repair_user = (
        "下面是一次交通诊断大模型输出，但不是合法 JSON。"
        f"解析错误：{parse_error}\n\n"
        "请把它修复为合法 JSON 对象：\n"
        f"{_compact_raw_for_repair(raw_text)}"
    )
    repair_cfg = dict(cfg)
    repair_cfg["max_tokens"] = max(int(repair_cfg.get("max_tokens") or 0), DIAGNOSIS_MIN_MAX_TOKENS)
    repair_cfg["temperature"] = 0
    result = chat_completion(repair_system, repair_user, repair_cfg)
    sections = _extract_json_object(result["content"])
    return sections, {
        "repair_model": result.get("model"),
        "repair_usage": result.get("usage") or {},
        "repair_raw_response": result.get("content") or "",
    }


def _fallback_sections_from_raw_text(raw_text: str, parse_error: Exception | str) -> dict[str, Any]:
    """模型未返回合法 JSON 时的兜底结构。

    GPT/兼容模型偶尔会返回接近报告正文的自然语言，直接 502 会导致报告无法生成。
    这里把原文包装为 ai_sections 的标准字段，保证前端/HTML 可以展示，
    同时在 data_gaps 中保留解析错误，便于后续排查 prompt 或模型行为。
    """
    raw = (raw_text or "").strip()
    if len(raw) > 6000:
        raw_show = raw[:6000] + "\n……（模型原文过长，已截断展示；完整内容见 llm_trace.raw_response）"
    else:
        raw_show = raw
    err = str(parse_error)
    return {
        "executive_summary": "本次 AI 返回内容未能自动结构化，系统已保留原始文本供人工复核。",
        "status_analysis": raw_show or "AI 返回内容为空，无法生成结构化分析。",
        "problems": [],
        "recommendations": [
            {
                "title": "复核 AI 原始建议",
                "targets_problem": 0,
                "measures": ["检查 AI 原始输出并重新生成结构化建议。"],
                "expected_effect": "保证 AI 对话结果能够稳定展示。",
                "difficulty": "I",
                "duration": "即时",
            }
        ],
        "gis_operations": [],
        "data_gaps": [f"模型 JSON 解析失败，已启用兜底展示：{err}"],
        "_fallback_from_raw_text": True,
    }


def _is_anthropic_style(cfg: dict[str, Any]) -> bool:
    return (cfg.get("api_style") or "").strip().lower() == "anthropic"


def _anthropic_headers(api_key: str) -> dict[str, str]:
    return {
        "Content-Type": "application/json",
        "x-api-key": api_key,
        "anthropic-version": "2023-06-01",
        "Authorization": f"Bearer {api_key}",
    }


def _anthropic_messages_url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/v1/messages"


def _parse_anthropic_content(payload: dict[str, Any]) -> str:
    parts: list[str] = []
    for block in payload.get("content") or []:
        if isinstance(block, dict) and block.get("type") == "text":
            parts.append(str(block.get("text") or ""))
    return "".join(parts)


def anthropic_messages_completion(
    system_prompt: str,
    user_prompt: str,
    cfg: dict[str, Any],
) -> dict[str, Any]:
    """Anthropic Messages API（Vector Engine 等中转）。"""
    url = _anthropic_messages_url(str(cfg["base_url"]))
    body: dict[str, Any] = {
        "model": cfg["model"],
        "max_tokens": int(cfg.get("max_tokens") or 2200),
        "system": system_prompt,
        "messages": [{"role": "user", "content": user_prompt}],
    }
    if cfg.get("temperature") is not None:
        body["temperature"] = float(cfg["temperature"])
    payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=payload,
        headers=_anthropic_headers(str(cfg["api_key"])),
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=int(cfg.get("timeout_sec", 120))) as resp:
            raw = resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        raise LlmCallError(f"LLM HTTP {e.code}: {err_body[:500]}") from e
    except urllib.error.URLError as e:
        raise LlmCallError(f"LLM 网络错误: {e}") from e
    parsed = json.loads(raw)
    content = _parse_anthropic_content(parsed)
    usage = parsed.get("usage") or {}
    return {
        "content": content,
        "model": parsed.get("model") or cfg["model"],
        "usage": {
            "prompt_tokens": usage.get("input_tokens"),
            "completion_tokens": usage.get("output_tokens"),
        },
        "raw_response": raw,
    }


def stream_anthropic_messages(
    system_prompt: str,
    user_prompt: str,
    cfg: dict[str, Any],
) -> Iterator[dict[str, Any]]:
    """Anthropic Messages 流式（SSE）。"""
    url = _anthropic_messages_url(str(cfg["base_url"]))
    body: dict[str, Any] = {
        "model": cfg["model"],
        "max_tokens": int(cfg.get("max_tokens") or 2200),
        "system": system_prompt,
        "messages": [{"role": "user", "content": user_prompt}],
        "stream": True,
    }
    if cfg.get("temperature") is not None:
        body["temperature"] = float(cfg["temperature"])
    req = urllib.request.Request(
        url,
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={
            **_anthropic_headers(str(cfg["api_key"])),
            "Accept": "text/event-stream",
        },
        method="POST",
    )
    content_parts: list[str] = []
    model_name = cfg["model"]
    try:
        with urllib.request.urlopen(req, timeout=int(cfg.get("timeout_sec", 120))) as resp:
            event_name = ""
            while True:
                line = resp.readline()
                if not line:
                    break
                s = line.decode("utf-8", errors="replace").strip()
                if not s:
                    continue
                if s.startswith("event:"):
                    event_name = s[6:].strip()
                    continue
                if not s.startswith("data:"):
                    continue
                data = s[5:].strip()
                if not data:
                    continue
                try:
                    chunk = json.loads(data)
                except json.JSONDecodeError:
                    continue
                if isinstance(chunk.get("model"), str):
                    model_name = chunk["model"]
                if event_name == "content_block_delta":
                    delta = chunk.get("delta") or {}
                    text = delta.get("text") or ""
                    if text:
                        content_parts.append(text)
                        yield {"type": "content", "text": text}
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        raise LlmCallError(f"LLM HTTP {e.code}: {err_body[:500]}") from e
    except urllib.error.URLError as e:
        raise LlmCallError(f"LLM 网络错误: {e}") from e
    yield {
        "type": "done",
        "content": "".join(content_parts),
        "thinking": "",
        "model": model_name,
    }


def chat_completion(
    system_prompt: str,
    user_prompt: str,
    cfg: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """调用 chat/completions 或 Anthropic /v1/messages，返回 {content, model, usage, raw}。"""
    c = require_api_key(cfg)
    if _is_anthropic_style(c):
        return anthropic_messages_completion(system_prompt, user_prompt, c)
    url = f"{c['base_url']}/chat/completions"

    def _post(with_json_mode: bool) -> str:
        body: dict[str, Any] = {
            "model": c["model"],
            "temperature": float(c.get("temperature", 0.2)),
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
        }
        if with_json_mode and c.get("use_json_mode", True):
            body["response_format"] = {"type": "json_object"}
        max_tok = int(c.get("max_tokens") or 0)
        if max_tok > 0:
            body["max_tokens"] = max_tok
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            url,
            data=payload,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {c['api_key']}",
            },
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=int(c.get("timeout_sec", 120))) as resp:
            return resp.read().decode("utf-8")

    try:
        raw = _post(with_json_mode=True)
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        if e.code == 400 and "response_format" in err_body.lower():
            raw = _post(with_json_mode=False)
        else:
            raise LlmCallError(f"LLM HTTP {e.code}: {err_body[:500]}") from e
    except urllib.error.URLError as e:
        raise LlmCallError(f"LLM 网络错误: {e}") from e

    parsed = json.loads(raw)
    choices = parsed.get("choices") or []
    if not choices:
        raise LlmCallError("LLM 响应无 choices")
    content = (choices[0].get("message") or {}).get("content") or ""
    return {
        "content": content,
        "model": parsed.get("model") or c["model"],
        "usage": parsed.get("usage") or {},
        "raw_response": raw,
    }


def call_for_diagnosis_sections(
    system_prompt: str,
    user_prompt: str,
    cfg: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """
    调用 LLM 并解析为 ai_sections。
    返回 (ai_sections, llm_trace)。
    """
    c = _diagnosis_cfg(cfg)
    result = chat_completion(system_prompt, user_prompt, c)
    provider = c.get("provider_id") or c.get("provider") or "deepseek"
    parse_error = None
    repair_trace: dict[str, Any] = {}
    try:
        sections = _extract_json_object(result["content"])
    except LlmCallError as e:
        parse_error = e
        try:
            sections, repair_trace = _repair_json_sections(result["content"], e, c)
        except Exception as repair_error:
            sections = _fallback_sections_from_raw_text(result["content"], repair_error)
            repair_trace = {"repair_error": str(repair_error)}
    trace = {
        "source": "llm",
        "provider": provider,
        "base_url": c["base_url"],
        "model": result["model"],
        "api_key_configured": True,
        "system_prompt": system_prompt,
        "user_prompt": user_prompt,
        "raw_response": result["content"],
        "usage": result.get("usage") or {},
    }
    if parse_error is not None:
        trace["json_parse_error"] = str(parse_error)
        trace["json_repaired"] = bool(repair_trace and not repair_trace.get("repair_error"))
        trace.update(repair_trace)
        if repair_trace.get("repair_error"):
            trace["fallback_from_raw_text"] = True
    return sections, trace


def stream_chat_completion(
    system_prompt: str,
    user_prompt: str,
    cfg: dict[str, Any] | None = None,
    *,
    json_mode: bool = True,
) -> Iterator[dict[str, Any]]:
    """
    流式调用 chat/completions 或 Anthropic /v1/messages。
    yield: {"type": "thinking"|"content", "text": "..."}
    结束时 yield: {"type": "done", "content": full_content, "thinking": full_thinking, "model": ...}
    """
    c = _diagnosis_cfg(cfg)
    if _is_anthropic_style(c):
        yield from stream_anthropic_messages(system_prompt, user_prompt, c)
        return
    url = f"{c['base_url']}/chat/completions"
    body: dict[str, Any] = {
        "model": c["model"],
        "temperature": float(c.get("temperature", 0.2)),
        "stream": True,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
    }
    if json_mode and c.get("use_json_mode", True):
        body["response_format"] = {"type": "json_object"}
    max_tok = int(c.get("max_tokens") or 0)
    if max_tok > 0:
        body["max_tokens"] = max_tok

    thinking_parts: list[str] = []
    content_parts: list[str] = []
    model_name = c["model"]

    def _read_stream(with_json: bool) -> Iterator[dict[str, Any]]:
        nonlocal model_name
        req_body = dict(body)
        if not with_json:
            req_body.pop("response_format", None)
        req_data = json.dumps(req_body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=req_data,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {c['api_key']}",
                "Accept": "text/event-stream",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=int(c.get("timeout_sec", 120))) as resp:
            while True:
                line = resp.readline()
                if not line:
                    break
                s = line.decode("utf-8", errors="replace").strip()
                if not s or not s.startswith("data:"):
                    continue
                data = s[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    chunk = json.loads(data)
                except json.JSONDecodeError:
                    continue
                model_name = chunk.get("model") or model_name
                for ch in chunk.get("choices") or []:
                    delta = ch.get("delta") or {}
                    rc = delta.get("reasoning_content") or delta.get("reasoning") or ""
                    ct = delta.get("content") or ""
                    if rc:
                        thinking_parts.append(rc)
                        yield {"type": "thinking", "text": rc}
                    if ct:
                        content_parts.append(ct)
                        yield {"type": "content", "text": ct}

    try:
        for ev in _read_stream(with_json=json_mode):
            yield ev
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        if e.code == 400 and "response_format" in err_body.lower() and json_mode:
            thinking_parts.clear()
            content_parts.clear()
            for ev in _read_stream(with_json=False):
                yield ev
        else:
            raise LlmCallError(f"LLM HTTP {e.code}: {err_body[:500]}") from e
    except urllib.error.URLError as e:
        raise LlmCallError(f"LLM 网络错误: {e}") from e

    full_content = "".join(content_parts)
    full_thinking = "".join(thinking_parts)
    yield {
        "type": "done",
        "content": full_content,
        "thinking": full_thinking,
        "model": model_name,
    }


def stream_for_diagnosis_sections(
    system_prompt: str,
    user_prompt: str,
    cfg: dict[str, Any] | None = None,
) -> Iterator[dict[str, Any]]:
    """流式生成并解析 ai_sections。"""
    c = require_api_key(cfg)
    full_content = ""
    full_thinking = ""
    model = c["model"]
    for ev in stream_chat_completion(system_prompt, user_prompt, c):
        if ev.get("type") == "done":
            full_content = ev.get("content") or ""
            full_thinking = ev.get("thinking") or ""
            model = ev.get("model") or model
            continue
        yield ev
    provider = c.get("provider_id") or c.get("provider") or "deepseek"
    parse_error = None
    repair_trace: dict[str, Any] = {}
    try:
        sections = _extract_json_object(full_content)
    except LlmCallError as e:
        parse_error = e
        try:
            sections, repair_trace = _repair_json_sections(full_content, e, c)
        except Exception as repair_error:
            sections = _fallback_sections_from_raw_text(full_content, repair_error)
            repair_trace = {"repair_error": str(repair_error)}
    trace = {
        "source": "llm_stream",
        "provider": provider,
        "base_url": c["base_url"],
        "model": model,
        "thinking": full_thinking,
        "raw_response": full_content,
    }
    if parse_error is not None:
        trace["json_parse_error"] = str(parse_error)
        trace["json_repaired"] = bool(repair_trace and not repair_trace.get("repair_error"))
        trace.update(repair_trace)
        if repair_trace.get("repair_error"):
            trace["fallback_from_raw_text"] = True
    yield {"type": "result", "ai_sections": sections, "llm_trace": trace}
