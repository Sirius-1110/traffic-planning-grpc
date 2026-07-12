"""AI 智能诊断 — 预设问题（来源：AI对话框-常见问题(2).docx）+ 自定义补充。"""
from __future__ import annotations

from typing import Any

# 与 docx 原文一致：question + expected_output
FAQ_ENTRIES: dict[str, dict[str, str]] = {
    "base_status_overview": {
        "category": "基础方案-分析",
        "question": "目前整体交通运行状况如何？",
        "expected_output": "输出整个交通饱和度，流量分布核心区域，OD分布，整体的碳排量等。",
        "task": "评价整体交通运行水平，须引用快照中的饱和度、流量、OD、碳排等可用数据；执行摘要末尾给出 1-2 句宏观/中观建议。",
    },
    "base_bottleneck_location": {
        "category": "基础方案-分析",
        "question": "拥挤/瓶颈路段位置？",
        "expected_output": "输出v/c占比高的区域的位置，并且GIS上实现定位。",
        "task": "列出 V/C 或饱和度偏高路段的 link_id 及位置描述，说明可在 GIS 定位；执行摘要给出针对瓶颈的简要治理方向。",
    },
    "base_improvement_advice": {
        "category": "基础方案-分析",
        "question": "修改建议是什么？",
        "expected_output": "调用AI输出修改建议的功能，基于基础方案进行分析并且给出修改意见。",
        "task": "基于基础方案数据给出可执行改善建议（措施、对象 ID、预期效果）。",
    },
    "slow_capacity_impact_overview": {
        "category": "基础方案-慢行容量影响",
        "question": "慢行通行能力受影响程度如何？",
        "expected_output": "读取 slow_macro_capacity_impact，说明受影响路段数、占比、平均容量下降和最大容量下降。",
        "task": "仅在 scheme_type=slow 时使用。基于 slow_macro_capacity_impact 诊断慢行通行能力受影响程度，并结合 slow_micro_capacity_impact_topn 概述高风险路段。不得泛泛讨论机动车拥堵。",
    },
    "slow_capacity_bottleneck": {
        "category": "基础方案-慢行容量影响",
        "question": "哪些慢行路段容量下降后风险更高？",
        "expected_output": "读取 slow_micro_capacity_impact_topn，列出容量下降明显且影响后 V/C 较高的路段。",
        "task": "基于 slow_micro_capacity_impact_topn 输出 link_id、容量下降、下降比例、分配流量和影响后 V/C，识别优先关注路段。",
    },
    "slow_capacity_high_risk_links": {
        "category": "基础方案-慢行容量影响",
        "question": "哪些慢行路段应优先关注和治理？",
        "expected_output": "结合容量影响 TOP-N 与慢行 V/C，给出优先治理路段和治理原因。",
        "task": "从 capacity_reduction、capacity_reduction_pct、adjusted_vc、flow 四类信息综合判断慢行高风险路段，并给出宏观/中观层面的治理方向。",
    },
    "scheme_vs_base": {
        "category": "普通方案-方案评审",
        "question": "这个方案与基础方案对比如何？",
        "expected_output": "对比基础方案的分析结果评价交通饱和度以及流量分布等内容。",
        "task": "对比基础方案与本方案：饱和度、流量分布、诊断指标变化，量化说明差异；执行摘要说明是否建议采纳或继续优化。",
    },
    "scheme_more_retrofit": {
        "category": "普通方案-方案评审",
        "question": "还能怎么改造？",
        "expected_output": "调用AI输出修改建议的功能，对目前普通方案进行分析并且给出修改意见。",
        "task": "在现有普通方案基础上提出进一步改造与优化建议。",
    },
    "scheme_raise_saturation": {
        "category": "普通方案-方案评审",
        "question": "我想提高交通饱和度",
        "expected_output": "调用AI输出修改建议的功能，针对提高交通饱和度提出建议。",
        "task": "针对提高交通运行饱和度/服务水平提出具体建议，引用现状数据。",
    },
    "scheme_raise_capacity": {
        "category": "普通方案-方案评审",
        "question": "我想提高承载力",
        "expected_output": "调用AI输出修改建议的功能，针对提高承载力提出意见。",
        "task": "针对提高道路/网络承载力提出工程与管理措施及预期效果。",
    },
    "scheme_status_overview": {
        "category": "普通方案-方案评审",
        "question": "当前的整体交通运行状况如何？",
        "expected_output": "输出整个交通饱和度，流量分布核心区域，OD分布，整体的碳排量等。",
        "task": "评价本方案整体运行状况：饱和度、流量分布、OD、碳排（若有数据）；执行摘要末尾给出 1-2 句宏观/中观建议。",
    },
    "slow_capacity_scheme_advice": {
        "category": "普通方案-慢行容量影响",
        "question": "慢行通行能力调整后，方案还应如何优化？",
        "expected_output": "基于 slow_macro_capacity_impact 与 slow_micro_capacity_impact_topn，提出慢行设施、断面和组织优化建议。",
        "task": "仅在 scheme_type=slow 时使用。围绕 capacity_slow_adj 后的慢行分配结果，识别容量下降显著且 adjusted_vc 偏高路段，提出可执行的慢行改善建议。",
    },
    "slow_capacity_before_after_compare": {
        "category": "普通方案-慢行容量影响",
        "question": "影响后慢行分配与基础方案相比有什么变化？",
        "expected_output": "对比基础方案与本方案慢行容量影响、V/C、TOP-N 风险路段变化。",
        "task": "优先使用基础方案对比数据；如缺少 base_case_id 或基础方案慢行容量指标，须明确说明无法量化对比，不得编造改善幅度。",
    },
    "slow_capacity_improvement_priority": {
        "category": "普通方案-慢行容量影响",
        "question": "哪些容量受影响路段应优先改造？",
        "expected_output": "给出优先改造 link_id、依据和建议措施。",
        "task": "按容量下降值、下降比例、影响后 V/C 和流量综合排序，输出优先改造路段及理由，建议应限于慢行设施供给、断面优化、连续性修复和交通组织层面。",
    },
}

FAQ_BY_CASE: dict[str, list[str]] = {
    "base": [
        "base_status_overview",
        "base_bottleneck_location",
        "base_improvement_advice",
        "slow_capacity_impact_overview",
        "slow_capacity_bottleneck",
        "slow_capacity_high_risk_links",
    ],
    "scheme": [
        "scheme_vs_base",
        "scheme_more_retrofit",
        "scheme_raise_saturation",
        "scheme_raise_capacity",
        "scheme_status_overview",
        "slow_capacity_scheme_advice",
        "slow_capacity_before_after_compare",
        "slow_capacity_improvement_priority",
    ],
}


def list_faq_options(case_id: int = 2, *, scheme_kind: str = "") -> list[dict[str, str]]:
    sk = (scheme_kind or "").strip().lower()
    if sk in ("base", "基础", "基础方案"):
        keys = FAQ_BY_CASE["base"]
    elif sk in ("scheme", "普通", "普通方案"):
        keys = FAQ_BY_CASE["scheme"]
    else:
        # 兼容旧调用：未传 scheme_kind 时 case_id==0 仍映射 base FAQ
        keys = FAQ_BY_CASE["base"] if case_id == 0 else FAQ_BY_CASE["scheme"]
    return [
        {
            "faq_id": k,
            "category": FAQ_ENTRIES[k]["category"],
            "question": FAQ_ENTRIES[k]["question"],
            "expected_output": FAQ_ENTRIES[k]["expected_output"],
            "custom_question": FAQ_ENTRIES[k]["question"],
        }
        for k in keys
    ]


def resolve_faq_id(prompt_blocks: dict[str, Any] | None, case_id: int) -> str | None:
    pb = prompt_blocks or {}
    fid = pb.get("faq_id") or pb.get("preset_question_id") or pb.get("common_question_id")
    if fid:
        return str(fid).strip()
    cq = (pb.get("custom_question") or pb.get("custom_input") or "").strip()
    if cq:
        for k, v in FAQ_ENTRIES.items():
            if v["question"] == cq or cq in v["question"] or v["question"] in cq:
                return k
    q = pb.get("preset_question") or pb.get("common_question")
    if q:
        for k, v in FAQ_ENTRIES.items():
            if v["question"] in str(q) or str(q) in v["question"]:
                return k
    return None


def build_faq_prompt_section(
    prompt_blocks: dict[str, Any] | None,
    *,
    case_id: int = 0,
) -> dict[str, Any]:
    pb = prompt_blocks or {}
    fid = resolve_faq_id(pb, case_id)
    custom = (pb.get("custom_question") or pb.get("custom_input") or "").strip()
    extra = (pb.get("engineer_notes") or "").strip()

    section: dict[str, Any] = {
        "faq_id": fid,
        "preset_question": "",
        "expected_output": "",
        "analysis_task": "",
        "custom_question": custom,
        "extra_notes": extra,
        "skill_hint": "",  # FAQ Skill 由 prompt_builder → skill_loader 统一注入 System
    }

    if fid and fid in FAQ_ENTRIES:
        ent = FAQ_ENTRIES[fid]
        section["preset_question"] = ent["question"]
        section["expected_output"] = ent["expected_output"]
        section["analysis_task"] = ent["task"]
        section["category"] = ent["category"]
        # docx 原文作为 custom_question（软件可把下拉选项原文传入）
        if not custom or custom == ent["question"]:
            section["custom_question"] = ent["question"]
    elif custom:
        section["preset_question"] = custom
        section["analysis_task"] = f"回答用户问题：{custom}"

    return section


def format_faq_for_user_prompt(section: dict[str, Any]) -> str:
    lines = ["[第3项-分析问题（AI对话框-常见问题 docx）]"]
    if section.get("faq_id"):
        lines.append(f"预设问题 ID：{section['faq_id']}")
    if section.get("category"):
        lines.append(f"分类：{section['category']}")
    if section.get("custom_question"):
        lines.append(f"用户问题（custom_question）：{section['custom_question']}")
    if section.get("expected_output"):
        lines.append(f"期望输出（docx）：{section['expected_output']}")
    if section.get("analysis_task"):
        lines.append(f"分析任务（Skill）：{section['analysis_task']}")
    if section.get("extra_notes"):
        lines.append(f"用户自定义补充：{section['extra_notes']}")
    elif not section.get("extra_notes"):
        lines.append("用户自定义补充：（无）")
    return "\n".join(lines)
