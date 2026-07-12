"""对外服务入口：采集 →（可选）调用 LLM → HTML。"""
from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

from .data_collector import collect_payload
from .html_report import render_html
from .advice_formatter import format_advice_text
from .llm_client import (
    LlmCallError,
    LlmConfigError,
    call_for_diagnosis_sections,
    resolve_llm_config,
    stream_for_diagnosis_sections,
)
from .narrative import generate_ai_sections, merge_ai_sections
from .prompt_builder import build_prompts
from .scope_utils import normalize_ai_sections, resolve_scope


def fetch_report_payload(
    project_id: int,
    user_id: int,
    case_id: int,
    scope: str = "all",
    *,
    prompt_blocks: dict[str, Any] | None = None,
    include_all_tables: bool = False,
    table_sample_rows: int = 0,
    diagnosis_limit: int = 12,
) -> dict[str, Any]:
    scope = resolve_scope(scope, prompt_blocks)
    return collect_payload(
        project_id,
        user_id,
        case_id,
        scope=scope,
        prompt_blocks=prompt_blocks,
        include_all_tables=include_all_tables,
        table_sample_rows=table_sample_rows,
        diagnosis_limit=diagnosis_limit,
    )


def invoke_llm_for_sections(
    payload: dict[str, Any],
    prompt_blocks: dict[str, Any] | None = None,
    *,
    compact_prompt: bool = True,
    llm_provider: str | None = None,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, str]]:
    """
    调用大模型生成 ai_sections。
    返回 (ai_sections, llm_trace, prompts_dict)。
    """
    prompts = build_prompts(payload, prompt_blocks, compact=compact_prompt)
    cfg = resolve_llm_config(llm_provider)
    sections, trace = call_for_diagnosis_sections(
        prompts["system_prompt"],
        prompts["user_prompt"],
        cfg,
    )
    trace["prompt_blocks"] = prompt_blocks or {}
    trace["prompt_blocks_text"] = prompts["prompt_blocks_text"]
    return sections, trace, prompts


def invoke_llm_for_sections_stream(
    payload: dict[str, Any],
    prompt_blocks: dict[str, Any] | None = None,
    *,
    compact_prompt: bool = True,
    llm_provider: str | None = None,
):
    """流式调用 LLM；yield thinking/content 块，最后 yield result dict。"""
    prompts = build_prompts(payload, prompt_blocks, compact=compact_prompt)
    cfg = resolve_llm_config(llm_provider)
    for ev in stream_for_diagnosis_sections(
        prompts["system_prompt"],
        prompts["user_prompt"],
        cfg,
    ):
        if ev.get("type") == "result":
            trace = ev.get("llm_trace") or {}
            trace["prompt_blocks"] = prompt_blocks or {}
            trace["prompt_blocks_text"] = prompts["prompt_blocks_text"]
            trace["system_prompt"] = prompts["system_prompt"]
            trace["user_prompt"] = prompts["user_prompt"]
            yield {
                "type": "result",
                "ai_sections": ev.get("ai_sections"),
                "llm_trace": trace,
                "prompts": prompts,
            }
        else:
            yield ev




def resolve_ai_dialog_path(
    project_id: int,
    user_id: int,
    case_id: int,
    *,
    scheme_kind: str | None = None,
) -> Path:
    """Default: /tmp/ai_dialog/project{p}_user{u}_case{c}_{base|normal}_scheme.md"""
    sk = (scheme_kind or "base").strip().lower()
    suffix = "base_scheme" if sk in ("", "base") else "normal_scheme"
    return Path(f"/tmp/ai_dialog/project{project_id}_user{user_id}_case{case_id}_{suffix}.md")


def save_advice_dialog_md(
    project_id: int,
    user_id: int,
    case_id: int,
    advice_text: str,
    *,
    scheme_kind: str | None = None,
) -> str:
    path = resolve_ai_dialog_path(project_id, user_id, case_id, scheme_kind=scheme_kind)
    text = (advice_text or "").strip()
    if not text:
        return str(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + "\n", encoding="utf-8")
    return str(path)

def generate_ai_advice(
    project_id: int,
    user_id: int,
    case_id: int,
    *,
    scope: str = "motor",
    prompt_blocks: dict[str, Any] | None = None,
    stream: bool = False,
    on_chunk=None,
    llm_provider: str | None = None,
) -> dict[str, Any]:
    """
    仅输出 AI 建议文本（无 HTML 报告）。
    stream=True 时通过 on_chunk(ev) 推送 thinking/content。
    """
    scope = resolve_scope(scope, prompt_blocks)
    payload = collect_payload(
        project_id,
        user_id,
        case_id,
        scope=scope,
        prompt_blocks=prompt_blocks,
        include_all_tables=False,
    )
    if stream:
        sections = None
        trace = {}
        prompts = {}
        for ev in invoke_llm_for_sections_stream(
            payload, prompt_blocks, llm_provider=llm_provider
        ):
            if on_chunk:
                on_chunk(ev)
            if ev.get("type") == "result":
                sections = ev.get("ai_sections")
                trace = ev.get("llm_trace") or {}
                prompts = ev.get("prompts") or {}
        if sections is None:
            raise LlmCallError("流式调用未返回结果")
        sections = normalize_ai_sections(sections)
    else:
        sections, trace, prompts = invoke_llm_for_sections(
            payload, prompt_blocks, llm_provider=llm_provider
        )
        sections = normalize_ai_sections(sections)

    advice_text = format_advice_text(sections, scope=scope)
    sk = (prompt_blocks or {}).get("scheme_kind") or "base"
    dialog_path = save_advice_dialog_md(
        project_id, user_id, case_id, advice_text, scheme_kind=sk,
    )
    return {
        "status": "ok",
        "advice_text": advice_text,
        "ai_sections": sections,
        "meta": payload.get("meta"),
        "data_gaps": payload.get("data_gaps"),
        "kpi_summary_text": payload.get("kpi_summary_text"),
        "llm_trace": _public_llm_trace(trace),
        "thinking": trace.get("thinking", ""),
        "dialog_path": dialog_path,
        "saved_path": dialog_path,
    }


def build_status_diagnosis_report(
    project_id: int,
    user_id: int,
    case_id: int,
    *,
    scope: str = "all",
    prompt_blocks: dict[str, Any] | None = None,
    ai_sections: dict[str, Any] | None = None,
    use_ai: bool = True,
    allow_rule_fallback: bool = False,
    output_path: str | Path | None = None,
    reports_dir: str | Path | None = None,
    include_all_tables: bool = False,
    table_sample_rows: int = 0,
    fast_mode: bool = True,
    llm_provider: str | None = None,
) -> dict[str, Any]:
    """
    生成现状交通诊断 HTML 报告。

    :param use_ai: True 时必须配置 TNA_AI_API_KEY（或 TNA_AI_CONFIG_JSON），并调用大模型
    :param allow_rule_fallback: 仅当 use_ai=True 但无 API Key 时，是否退回规则引擎（报告会标注「非 AI」）
    :param ai_sections: 若软件/ Cursor 已调用过 LLM，可直接传入，跳过本次 API 调用
    """
    if project_id <= 0 or user_id <= 0:
        raise ValueError("project_id and user_id must be positive integers")
    if case_id < 0:
        raise ValueError("case_id must be >= 0 (scheme id, including base scheme 0)")

    scope = resolve_scope(scope, prompt_blocks)
    compact = fast_mode
    scan_tables = include_all_tables or (not fast_mode)
    t0 = time.perf_counter()
    payload = collect_payload(
        project_id,
        user_id,
        case_id,
        scope=scope,
        prompt_blocks=prompt_blocks,
        include_all_tables=scan_tables,
        table_sample_rows=table_sample_rows,
        diagnosis_limit=12 if fast_mode else 40,
    )
    t_collect = time.perf_counter() - t0
    t_llm = 0.0
    prompts = build_prompts(payload, prompt_blocks, compact=compact)
    llm_cfg = resolve_llm_config(llm_provider)
    llm_trace: dict[str, Any] = {
        "source": "unknown",
        "provider": llm_cfg.get("provider_id") or llm_cfg.get("provider"),
        "model": llm_cfg.get("model"),
        "api_key_configured": bool(llm_cfg.get("api_key")),
    }

    if ai_sections is not None:
        sections = merge_ai_sections(
            generate_ai_sections(payload, prompt_blocks),
            ai_sections,
        )
        llm_trace.update(
            {
                "source": "client_provided",
                "note": "ai_sections 由调用方传入，本服务未重复调用大模型",
                "system_prompt": prompts["system_prompt"],
                "user_prompt": prompts["user_prompt"],
                "prompt_blocks": prompt_blocks or {},
                "prompt_blocks_text": prompts["prompt_blocks_text"],
            }
        )
    elif use_ai:
        try:
            t1 = time.perf_counter()
            sections, llm_trace, prompts = invoke_llm_for_sections(
                payload, prompt_blocks, compact_prompt=compact, llm_provider=llm_provider
            )
            t_llm = time.perf_counter() - t1
            # 大模型成功：HTML 章节仅来自模型 JSON，不与规则引擎拼接
            sections = normalize_ai_sections(sections)
        except LlmConfigError:
            if not allow_rule_fallback:
                raise
            generated = generate_ai_sections(payload, prompt_blocks)
            sections = generated
            llm_trace = {
                "source": "rule_fallback",
                "api_key_configured": False,
                "error": "未配置 TNA_AI_API_KEY，已使用规则引擎占位文本（非真实 AI 建议）",
                "system_prompt": prompts["system_prompt"],
                "user_prompt": prompts["user_prompt"],
                "prompt_blocks": prompt_blocks or {},
                "prompt_blocks_text": prompts["prompt_blocks_text"],
                "raw_response": "",
            }
        except Exception as e:
            llm_trace = {
                "source": "llm_failed",
                "error": str(e),
                "system_prompt": prompts["system_prompt"],
                "user_prompt": prompts["user_prompt"],
                "prompt_blocks": prompt_blocks or {},
            }
            raise
    else:
        sections = generate_ai_sections(payload, prompt_blocks)
        llm_trace = {
            "source": "rule_only",
            "note": "use_ai=False，未调用大模型",
            "system_prompt": prompts["system_prompt"],
            "user_prompt": prompts["user_prompt"],
            "prompt_blocks": prompt_blocks or {},
            "prompt_blocks_text": prompts["prompt_blocks_text"],
        }

    report_ctx = {
        "prompt_blocks": prompt_blocks or {},
        "prompts": prompts,
        "llm_trace": llm_trace,
    }

    t2 = time.perf_counter()
    html = render_html(payload, sections, report_ctx)
    t_html = time.perf_counter() - t2

    if output_path:
        out = Path(output_path)
    else:
        root = Path(reports_dir) if reports_dir else Path(__file__).resolve().parent.parent / "reports"
        root.mkdir(parents=True, exist_ok=True)
        out = root / f"ai_status_diagnosis_{project_id}_{user_id}_{case_id}.html"

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(html, encoding="utf-8")

    result_timing: dict[str, Any] = {
        "collect_sec": round(t_collect, 2),
        "llm_sec": round(t_llm, 2),
        "html_sec": round(t_html, 2),
        "fast_mode": fast_mode,
        "total_sec": round(t_collect + t_llm + t_html, 2),
    }

    return {
        "status": "ok",
        "html_path": str(out),
        "meta": payload.get("meta"),
        "data_gaps": payload.get("data_gaps"),
        "kpi_summary_text": payload.get("kpi_summary_text"),
        "ai_sections": sections,
        "llm_trace": _public_llm_trace(llm_trace),
        "timing": result_timing,
        "logs": payload.get("logs"),
    }


def _public_llm_trace(trace: dict[str, Any]) -> dict[str, Any]:
    """返回给 MCP/调用方：不含 api_key。"""
    t = dict(trace)
    t.pop("api_key", None)
    return t


def build_prompt_for_ai(payload: dict[str, Any], prompt_blocks: dict[str, Any] | None = None) -> str:
    """兼容旧接口：返回完整用户提示词。"""
    return build_prompts(payload, prompt_blocks)["user_prompt"]
