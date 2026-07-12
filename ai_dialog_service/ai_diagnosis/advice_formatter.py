"""将 AI JSON 章节格式化为可读建议文本（无 HTML 报告）。"""
from __future__ import annotations

import json
import re
from typing import Any


def _clean_pt_text(text: str) -> str:
    replacements = {
        "AON": "公交分配",
        "超路径": "公交出行路径",
        "V/C": "饱和度",
        "v/c": "饱和度",
        "link_id": "站间编号",
        "shape_id": "线路方向编号",
        "pt_macro_": "",
        "pt_meso_": "",
        "flow": "客流",
        "capacity": "工程容量",
        "数据库表": "数据结果",
        "算法内部": "计算过程",
    }
    out = text
    for old, new in replacements.items():
        out = out.replace(old, new)
    out = re.sub(r"\bshape\b", "线路方向", out, flags=re.IGNORECASE)
    return out


def _append_text(lines: list[str], text: Any, *, pt: bool) -> None:
    s = str(text)
    lines.append(_clean_pt_text(s) if pt else s)


def format_advice_text(sections: dict[str, Any], *, scope: str = "") -> str:
    is_pt = scope == "pt"
    lines: list[str] = []
    if sections.get("executive_summary"):
        lines.append("## 总体判断" if is_pt else "## 执行摘要")
        _append_text(lines, sections["executive_summary"], pt=is_pt)
        lines.append("")

    analysis = sections.get("status_analysis") or sections.get("gap_analysis") or ""
    if analysis:
        lines.append("## 主要问题" if is_pt else "## 现状分析")
        _append_text(lines, analysis, pt=is_pt)
        lines.append("")

    probs = sections.get("problems") or []
    if probs:
        lines.append("## 重点区域/线路" if is_pt else "## 问题诊断")
        for i, p in enumerate(probs, 1):
            if not isinstance(p, dict):
                continue
            title = _clean_pt_text(str(p.get('title', '问题'))) if is_pt else p.get('title', '问题')
            urgency = _clean_pt_text(str(p.get('urgency', ''))) if is_pt else p.get('urgency', '')
            lines.append(f"{i}. **{title}**（{urgency}）")
            for ev in p.get("evidence") or []:
                _append_text(lines, f"   - {ev}", pt=is_pt)
            if p.get("root_cause"):
                _append_text(lines, f"   - 根因：{p['root_cause']}", pt=is_pt)
        lines.append("")

    recs = sections.get("recommendations") or sections.get("scheme_recommendations") or []
    if recs:
        lines.append("## 优先治理建议" if is_pt else "## AI 建议")
        for i, r in enumerate(recs, 1):
            if not isinstance(r, dict):
                continue
            title = _clean_pt_text(str(r.get('title', '建议'))) if is_pt else r.get('title', '建议')
            lines.append(f"{i}. **{title}**")
            for m in r.get("measures") or []:
                _append_text(lines, f"   - {m}", pt=is_pt)
            if r.get("expected_effect"):
                _append_text(lines, f"   - 预期：{r['expected_effect']}", pt=is_pt)
            if r.get("expected_indicators"):
                text = f"   - 指标：{json.dumps(r['expected_indicators'], ensure_ascii=False)}"
                _append_text(lines, text, pt=is_pt)
        lines.append("")

    gis = sections.get("gis_operations") or []
    if gis and not is_pt:
        lines.append("## GIS 操作建议")
        for g in gis:
            if isinstance(g, dict):
                lines.append(
                    f"- [{g.get('op_id')}] {g.get('target_id')}: {g.get('action')} "
                    f"({g.get('params')}) {g.get('note', '')}"
                )
        lines.append("")

    fallback = json.dumps(sections, ensure_ascii=False, indent=2)
    return "\n".join(lines).strip() or (_clean_pt_text(fallback) if is_pt else fallback)
