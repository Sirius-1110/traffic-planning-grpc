"""组装发给大模型的系统提示词与用户提示词（含多级提示词块 + 数据快照）。"""
from __future__ import annotations

import json
from typing import Any

from .faq_prompts import build_faq_prompt_section, format_faq_for_user_prompt
from .prompt_defaults import default_business_goal
from .scope_utils import mode_active, scope_label
from .skill_loader import (
    build_skill_system_appendix,
    skill_sources_loaded,
)
from .table_registry import TABLE_SPECS

# 核心 JSON 契约（详细质量约束见 prompts/ai-diagnosis-advice/SKILL.md + traffic-planning-analyst）
SYSTEM_PROMPT_CORE = """你是资深城市交通规划分析师，服务于交通规划管理软件。
你必须严格遵守下方「Skill 规范」中的输入解析、专业标准与输出质量约束。

【JSON 输出契约 — 不可违反】
输出必须是单个 JSON 对象（不要 Markdown 或代码块包裹），字段严格为：
{
  "executive_summary": "执行摘要，3-5句：直接回应用户问题 + 核心结论 + 数据缺口（若有）+ 给用户的简要建议（1-2句宏观/中观方向）",
  "status_analysis": "现状与目标差距，引用快照具体数字与单位",
  "problems": [
    {
      "title": "问题名称",
      "urgency": "高|中|低",
      "evidence": ["指标: 当前值 vs 阈值"],
      "scope_link_ids": ["路段或路口ID，须来自快照"],
      "root_cause": "根因分析"
    }
  ],
  "recommendations": [
    {
      "title": "方案名称",
      "targets_problem": 0,
      "measures": ["可执行工程措施，精确到ID和参数"],
      "expected_effect": "量化预期效果（当前值→预计值）",
      "difficulty": "I|II|III|IV|V 或 I-V级",
      "duration": "工期"
    }
  ],
  "gis_operations": [
    {"op_id": "REV-01", "target_id": "link_id", "action": "高亮显示", "params": "V/C、容量", "note": "复核饱和路段"}
  ]
}
全部使用简体中文。

【分析层级】宏观/中观交通规划（路网、分配、OD、公交、慢行）。禁止信号灯配时、绿波、相位、路口周期等微观信号控制建议。现状类 FAQ 的 recommendations 可为空，但 executive_summary 须含给用户的简要建议。"""


def build_prompts(
    payload: dict[str, Any],
    prompt_blocks: dict[str, Any] | None = None,
    *,
    compact: bool = True,
) -> dict[str, str]:
    pb = prompt_blocks or {}
    meta = payload.get("meta") or {}
    scope = meta.get("scope", "all")
    case_id = int(meta.get("case_id") or 0)
    spatial = meta.get("spatial_scope") or {}
    indicator_codes = meta.get("indicator_codes") or pb.get("indicator_codes")

    faq_section = build_faq_prompt_section(pb, case_id=case_id)
    faq_text = format_faq_for_user_prompt(faq_section)

    blocks_text = "\n".join(
        [
            "[第1项-框选范围]",
            str(spatial.get("label") or "全局范围（未框选）"),
            f"spatial_mode={spatial.get('mode', 'global')}",
            "",
            "[第2项-方案类型与诊断指标]",
            f"方案类型（scope）：{scope_label(scope)}（{scope}）",
            f"所选指标：{indicator_codes if indicator_codes else '（未限定，加载已计算指标节选）'}",
            "",
            faq_text,
            "",
            "[兼容-分析对象]",
            str(pb.get("analysis_object", "全网交通系统")),
            "",
            "[兼容-业务目标（L4，暂时保留）]",
            str(pb.get("business_goal") or default_business_goal(prompt_blocks=pb)),
            "",
            "[兼容-规划目标]",
            str(pb.get("planning_targets", "（未指定）")),
            "",
            "[兼容-约束条件]",
            str(pb.get("constraints", "（无）")),
        ]
    )

    # 附加原始 prompt_blocks JSON（软件可能传入更多层级字段）
    known = {
        "analysis_object", "business_goal", "planning_targets", "constraints", "engineer_notes",
        "faq_id", "preset_question_id", "custom_question", "custom_input",
        "indicator_codes", "selected_indicators", "spatial_scope", "bbox", "link_ids",
        "line_ids", "node_ids", "object_ids", "scope", "scheme_type",
    }
    extra_keys = {k: v for k, v in pb.items() if k not in known}
    if extra_keys:
        blocks_text += "\n\n[L0-软件扩展字段]\n" + json.dumps(extra_keys, ensure_ascii=False, indent=2)

    meta = payload.get("meta") or {}
    scope = meta.get("scope", "all")
    slabel = scope_label(scope)
    scope_rule = (
        f"【分析范围】本次仅限「{slabel}」。快照中未出现的交通方式一律不要分析、不要写问题、不要写建议。"
        if scope != "all"
        else "【分析范围】可对快照中出现的各交通方式分别分析，但须与数据一一对应。"
    )
    skill_appendix = build_skill_system_appendix(compact=compact, prompt_blocks=pb, scope=scope)
    system_parts = [SYSTEM_PROMPT_CORE, scope_rule]
    if skill_appendix:
        system_parts.append(
            "【Skill 规范 — 必须遵守】\n"
            "以下内容由 traffic-planning-analyst / ai-diagnosis-advice / ai-dialog-faq 加载：\n\n"
            + skill_appendix
        )
    system_prompt = "\n\n".join(system_parts)

    data_snapshot = _format_data_snapshot(payload, compact=compact)
    gaps = payload.get("data_gaps") or []
    gap_text = "\n".join(f"- {g}" for g in gaps) if gaps else "（无）"

    cmp_ = payload.get("base_scheme_comparison") or {}
    cmp_text = "（未提供基础方案对比）"
    if cmp_.get("ok"):
        cmp_text = json.dumps(cmp_, ensure_ascii=False, indent=2)

    user_prompt = f"""## 分析范围（软件指定，必须遵守）

仅限：{slabel}（scope={scope}）
框选：{spatial.get('label', '全局')}

## 用户输入（三项：框选 / 指标 / 问题）

{blocks_text}

## 基础方案对比（普通方案）

{cmp_text}

## 数据缺口

{gap_text}

## 当前数据快照（来自 PostgreSQL 交通模型，请作为唯一定量依据）

{data_snapshot}

请严格按 System 中的 Skill 规范与 JSON 契约：
1. 优先回答【第3项-分析问题】中的预设任务与 custom_question；
2. 引用快照具体数值，数据缺口处写 ⚠；
3. 仅输出单个 JSON 对象。"""

    return {
        "system_prompt": system_prompt,
        "user_prompt": user_prompt,
        "prompt_blocks_text": blocks_text,
        "full_user_prompt": user_prompt,
        "skill_sources": skill_sources_loaded(),
    }


def _table_categories_for_scope(scope: str) -> set[str]:
    if scope == "motor":
        return {"motor", "network", "diagnosis"}
    if scope == "slow":
        return {"slow", "diagnosis"}
    if scope == "pt":
        return {"pt", "diagnosis"}
    return {"motor", "slow", "pt", "trip", "diagnosis", "network"}


def _format_data_snapshot(payload: dict[str, Any], *, compact: bool = True) -> str:
    meta = payload.get("meta") or {}
    scope = meta.get("scope", "all")
    lines = [payload.get("kpi_summary_text", "")]
    top_n = 5 if compact else 8
    diag_n = 8 if compact else 20
    motor = payload.get("motor") or {}
    if motor.get("ok") and mode_active(scope, "motor"):
        lines.append(f"\n### 机动车\n- 路段数: {motor.get('total_links')}")
        lines.append(f"- 平均 V/C: {motor.get('avg_vc')}")
        lines.append(f"- 拥堵路段占比(V/C≥0.8): {motor.get('congested_pct')}%")
        lines.append(f"- V/C 分布 [畅通,基本,轻度,严重]: {motor.get('vc_dist')}")
        for td in motor.get("type_data") or []:
            line = f"  - {td['label']}: 平均V/C={td.get('avg_vc')}, 路段数={td.get('count')}"
            if td.get("note"):
                line += f"（{td['note']}）"
            lines.append(line)
        if any(td.get("is_centroid_connector") for td in (motor.get("type_data") or [])):
            lines.append(
                "  - 路网语义：质心连杆(type=10)为交通小区接入虚拟路段，不属于快速路/主干路/次干路/支路等级，"
                "不得提出「贯通 Type10」「修复 Type10 拓扑」类建议"
            )
        tops = motor.get("top_sat_links") or []
        if tops:
            lines.append("- Top 饱和路段:")
            for t in tops[:top_n]:
                lines.append(
                    "  - "
                    f"link_id={t['link_id']} 路名={t.get('name') or '未知'} "
                    f"等级={t.get('type_label') or '未标注'} 方向={t.get('direction') or '未标注'} "
                    f"V/C={t['v_c']} volume={t['volume']} capacity={t['capacity']}"
                )

    slow = payload.get("slow") or {}
    if slow.get("ok") and mode_active(scope, "slow"):
        slow_bits = (
            f"\n### 慢行\n- 路网 {slow.get('network_len_km')} km, 均流 {slow.get('avg_flow')} 人次/h, "
            f"高流量占比 {slow.get('high_flow_pct')}%"
        )
        if slow.get("area_percap") is not None:
            slow_bits += f", 人均慢行道路面 {slow.get('area_percap')} {slow.get('area_percap_unit', 'm²/人')}"
        lines.append(slow_bits)
        top_flows = slow.get("top_flow_links") or []
        if top_flows:
            lines.append("- 高流量慢行路段（节选）:")
            for t in top_flows[:top_n]:
                lines.append(f"  - link_id={t.get('link_id')} 路名={t.get('name')} flow={t.get('flow')} 人次/h")
        top_vcs = slow.get("top_vc_links") or []
        if top_vcs:
            lines.append("- 高 V/C 慢行路段（节选）:")
            for t in top_vcs[:top_n]:
                lines.append(f"  - link_id={t.get('link_id')} 路名={t.get('name')} V/C={t.get('v_c')} flow={t.get('volume')} capacity={t.get('capacity')}")

    pt = payload.get("pt") or {}
    if pt.get("ok") and mode_active(scope, "pt"):
        lines.append(
            f"\n### 公交\n- 线路 {pt.get('route_count')} 条, 站点 {pt.get('stop_count')} 个, "
            f"平均站间客流 {pt.get('enroute_avg')} 人次"
        )
        summary = pt.get("summary") or {}
        if summary:
            labels = [
                ("公交出行需求", "total_demand", "人次"),
                ("可服务需求", "assigned_demand", "人次"),
                ("暂不可服务需求", "infeasible_flow", "人次"),
                ("站间步行换乘连通数", "num_of_walk_links", "条"),
                ("平均出行时间成本", "avg_path_cost", "分钟"),
                ("平均等待时间", "avg_wait_cost", "分钟"),
            ]
            brief = "，".join(
                f"{label}{summary[key]}{unit}"
                for label, key, unit in labels
                if summary.get(key) is not None
            )
            if brief:
                lines.append(f"- 公交分配结果: {brief}")
            total_demand = summary.get("total_demand")
            infeasible_flow = summary.get("infeasible_flow")
            if total_demand and infeasible_flow is not None:
                ratio = round(float(infeasible_flow) / max(float(total_demand), 1.0) * 100, 2)
                lines.append(
                    f"- 不可达需求比例: {ratio}%（请解释为公交网络连通性、站点匹配、步行换乘或 OD 数据问题，不要直接写算法失败）"
                )
        route_flow_top = pt.get("route_flow_top") or []
        if route_flow_top:
            lines.append("- 重点线路客流强度:")
            for t in route_flow_top[:top_n]:
                lines.append(
                    f"  - {t.get('route_id')}: 线路客流强度 {t.get('total_flow')}人次，"
                    f"最大断面客流 {t.get('max_flow')}人次"
                )
        top_vc_links = pt.get("top_vc_links") or []
        if top_vc_links:
            lines.append("- 高负荷站间:")
            for t in top_vc_links[:top_n]:
                from_name = t.get("from_stop_name") or t.get("from_stop")
                to_name = t.get("to_stop_name") or t.get("to_stop")
                lines.append(
                    f"  - {from_name}→{to_name}: 断面客流 {t.get('flow')}人次，"
                    f"饱和度 {t.get('vc_ratio')}，涉及线路 {t.get('route_count')} 条"
                )
        vc_distribution = pt.get("vc_distribution") or []
        if vc_distribution:
            lines.append(
                "- 站间饱和度分布: "
                + ", ".join(f"{x.get('bucket')}={x.get('link_count')}条" for x in vc_distribution)
            )
        path_summary = pt.get("path_summary") or {}
        if path_summary:
            preferred = {
                "avg_path_cost": "平均出行时间成本",
                "avg_wait_cost": "平均等待时间",
                "avg_num_links": "平均出行环节数",
                "path_count": "公交出行路径记录数",
            }
            lines.append(
                "- 出行体验摘要: "
                + "，".join(
                    f"{preferred[k]}={v}"
                    for k, v in path_summary.items()
                    if k in preferred
                )
            )

    diag = payload.get("diagnosis") or {}
    if diag.get("rows"):
        lines.append("\n### 诊断指标（节选）")
        for r in diag["rows"][:diag_n]:
            val = r.get("num")
            display = r.get("name") or r.get("code")
            if val is not None:
                lines.append(
                    f"- {display} | ctx={r.get('context')} | "
                    f"{val} {r.get('unit', '')}"
                )
            elif r.get("text"):
                lines.append(f"- {display} | {r.get('text')}")

    tables = payload.get("tables") or {}
    if tables.get("ok") and not compact:
        reg = tables.get("registered") or {}
        cats = _table_categories_for_scope(scope)
        suffix_cat = {s["suffix"]: s["category"] for s in TABLE_SPECS}
        lines.append("\n### 数据表清单（与本分析范围相关）")
        shown = 0
        for suffix, info in reg.items():
            if suffix_cat.get(suffix) not in cats:
                continue
            shown += 1
            if shown > 35:
                break
            if not info.get("exists"):
                lines.append(f"- {suffix} ({info.get('label','')}): 不存在")
                continue
            rc = info.get("row_count", 0)
            sm = info.get("summary") or {}
            sm_brief = ", ".join(f"{k}={v}" for k, v in list(sm.items())[:4]) if sm else ""
            lines.append(f"- {suffix}: {rc} 行" + (f" | {sm_brief}" if sm_brief else ""))
    return "\n".join(lines)
