"""分析范围（机动车 / 慢行 / 公交）解析与过滤。"""
from __future__ import annotations

from typing import Any

SCOPE_LABELS = {
    "motor": "机动车",
    "slow": "慢行",
    "pt": "公交",
    "all": "综合（机动车+慢行+公交）",
}

_SCOPE_ALIASES = {
    "motor": "motor",
    "机动车": "motor",
    "汽车": "motor",
    "road": "motor",
    "1": "motor",
    "slow": "slow",
    "慢行": "slow",
    "非机动车": "slow",
    "walk": "slow",
    "2": "slow",
    "pt": "pt",
    "公交": "pt",
    "公共交通": "pt",
    "轨道": "pt",
    "transit": "pt",
    "bus": "pt",
    "3": "pt",
    "all": "all",
    "全部": "all",
    "综合": "all",
    "全网": "all",
}


def normalize_scope(value: str | None) -> str:
    if not value:
        return "all"
    key = str(value).strip().lower()
    if key in _SCOPE_ALIASES:
        return _SCOPE_ALIASES[key]
    # 中文原文
    raw = str(value).strip()
    if raw in _SCOPE_ALIASES:
        return _SCOPE_ALIASES[raw]
    return "all"


def resolve_scheme_type(
    scheme_type: str | None = None,
    *,
    legacy_scope: str | None = None,
    prompt_blocks: dict[str, Any] | None = None,
    default: str = "motor",
) -> str:
    """解析分析哪种交通（机动车/慢行/公交）。正式字段为 scheme_type；scope 仅兼容旧客户端。"""
    pb = prompt_blocks or {}
    for key in ("scheme_type", "scope", "analysis_scope", "transport_mode", "mode", "diagnosis_scope", "traffic_mode"):
        val = pb.get(key)
        if val is not None and str(val).strip():
            out = normalize_scope(str(val))
            return default if out == "all" and default != "all" else out
    st = (scheme_type or "").strip()
    if st:
        out = normalize_scope(st)
        return default if out == "all" and default != "all" else out
    ls = (legacy_scope or "").strip()
    if ls:
        out = normalize_scope(ls)
        return default if out == "all" and default != "all" else out
    out = normalize_scope(default)
    return default if out == "all" and default != "all" else out


def resolve_scope(scope: str | None, prompt_blocks: dict[str, Any] | None = None) -> str:
    """读库/拼提示词内部分析范围（motor/slow/pt）。优先 scheme_type。"""
    return resolve_scheme_type(scope, legacy_scope=scope, prompt_blocks=prompt_blocks, default="motor")


def scope_label(scope: str) -> str:
    return SCOPE_LABELS.get(scope, scope)


def mode_active(scope: str, mode: str) -> bool:
    return scope == "all" or scope == mode


def diagnosis_row_matches_scope(code: str, scope: str) -> bool:
    if scope == "all":
        return True
    c = (code or "").lower()
    if scope == "pt":
        return c.startswith("pt_")
    if scope == "slow":
        return c.startswith("slow_") or "_slow_" in c
    # motor：排除公交/慢行专用指标
    if c.startswith("pt_") or c.startswith("slow_"):
        return False
    return True


def filter_diagnosis_rows(rows: list[dict[str, Any]], scope: str) -> list[dict[str, Any]]:
    if scope == "all":
        return rows
    return [r for r in rows if diagnosis_row_matches_scope(r.get("code", ""), scope)]


def normalize_ai_sections(sections: dict[str, Any] | None) -> dict[str, Any]:
    """大模型 JSON → 报告章节（空字段用空值，不用规则引擎填充）。"""
    s = sections or {}
    return {
        "executive_summary": str(s.get("executive_summary") or ""),
        "status_analysis": str(s.get("status_analysis") or ""),
        "problems": list(s.get("problems") or []),
        "recommendations": list(s.get("recommendations") or []),
        "gis_operations": list(s.get("gis_operations") or []),
    }
