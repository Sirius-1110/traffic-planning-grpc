"""L4 多级提示词兼容字段默认值（暂时保留，见交付文档 §2.2）。"""
from __future__ import annotations

from typing import Any

# 旧版 L4 业务类型；三项 UI 不展示该选项，但 prompt_blocks 仍接受显式传入
BUSINESS_GOAL_STATUS_DIAGNOSIS = "现状诊断"
BUSINESS_GOAL_SCHEME_ADVICE = "方案建议"

DEFAULT_BUSINESS_GOAL_BY_SCHEME_KIND: dict[str, str] = {
    "base": BUSINESS_GOAL_STATUS_DIAGNOSIS,
    "scheme": BUSINESS_GOAL_SCHEME_ADVICE,
}

SCHEME_KIND_LABELS = {
    "base": "基础方案页（问现状、瓶颈）",
    "scheme": "普通方案页（问改造、对比）",
}


def normalize_scheme_kind(value: str | None) -> str:
    """页面类型：用户在基础方案页还是普通方案页提问。"""
    v = (value or "").strip().lower()
    if v in ("base", "基础", "基础方案"):
        return "base"
    if v in ("scheme", "普通", "普通方案", "改造", "改造方案"):
        return "scheme"
    return v


def infer_scheme_kind(prompt_blocks: dict[str, Any] | None, *, scheme_kind: str = "") -> str:
    """从 scheme_kind 或 faq_id 前缀推断 base|scheme。"""
    sk = normalize_scheme_kind(scheme_kind or (prompt_blocks or {}).get("scheme_kind") or "")
    if sk in DEFAULT_BUSINESS_GOAL_BY_SCHEME_KIND:
        return sk
    fid = str((prompt_blocks or {}).get("faq_id") or "").strip()
    if fid.startswith("scheme_"):
        return "scheme"
    return "base"


def default_business_goal(*, scheme_kind: str = "", prompt_blocks: dict[str, Any] | None = None) -> str:
    sk = infer_scheme_kind(prompt_blocks, scheme_kind=scheme_kind)
    return DEFAULT_BUSINESS_GOAL_BY_SCHEME_KIND[sk]


def apply_legacy_prompt_defaults(
    prompt_blocks: dict[str, Any] | None,
    *,
    scheme_kind: str = "",
    base_case_id: int | None = None,
) -> dict[str, Any]:
    """未传 business_goal 时按页面类型填兼容默认值；显式传入则保留。"""
    pb = dict(prompt_blocks or {})
    raw = pb.get("business_goal")
    if raw is None or not str(raw).strip():
        pb["business_goal"] = default_business_goal(scheme_kind=scheme_kind, prompt_blocks=pb)
    if base_case_id is not None and int(base_case_id) > 0:
        pb["base_case_id"] = int(base_case_id)
    elif pb.get("base_case_id") in (0, "0"):
        pb.pop("base_case_id", None)
    return pb
