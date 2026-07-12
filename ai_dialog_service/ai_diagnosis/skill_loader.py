"""Load prompt skills and inject them into the AI diagnosis system prompt."""
from __future__ import annotations

import os
import re
from pathlib import Path

_PKG_ROOT = Path(__file__).resolve().parent.parent

_ANALYST_SECTION_TITLES = (
    "交通工程专业知识库",
    "改造难度等级定义",
    "行为准则",
)

_DEFAULT_ADVICE_SKILL = "prompts/ai-diagnosis-advice/SKILL.md"
_DEFAULT_ANALYST_SKILL = "prompts/traffic-planning-analyst/SKILL.md"
_DEFAULT_FAQ_SKILL = "prompts/ai-dialog-faq/SKILL.md"
_PT_TRANSIT_SKILL = "prompts/pt-transit-diagnosis/SKILL.md"


def _resolve_path(relative: str) -> Path:
    override = os.environ.get("TNA_SKILL_PATH", "").strip()
    if override and relative.endswith("traffic-planning-analyst/SKILL.md"):
        p = Path(override)
        if p.is_file():
            return p
    return _PKG_ROOT / relative.replace("/", os.sep)


def _strip_yaml_frontmatter(text: str) -> str:
    if text.startswith("---"):
        end = text.find("---", 3)
        if end != -1:
            return text[end + 3 :].lstrip("\n")
    return text


def load_skill_file(relative_path: str, *, max_chars: int | None = None) -> str:
    path = _resolve_path(relative_path)
    if not path.is_file():
        return ""
    try:
        text = _strip_yaml_frontmatter(path.read_text(encoding="utf-8"))
    except OSError:
        return ""
    text = text.strip()
    if max_chars and len(text) > max_chars:
        return text[:max_chars].rstrip() + "\n\n...（Skill 节选，完整版见 prompts/）"
    return text


def _extract_markdown_sections(text: str, titles: tuple[str, ...]) -> str:
    if not text:
        return ""
    parts = re.split(r"(?=^#{1,2} )", text, flags=re.MULTILINE)
    picked: list[str] = []
    for part in parts:
        part = part.strip()
        if not part:
            continue
        first_line = part.split("\n", 1)[0]
        if any(title in first_line for title in titles):
            picked.append(part)
    return "\n\n".join(picked)


def load_analyst_skill_excerpt() -> str:
    full = load_skill_file(_DEFAULT_ANALYST_SKILL)
    if not full:
        return ""
    excerpt = _extract_markdown_sections(full, _ANALYST_SECTION_TITLES)
    return excerpt or load_skill_file(_DEFAULT_ANALYST_SKILL, max_chars=3500)


def load_advice_skill() -> str:
    return load_skill_file(_DEFAULT_ADVICE_SKILL)


def load_faq_skill(*, max_chars: int = 2000) -> str:
    return load_skill_file(_DEFAULT_FAQ_SKILL, max_chars=max_chars)


def load_pt_transit_skill(*, max_chars: int = 4000) -> str:
    return load_skill_file(_PT_TRANSIT_SKILL, max_chars=max_chars)


def is_pt_transit_prompt(prompt_blocks: dict | None, *, scope: str | None = None) -> bool:
    pb = prompt_blocks or {}
    vals = [
        scope,
        pb.get("scope"),
        pb.get("scheme_type"),
        pb.get("type"),
        pb.get("type2"),
        pb.get("analysis_type"),
        pb.get("faq_id"),
        pb.get("preset_question_id"),
        pb.get("custom_question"),
        pb.get("business_goal"),
        pb.get("analysis_object"),
    ]
    text = " ".join(str(v or "") for v in vals).lower()
    return any(
        key in text
        for key in (
            "pt",
            "bus",
            "transit",
            "public transit",
            "公交",
            "公共交通",
            "公交分配",
            "公交诊断",
            "公交线路",
            "站间客流",
        )
    )


def build_skill_system_appendix(
    *,
    compact: bool = True,
    prompt_blocks: dict | None = None,
    scope: str | None = None,
) -> str:
    mode = os.environ.get("TNA_SKILL_MODE", "compact" if compact else "full").strip().lower()
    parts: list[str] = []

    advice = load_advice_skill()
    if advice:
        parts.append(f"# Skill: AI 诊断建议（ai-diagnosis-advice）\n\n{advice}")

    analyst = (
        load_skill_file(_DEFAULT_ANALYST_SKILL, max_chars=8000)
        if mode == "full"
        else load_analyst_skill_excerpt()
    )
    if analyst:
        parts.append(f"# Skill 节选：traffic-planning-analyst\n\n{analyst}")

    faq = load_faq_skill()
    if faq:
        parts.append(f"# Skill: AI 对话常见问题（ai-dialog-faq）\n\n{faq}")

    if is_pt_transit_prompt(prompt_blocks, scope=scope):
        pt_transit = load_pt_transit_skill()
        if pt_transit:
            parts.append(f"# Skill: 公交运行诊断（pt-transit-diagnosis）\n\n{pt_transit}")

    return "\n\n---\n\n".join(parts)


def skill_sources_loaded() -> dict[str, bool]:
    return {
        "ai_diagnosis_advice": _resolve_path(_DEFAULT_ADVICE_SKILL).is_file(),
        "traffic_planning_analyst": _resolve_path(_DEFAULT_ANALYST_SKILL).is_file(),
        "ai_dialog_faq": _resolve_path(_DEFAULT_FAQ_SKILL).is_file(),
        "pt_transit_diagnosis": _resolve_path(_PT_TRANSIT_SKILL).is_file(),
    }
