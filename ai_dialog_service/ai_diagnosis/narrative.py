"""根据数据与多级提示词生成 AI 章节（规则引擎；客户端可传入 ai_sections 覆盖）。"""
from __future__ import annotations

from typing import Any

from .gis_edit_log import merge_gis_operations
from .scope_utils import mode_active


def _pb(prompt_blocks: dict[str, Any], key: str, default: str = "") -> str:
    v = prompt_blocks.get(key, default)
    if isinstance(v, dict):
        return json_dumps_safe(v)
    return str(v).strip() if v else default


def _link_label(link: dict[str, Any], *, disambiguate: bool = False) -> str:
    lid = link.get("link_id", "")
    name = (link.get("name") or "").strip() or f"路段{lid}"
    if disambiguate or name in ("三亚", "路段", "") or name.startswith("路段"):
        return f"{name}·{lid}（link_id={lid}）"
    return f"{name}（link_id={lid}）"


def _link_labels(links: list[dict[str, Any]], n: int = 5) -> str:
    top = (links or [])[:n]
    names = [(x.get("name") or "").strip() for x in top]
    dup = len(set(names)) < len(names)
    return "；".join(_link_label(x, disambiguate=dup) for x in top) or "—"


def json_dumps_safe(obj) -> str:
    import json

    try:
        return json.dumps(obj, ensure_ascii=False)
    except Exception:
        return str(obj)


def generate_ai_sections(
    payload: dict[str, Any],
    prompt_blocks: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """
    当软件/大模型未提供 ai_sections 时，用数据库指标 + 用户提示词块生成默认可嵌入 HTML 的章节。
    所有结论尽量引用 payload 中的数值。
    """
    pb = prompt_blocks or {}
    meta = payload.get("meta") or {}
    scope = meta.get("scope", "all")
    motor = payload.get("motor") or {}
    slow = payload.get("slow") or {}
    pt = payload.get("pt") or {}
    diag = payload.get("diagnosis") or {}
    nb = payload.get("network_bundle") or {}
    od = (nb.get("od") or {}) if nb else {}
    gaps = payload.get("data_gaps") or []

    obj = _pb(pb, "analysis_object", "全网交通系统")
    goal = _pb(pb, "business_goal", "现状诊断")
    targets = _pb(pb, "planning_targets", "")
    constraints = _pb(pb, "constraints", "")
    notes = _pb(pb, "engineer_notes", "")

    parts = []
    if motor.get("ok") and mode_active(scope, "motor"):
        if motor.get("has_flow") or motor.get("avg_vc"):
            parts.append(
                f"机动车路网共 {motor['total_links']} 条路段，全网平均 V/C 为 {motor['avg_vc']}，"
                f"V/C≥0.8 的路段占比 {motor['congested_pct']}%。"
            )
        else:
            parts.append(
                f"机动车基础路网共 {motor['total_links']} 条路段、约 {motor.get('total_length_km', 0)} km"
                f"（尚无交通分配流量，仅结构统计）。"
            )
    if od.get("ok") and mode_active(scope, "motor"):
        parts.append(
            f"OD 矩阵 {od['pair_count']} 对，总需求 {od['total_demand']}；"
            f"Top 期望出行 {od['top_pairs'][0]['f_id']}→{od['top_pairs'][0]['t_id']} "
            f"（{od['top_pairs'][0]['demand']}）"
            if od.get("top_pairs")
            else ""
        )
    if slow.get("ok") and mode_active(scope, "slow"):
        parts.append(
            f"慢行网络规模 {slow['network_len_km']} km，平均断面流量 {slow['avg_flow']} 人次/h。"
        )
    if pt.get("ok") and mode_active(scope, "pt"):
        parts.append(
            f"公交运营线路 {pt['route_count']} 条、站点 {pt['stop_count']} 个，"
            f"平均断面客流 {pt['enroute_avg']} 人次/班。"
        )
    if diag.get("ok") and not parts:
        for row in (diag.get("rows") or [])[:5]:
            if row.get("num") is not None:
                parts.append(
                    f"{row.get('name') or row.get('code')}={row['num']}{row.get('unit') or ''}"
                )
    status_analysis = " ".join(parts) if parts else "⚠ 当前方案缺少可用的分配或路网数据，无法完成定量现状描述。"

    if notes:
        status_analysis += f" 结合工程师补充说明：{notes}"
    if targets:
        status_analysis += f" 对照规划目标：{targets}"

    problems: list[dict[str, Any]] = []
    recommendations: list[dict[str, Any]] = []

    if mode_active(scope, "motor") and motor.get("ok") and motor.get("congested_pct", 0) >= 15:
        top = motor.get("top_sat_links") or []
        link_ids = [str(x["link_id"]) for x in top[:5]]
        link_names = _link_labels(top, 5)
        problems.append(
            {
                "title": "机动车路网拥堵路段占比较高",
                "urgency": "高" if motor["congested_pct"] >= 25 else "中",
                "evidence": [
                    f"拥堵路段(V/C≥0.8)占比 {motor['congested_pct']}%",
                    f"平均 V/C {motor['avg_vc']}",
                    f"Top 饱和路段：{link_names}",
                ],
                "scope_link_ids": link_ids,
                "scope_link_labels": link_names,
                "root_cause": "高峰需求接近或超过路段通行能力，可能存在瓶颈路段或交通组织不合理。",
            }
        )
        recommendations.append(
            {
                "title": "优先治理 Top 饱和路段",
                "targets_problem": len(problems) - 1,
                "measures": [
                    f"对以下高饱和路段开展拓宽论证、平行分流或公交分担评估：{link_names}",
                ],
                "expected_effect": f"走廊平均 V/C 由 {motor['avg_vc']} 预计可降至 0.85 以下（需模型复算验证）",
                "difficulty": "II级",
                "duration": "1-3月",
            }
        )

    if mode_active(scope, "motor") and motor.get("ok"):
        for td in motor.get("type_data") or []:
            if td.get("avg_vc", 0) >= 0.9:
                problems.append(
                    {
                        "title": f"{td['label']}等级道路整体运行偏紧",
                        "urgency": "中",
                        "evidence": [f"{td['label']} 平均 V/C={td['avg_vc']}"],
                        "scope_link_ids": [],
                        "root_cause": "该等级道路承担过多过境或生成交通量。",
                    }
                )

    if mode_active(scope, "pt") and pt.get("ok") and pt.get("enroute_avg", 0) > 80:
        problems.append(
            {
                "title": "公交断面客流水平偏高",
                "urgency": "中",
                "evidence": [f"平均断面客流 {pt['enroute_avg']} 人次/班"],
                "scope_link_ids": [],
                "root_cause": "线路运力与高峰需求不匹配或班次不足。",
            }
        )
        recommendations.append(
            {
                "title": "提升公交高峰运力",
                "targets_problem": len(problems) - 1,
                "measures": ["高峰增发班次或启用大容量车型", "评估是否需要开辟快线"],
                "expected_effect": "高峰满载率下降 10-15 个百分点（需公交分配复算）",
                "difficulty": "I级",
                "duration": "1-4周",
            }
        )

    if not problems and mode_active(scope, "motor") and motor.get("ok") and motor.get("has_flow"):
        problems.append(
            {
                "title": "全网运行总体可控",
                "urgency": "低",
                "evidence": [f"平均 V/C {motor['avg_vc']}", f"拥堵占比 {motor['congested_pct']}%"],
                "scope_link_ids": [],
                "root_cause": "主要指标未达到严重拥堵阈值，宜持续监测高峰时段。",
            }
        )
    if not problems and mode_active(scope, "motor") and motor.get("ok") and not motor.get("has_flow"):
        problems.append(
            {
                "title": "基础路网已具备，尚未完成交通分配",
                "urgency": "中",
                "evidence": [
                    f"路网 {motor['total_links']} 条 / {motor.get('total_length_km', 0)} km",
                    "road_way 无 volume/v_c 或方案表缺失",
                ],
                "scope_link_ids": [],
                "root_cause": "需先执行 base_motor_network 分配后方能评估运行状态。",
            }
        )
        recommendations.append(
            {
                "title": "补全交通分配与宏观诊断",
                "targets_problem": 0,
                "measures": [
                    "执行机动车交通分配，写入方案 road_way 流量与 V/C",
                    "执行机动车宏观诊断，写入诊断指标结果表",
                ],
                "expected_effect": "报告可展示 V/C 分布、拥堵路段与诊断指标图表",
                "difficulty": "I级",
                "duration": "数分钟",
            }
        )

    # 诊断指标异常
    for row in (diag.get("rows") or [])[:8]:
        num = row.get("num")
        if num is None:
            continue
        code = row.get("code", "")
        if "vc" in code.lower() and num >= 0.9:
            problems.append(
                {
                    "title": f"诊断指标告警：{row.get('name', code)}",
                    "urgency": "中",
                    "evidence": [
                        f"{code} context={row.get('context')} value={num} {row.get('unit', '')}"
                    ],
                    "scope_link_ids": [],
                    "root_cause": "与宏观/中观诊断模块计算结果一致，建议结合 GIS 图层复核。",
                }
            )

    if constraints:
        for rec in recommendations:
            rec.setdefault("risks", [])
            rec["risks"].append(f"须满足约束：{constraints}")

    client_gis = (payload.get("gis_edit_log") or {}).get("operations") or []
    rule_gis_ops: list[dict[str, Any]] = []
    if mode_active(scope, "motor") and motor.get("top_sat_links"):
        top0 = motor["top_sat_links"][0]
        lid = top0["link_id"]
        rule_gis_ops.append(
            {
                "op_id": "REV-01",
                "target_id": str(lid),
                "action": "重点复核",
                "params": "V/C、流量、容量",
                "note": f"最高饱和路段：{top0.get('name') or lid}",
            }
        )
    gis_ops = merge_gis_operations(client_gis, rule_gis_ops)

    exec_parts = [f"分析对象：{obj}。", f"业务目标：{goal}。"]
    if mode_active(scope, "motor") and motor.get("ok"):
        if motor.get("has_flow"):
            exec_parts.append(
                f"机动车平均 V/C {motor['avg_vc']}，拥堵路段占比 {motor['congested_pct']}%。"
            )
        else:
            exec_parts.append(
                f"机动车基础路网 {motor['total_links']} 条（约 {motor.get('total_length_km', 0)} km），待分配后评估运行。"
            )
    if diag.get("ok"):
        exec_parts.append(f"已加载诊断指标 {len(diag.get('rows') or [])} 条。")
    if gaps:
        exec_parts.append("数据缺口：" + "；".join(gaps))

    if recommendations:
        advice_snippets: list[str] = []
        for rec in recommendations[:2]:
            title = str(rec.get("title") or "").strip()
            measures = rec.get("measures") or []
            if measures:
                advice_snippets.append(f"{title}：{measures[0]}")
            elif title:
                advice_snippets.append(title)
        if advice_snippets:
            exec_parts.append("建议：" + "；".join(advice_snippets) + "。")
    elif problems and mode_active(scope, "motor") and motor.get("ok"):
        top = motor.get("top_sat_links") or []
        if top:
            exec_parts.append(
                f"建议：优先关注高饱和路段 {_link_label(top[0])}，开展拓宽论证或平行分流评估。"
            )
        elif motor.get("has_flow"):
            exec_parts.append("建议：持续监测高峰时段运行，必要时对拥堵走廊开展容量提升论证。")
        else:
            exec_parts.append(
                "建议：先完成机动车交通分配与宏观诊断，再据此制定改善措施。"
            )

    return {
        "executive_summary": "".join(exec_parts),
        "status_analysis": status_analysis,
        "problems": problems,
        "recommendations": recommendations,
        "gis_operations": gis_ops,
        "prompt_echo": {
            "analysis_object": obj,
            "business_goal": goal,
            "planning_targets": targets,
            "constraints": constraints,
            "engineer_notes": notes,
        },
    }


def merge_ai_sections(
    generated: dict[str, Any],
    client_sections: dict[str, Any] | None,
) -> dict[str, Any]:
    """客户端 ai_sections 优先覆盖对应字段。"""
    if not client_sections:
        return generated
    out = dict(generated)
    for key in (
        "executive_summary",
        "status_analysis",
        "problems",
        "recommendations",
        "gis_operations",
    ):
        if key in client_sections and client_sections[key]:
            out[key] = client_sections[key]
    return out
