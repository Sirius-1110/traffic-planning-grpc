"""
现状交通 AI 诊断 — HTTP 服务（供 NestJS / Java / 其他服务端调用，非 Cursor MCP）。

启动（工作区根目录）：
  pip install -r ai_diagnosis/requirements-server.txt
  python -m ai_diagnosis.http_server

环境变量见 docs/ai-diagnosis-http-service部署-2026-05-29.md
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Header
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel, Field, model_validator

from .data_collector import collect_payload
from .llm_client import LlmConfigError, list_llm_providers, load_llm_config
from .prompt_builder import build_prompts
from .faq_prompts import list_faq_options
from .indicator_catalog import list_common_indicators
from .prompt_defaults import apply_legacy_prompt_defaults, normalize_scheme_kind
from .scope_utils import resolve_scheme_type
from .service import (
    build_status_diagnosis_report,
    fetch_report_payload,
    generate_ai_advice,
    invoke_llm_for_sections,
    invoke_llm_for_sections_stream,
)

# 报告输出根目录（服务器默认与部署路径一致）
_REPORT_ROOT = Path(
    os.environ.get(
        "TNA_AI_REPORT_DIR",
        str(Path(__file__).resolve().parent.parent / "reports"),
    )
).resolve()

_SERVICE_TOKEN = os.environ.get("TNA_AI_SERVICE_TOKEN", "").strip()

app = FastAPI(
    title="TNA AI Status Diagnosis Service",
    description="读 PostgreSQL + DeepSeek/OpenAI → 图文并茂 HTML 诊断报告",
    version="1.0.0",
)


def _check_token(authorization: str | None) -> None:
    if not _SERVICE_TOKEN:
        return
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(401, "缺少 Authorization: Bearer <TNA_AI_SERVICE_TOKEN>")
    token = authorization[7:].strip()
    if token != _SERVICE_TOKEN:
        raise HTTPException(403, "无效的服务令牌")


def _apply_legacy_scope_alias(data: dict) -> dict:
    """旧字段 scope 与 scheme_type 同义，自动转到 scheme_type。"""
    if not (str(data.get("scheme_type") or "").strip()) and str(data.get("scope") or "").strip():
        data["scheme_type"] = data["scope"]
    pb = data.get("prompt_blocks")
    if isinstance(pb, dict):
        if not (str(pb.get("scheme_type") or "").strip()) and str(pb.get("scope") or "").strip():
            pb["scheme_type"] = pb["scope"]
    return data


class PromptBlocks(BaseModel):
    analysis_object: str = ""
    business_goal: str = ""  # 空则按 scheme_kind 推断，见 prompt_defaults
    planning_targets: str = ""
    constraints: str = ""
    engineer_notes: str = ""
    # AI 智能诊断三项 UI（2026-06）
    scheme_type: str = ""
    indicator_codes: list[str] = Field(default_factory=list)
    faq_id: str = ""
    custom_question: str = ""
    bbox: list[float] | None = None
    link_ids: list[int] = Field(default_factory=list)
    line_ids: list[int] = Field(default_factory=list)
    node_ids: list[int] = Field(default_factory=list)
    object_ids: list[str] = Field(default_factory=list)

    class Config:
        extra = "allow"


class LlmProviderMixin(BaseModel):
    llm_provider: str = Field(
        "",
        description="大模型提供方：deepseek | openai | gpt | vectorengine | claude；空则使用 default_provider",
    )


class ReportGenerateRequest(LlmProviderMixin):
    project_id: int = Field(..., gt=0)
    user_id: int = Field(..., gt=0)
    case_id: int = Field(..., ge=0)
    scheme_type: str = Field("motor", description="motor=机动车 slow=慢行 pt=公交")
    scheme_kind: str = Field("", description="base=基础方案页 scheme=普通方案页")
    base_case_id: int | None = Field(
        None,
        ge=0,
        description="改扩建对比基准方案 ID；须为真实基础方案（如 project12 用 2，勿传 0）",
    )
    prompt_blocks: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="before")
    @classmethod
    def _legacy_aliases(cls, data: Any) -> Any:
        if isinstance(data, dict):
            data = _apply_legacy_scope_alias(data)
            sk = normalize_scheme_kind(data.get("scheme_kind"))
            if sk:
                data["scheme_kind"] = sk
        return data
    ai_sections: dict[str, Any] | None = None
    use_ai: bool = True
    allow_rule_fallback: bool = False
    output_path: str | None = None
    return_html: bool = False
    include_all_tables: bool = False
    table_sample_rows: int = Field(0, ge=0, le=10)
    fast_mode: bool = True


class AiSectionsRequest(LlmProviderMixin):
    project_id: int = Field(..., gt=0)
    user_id: int = Field(..., gt=0)
    case_id: int = Field(..., ge=0)
    scheme_type: str = Field("motor", description="motor=机动车 slow=慢行 pt=公交")
    scheme_kind: str = Field("", description="base=基础方案页 scheme=普通方案页")
    base_case_id: int | None = Field(
        None,
        ge=0,
        description="改扩建对比基准方案 ID；须为真实基础方案（如 project12 用 2，勿传 0）",
    )
    prompt_blocks: dict[str, Any] = Field(default_factory=dict)
    include_all_tables: bool = False
    table_sample_rows: int = Field(0, ge=0, le=10)

    @model_validator(mode="before")
    @classmethod
    def _legacy_aliases(cls, data: Any) -> Any:
        if isinstance(data, dict):
            data = _apply_legacy_scope_alias(data)
            sk = normalize_scheme_kind(data.get("scheme_kind"))
            if sk:
                data["scheme_kind"] = sk
        return data


def _effective_scheme_type(req: AiSectionsRequest | ReportGenerateRequest | "AdviceStreamRequest") -> str:
    pb = getattr(req, "prompt_blocks", {}) or {}
    return resolve_scheme_type(getattr(req, "scheme_type", ""), prompt_blocks=pb, default="motor")


def _payload_kwargs(req: AiSectionsRequest) -> dict[str, Any]:
    st = _effective_scheme_type(req)
    pb = dict(req.prompt_blocks or {})
    pb.setdefault("scheme_type", st)
    sk = getattr(req, "scheme_kind", "") or ""
    pb = apply_legacy_prompt_defaults(
        pb, scheme_kind=sk, base_case_id=getattr(req, "base_case_id", None)
    )
    return {
        "scope": st,
        "prompt_blocks": pb,
        "include_all_tables": req.include_all_tables,
        "table_sample_rows": req.table_sample_rows,
    }


@app.get("/health")
def health():
    cfg = load_llm_config()
    providers = list_llm_providers()
    return {
        "status": "ok",
        "report_root": str(_REPORT_ROOT),
        "ai_configured": bool(cfg.get("api_key")),
        "ai_model": cfg.get("model"),
        "ai_base_url": cfg.get("base_url"),
        "default_llm_provider": next(
            (p["id"] for p in providers if p.get("is_default")), "deepseek"
        ),
        "llm_providers": providers,
    }


@app.get("/v1/llm/providers")
def v1_llm_providers(authorization: str | None = Header(None)):
    """供前端下拉：可选大模型及是否已配置 Key。"""
    _check_token(authorization)
    providers = list_llm_providers()
    return {
        "default_provider": next(
            (p["id"] for p in providers if p.get("is_default")), "deepseek"
        ),
        "providers": providers,
    }


@app.get("/v1/config")
def v1_config(authorization: str | None = Header(None)):
    _check_token(authorization)
    cfg = load_llm_config()
    return {
        "ai_configured": bool(cfg.get("api_key")),
        "model": cfg.get("model"),
        "base_url": cfg.get("base_url"),
        "provider": cfg.get("provider"),
        "report_root": str(_REPORT_ROOT),
        "llm_providers": list_llm_providers(),
    }


@app.get("/v1/indicators/common")
def v1_common_indicators(scheme_type: str = "motor", authorization: str | None = Header(None)):
    """第2项：常用诊断指标列表。"""
    _check_token(authorization)
    return {"scheme_type": scheme_type, "indicators": list_common_indicators(scheme_type)}


@app.get("/v1/faq/list")
def v1_faq_list(
    case_id: int = 2,
    scheme_kind: str = "scheme",
    authorization: str | None = Header(None),
):
    """预设常见问题。scheme_kind：base=基础方案页 scheme=普通方案页。"""
    _check_token(authorization)
    if case_id < 0:
        raise HTTPException(400, "case_id 须 >= 0")
    sk = normalize_scheme_kind(scheme_kind) or "scheme"
    return {
        "case_id": case_id,
        "scheme_kind": sk,
        "questions": list_faq_options(case_id, scheme_kind=sk),
    }


@app.get("/v1/tables/list")
def v1_tables_list(
    project_id: int,
    user_id: int,
    case_id: int,
    scope: str = "all",
    authorization: str | None = Header(None),
):
    """仅列出 server-workspace 登记表 + 库中同前缀表的存在性与行数。"""
    _check_token(authorization)
    payload = fetch_report_payload(
        project_id, user_id, case_id, **_payload_kwargs(
            AiSectionsRequest(
                project_id=project_id,
                user_id=user_id,
                case_id=case_id,
                scope=scope,
            )
        ),
    )
    return payload.get("tables", {})


@app.post("/v1/data/payload")
def v1_data_payload(req: AiSectionsRequest, authorization: str | None = Header(None)):
    """返回数据库采集 JSON（供 Node MCP / 前端预览）。"""
    _check_token(authorization)
    payload = fetch_report_payload(
        req.project_id, req.user_id, req.case_id, **_payload_kwargs(req)
    )
    return payload


@app.post("/v1/report/prompt")
def v1_build_prompt(req: AiSectionsRequest, authorization: str | None = Header(None)):
    _check_token(authorization)
    payload = fetch_report_payload(
        req.project_id, req.user_id, req.case_id, **_payload_kwargs(req)
    )
    prompts = build_prompts(payload, req.prompt_blocks)
    return {
        "system_prompt": prompts["system_prompt"],
        "user_prompt": prompts["user_prompt"],
        "kpi_summary_text": payload.get("kpi_summary_text"),
        "data_gaps": payload.get("data_gaps"),
    }


@app.post("/v1/report/ai-sections")
def v1_ai_sections(req: AiSectionsRequest, authorization: str | None = Header(None)):
    _check_token(authorization)
    payload = fetch_report_payload(
        req.project_id, req.user_id, req.case_id, **_payload_kwargs(req)
    )
    try:
        sections, trace, prompts = invoke_llm_for_sections(
            payload, req.prompt_blocks, llm_provider=req.llm_provider or None
        )
    except LlmConfigError as e:
        raise HTTPException(503, str(e)) from e
    except Exception as e:
        raise HTTPException(502, f"大模型调用失败: {e}") from e
    return {
        "ai_sections": sections,
        "llm_trace": {k: v for k, v in trace.items() if k != "api_key"},
        "system_prompt": prompts["system_prompt"],
        "user_prompt": prompts["user_prompt"],
    }


@app.post("/v1/report/generate")
def v1_generate_report(req: ReportGenerateRequest, authorization: str | None = Header(None)):
    """一键生成 HTML 报告（主接口，供业务服务端调用）。"""
    _check_token(authorization)
    out_path = req.output_path
    if out_path:
        out_path = str(Path(out_path))
    else:
        _REPORT_ROOT.mkdir(parents=True, exist_ok=True)
        out_path = str(
            _REPORT_ROOT / f"ai_status_diagnosis_{req.project_id}_{req.user_id}_{req.case_id}.html"
        )

    try:
        st = _effective_scheme_type(req)
        pb = dict(req.prompt_blocks or {})
        pb.setdefault("scheme_type", st)
        pb = apply_legacy_prompt_defaults(
            pb,
            scheme_kind=getattr(req, "scheme_kind", "") or "",
            base_case_id=getattr(req, "base_case_id", None),
        )
        result = build_status_diagnosis_report(
            req.project_id,
            req.user_id,
            req.case_id,
            scope=st,
            prompt_blocks=pb,
            ai_sections=req.ai_sections,
            use_ai=req.use_ai,
            allow_rule_fallback=req.allow_rule_fallback,
            output_path=out_path,
            reports_dir=str(_REPORT_ROOT),
            include_all_tables=req.include_all_tables,
            table_sample_rows=req.table_sample_rows,
            fast_mode=req.fast_mode,
            llm_provider=req.llm_provider or None,
        )
    except LlmConfigError as e:
        raise HTTPException(503, str(e)) from e
    except ValueError as e:
        raise HTTPException(400, str(e)) from e
    except Exception as e:
        raise HTTPException(500, f"报告生成失败: {e}") from e

    html_path = Path(result["html_path"])
    resp: dict[str, Any] = {
        "status": "ok",
        "html_path": str(html_path),
        "html_filename": html_path.name,
        "download_url": f"/v1/report/files/{html_path.name}",
        "meta": result.get("meta"),
        "data_gaps": result.get("data_gaps"),
        "llm_trace": result.get("llm_trace"),
        "timing": result.get("timing"),
    }
    if req.return_html and html_path.is_file():
        resp["html_content"] = html_path.read_text(encoding="utf-8")
    return resp


class AdviceStreamRequest(LlmProviderMixin):
    project_id: int = Field(..., gt=0)
    user_id: int = Field(..., gt=0)
    case_id: int = Field(..., ge=0)
    scheme_type: str = Field("motor", description="motor=机动车 slow=慢行 pt=公交")
    scheme_kind: str = Field("", description="base=基础方案页 scheme=普通方案页")
    base_case_id: int | None = Field(
        None,
        ge=0,
        description="改扩建对比基准方案 ID；须为真实基础方案（如 project12 用 2，勿传 0）",
    )
    prompt_blocks: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="before")
    @classmethod
    def _legacy_aliases(cls, data: Any) -> Any:
        if isinstance(data, dict):
            data = _apply_legacy_scope_alias(data)
            sk = normalize_scheme_kind(data.get("scheme_kind"))
            if sk:
                data["scheme_kind"] = sk
        return data


def _advice_prompt_blocks(req: AdviceStreamRequest) -> dict[str, Any]:
    st = _effective_scheme_type(req)
    pb = dict(req.prompt_blocks or {})
    pb.setdefault("scheme_type", st)
    return apply_legacy_prompt_defaults(
        pb,
        scheme_kind=getattr(req, "scheme_kind", "") or "",
        base_case_id=getattr(req, "base_case_id", None),
    )


@app.post("/v1/advice/generate")
def v1_advice_generate(req: AdviceStreamRequest, authorization: str | None = Header(None)):
    """仅返回 AI 建议 JSON（无 HTML）。"""
    _check_token(authorization)
    st = _effective_scheme_type(req)
    pb = _advice_prompt_blocks(req)
    sk = getattr(req, "scheme_kind", "") or "base"
    pb = dict(pb)
    pb.setdefault("scheme_kind", sk)
    try:
        return generate_ai_advice(
            req.project_id,
            req.user_id,
            req.case_id,
            scope=st,
            prompt_blocks=pb,
            stream=False,
            llm_provider=req.llm_provider or None,
        )
    except LlmConfigError as e:
        raise HTTPException(503, str(e)) from e
    except Exception as e:
        raise HTTPException(502, str(e)) from e


@app.post("/v1/advice/stream")
def v1_advice_stream(req: AdviceStreamRequest, authorization: str | None = Header(None)):
    """SSE 流式输出 thinking + content，最后 event=result 含 advice_text。"""
    _check_token(authorization)
    st = _effective_scheme_type(req)
    pb = _advice_prompt_blocks(req)

    def _gen():
        from .data_collector import collect_payload
        from .scope_utils import normalize_ai_sections
        from .advice_formatter import format_advice_text

        payload = collect_payload(
            req.project_id, req.user_id, req.case_id, scope=st, prompt_blocks=pb
        )
        try:
            for ev in invoke_llm_for_sections_stream(
                payload, pb, llm_provider=req.llm_provider or None
            ):
                if ev.get("type") in ("thinking", "content"):
                    yield f"data: {json.dumps(ev, ensure_ascii=False)}\n\n"
                elif ev.get("type") == "result":
                    sections = normalize_ai_sections(ev.get("ai_sections"))
                    out = {
                        "type": "result",
                        "advice_text": format_advice_text(sections, scope=st),
                        "ai_sections": sections,
                        "meta": payload.get("meta"),
                        "data_gaps": payload.get("data_gaps"),
                    }
                    yield f"data: {json.dumps(out, ensure_ascii=False)}\n\n"
            yield "data: [DONE]\n\n"
        except LlmConfigError as e:
            yield f"data: {json.dumps({'type':'error','message':str(e)}, ensure_ascii=False)}\n\n"

    return StreamingResponse(_gen(), media_type="text/event-stream")


@app.get("/v1/report/files/{filename}")
def v1_download_report(filename: str, authorization: str | None = Header(None)):
    _check_token(authorization)
    safe = Path(filename).name
    if safe != filename or ".." in filename:
        raise HTTPException(400, "非法文件名")
    path = (_REPORT_ROOT / safe).resolve()
    if not str(path).startswith(str(_REPORT_ROOT)):
        raise HTTPException(403, "路径越界")
    if not path.is_file():
        raise HTTPException(404, "报告不存在")
    return FileResponse(path, media_type="text/html; charset=utf-8", filename=safe)


def main():
    import uvicorn

    host = os.environ.get("TNA_AI_SERVICE_HOST", "127.0.0.1")
    port = int(os.environ.get("TNA_AI_SERVICE_PORT", "18080"))
    uvicorn.run(
        "ai_diagnosis.http_server:app",
        host=host,
        port=port,
        reload=False,
        log_level="info",
    )


if __name__ == "__main__":
    main()
