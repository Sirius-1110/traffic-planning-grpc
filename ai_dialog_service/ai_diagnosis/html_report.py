"""将 payload + ai_sections 组装为图文并茂的 ECharts HTML 报告。"""
from __future__ import annotations

import html
import json
import os
from datetime import datetime

from typing import Any

from .llm_client import _extract_json_object
from .scope_utils import mode_active, normalize_ai_sections, scope_label

_CSS = """
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:"Times New Roman","宋体",serif;background:#f0f4f8;color:#1a1a2e;font-size:14px;line-height:1.75}
.hdr{background:linear-gradient(120deg,#0d2137,#1b4a82 55%,#2471a3);color:#fff;padding:24px 40px 20px}
.hdr h1{font-size:20px;font-weight:700}
.hdr .sub{font-size:12px;opacity:.85;margin-top:6px}
.nav{background:#1b4a82;display:flex;flex-wrap:wrap;gap:2px;padding:0 24px;position:sticky;top:0;z-index:50}
.nav a{color:#c8dff5;text-decoration:none;padding:10px 16px;font-size:12px}
.nav a:hover{color:#fff}
.wrap{max-width:1280px;margin:20px auto;padding:0 20px 40px}
.card{background:#fff;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.08);padding:22px 26px;margin-bottom:18px}
.sec-title{font-size:15px;font-weight:700;color:#1b4a82;border-left:4px solid #2471a3;padding-left:10px;margin-bottom:14px}
.badge-row{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:14px}
.badge{flex:1;min-width:130px;background:#eaf2fb;border-radius:8px;padding:12px 14px;text-align:center;border-left:4px solid #2471a3}
.badge .lbl{font-size:11px;color:#5d6d7e}
.badge .val{font-size:20px;font-weight:700;color:#1b4a82}
.badge.warn{border-color:#e67e22;background:#fef5e7}
.badge.bad{border-color:#e74c3c;background:#fdedec}
.chart-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:14px}
.chart-box{height:300px;border:1px solid #eaecef;border-radius:6px;background:#fafbfc}
.chart-box.wide{grid-column:1/-1;height:340px}
.prose{font-size:14px;color:#333;text-align:justify}
.prose p{margin-bottom:10px}
.prompt-box{background:#f8f9fa;border-left:4px solid #8e44ad;padding:12px 16px;margin-bottom:12px;font-size:13px}
.problem{margin-bottom:14px;padding:12px 14px;border-radius:6px;background:#fff8f0;border-left:4px solid #e67e22}
.problem.high{border-color:#e74c3c;background:#fdedec}
.rec{margin-bottom:12px;padding:12px 14px;border-radius:6px;background:#eafaf1;border-left:4px solid #27ae60}
table{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px}
th{background:#1b4a82;color:#fff;padding:8px 10px}
td{padding:7px 10px;border-bottom:1px solid #eaecef;text-align:center}
.gap{color:#c0392b;font-size:13px;margin:8px 0}
.source-tag{display:inline-block;font-size:11px;padding:2px 8px;border-radius:4px;margin-left:8px}
.source-tag.llm{background:#d5f5e3;color:#1e8449}
.source-tag.rule{background:#fdebd0;color:#b7950b}
.source-tag.client{background:#d6eaf8;color:#1f618d}
pre.preblock{background:#1e272e;color:#ecf0f1;padding:14px 16px;border-radius:6px;font-size:12px;line-height:1.55;overflow:auto;max-height:420px;white-space:pre-wrap;word-break:break-word}
details{margin-bottom:12px;border:1px solid #dce4ec;border-radius:6px;padding:8px 12px}
details summary{cursor:pointer;font-weight:600;color:#1b4a82;font-size:13px}
.meta-table td{text-align:left}
.meta-table td:first-child{font-weight:600;width:140px;color:#555;background:#f8f9fa}
footer{text-align:center;font-size:11px;color:#999;padding:24px 0}
.map-box{height:480px;width:100%;border:1px solid #dce4ec;border-radius:8px;z-index:1}
.map-stack{display:flex;flex-direction:column;gap:20px}
.map-subtitle{font-size:13px;font-weight:600;color:#1b4a82;margin:0 0 8px}
.map-legend{font-size:12px;color:#555;margin:8px 0 12px}
.map-legend span{display:inline-block;margin-right:14px}
.map-legend i{display:inline-block;width:22px;height:4px;vertical-align:middle;margin-right:4px}
.map-legend .od-w-thin{display:inline-block;width:28px;height:2px;background:#c0392b;vertical-align:middle;margin:0 4px}
.map-legend .od-w-mid{display:inline-block;width:28px;height:6px;background:#c0392b;vertical-align:middle;margin:0 4px}
.map-legend .od-w-thick{display:inline-block;width:28px;height:11px;background:#c0392b;vertical-align:middle;margin:0 4px}
.map-legend .zone-fill{display:inline-block;width:14px;height:14px;background:rgba(52,152,219,0.25);border:1px solid #2980b9;vertical-align:middle;margin:0 4px}
.road-name-label,.od-pair-label,.zone-id-label{background:none!important;border:none!important}
.road-name-label span,.od-pair-label span,.zone-id-label span{
  display:inline-block;padding:1px 4px;border-radius:3px;font-size:10px;font-weight:600;line-height:1.2;
  white-space:nowrap;pointer-events:none;text-shadow:-1px -1px 2px #fff,1px -1px 2px #fff,-1px 1px 2px #fff,1px 1px 2px #fff}
.road-name-label span{color:#1a5276}
.od-pair-label span{color:#922b21;background:rgba(255,255,255,0.85)}
.zone-id-label span{color:#1f618d;background:rgba(255,255,255,0.8)}
.leaflet-od-legend{padding:8px 10px;background:rgba(255,255,255,0.92);border:1px solid #ccc;border-radius:6px;font-size:11px;line-height:1.6;box-shadow:0 1px 4px rgba(0,0,0,0.12)}
details.map-fold{margin-bottom:14px;border:1px solid #dce4ec;border-radius:8px;padding:10px 14px;background:#fafbfc}
details.map-fold>summary{cursor:pointer;font-size:14px;font-weight:600;color:#1b4a82;list-style-position:outside}
details.map-fold[open]>summary{margin-bottom:10px}
.map-empty{font-size:12px;color:#856404;line-height:1.8;padding:8px 4px}
.map-fold-body{padding-top:4px}
"""


def _echarts_script() -> str:
    for path in (
        "/tmp/echarts.min.js",
        os.path.join(os.path.dirname(__file__), "..", "docs", "echarts.min.js"),
    ):
        try:
            with open(path, encoding="utf-8") as f:
                return "<script>" + f.read() + "</script>"
        except OSError:
            continue
    return '<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>'


def _js_pie(chart_id: str, title: str, labels: list, values: list) -> str:
    return f"""(function(){{
var c=echarts.init(document.getElementById('{chart_id}'));
c.setOption({{
  title:{{text:{json.dumps(title)},left:'center',textStyle:{{fontSize:13,color:'#1b4a82'}}}},
  tooltip:{{trigger:'item'}},
  legend:{{bottom:4,textStyle:{{fontSize:10}}}},
  series:[{{type:'pie',radius:['38%','62%'],data:{json.dumps([{"name": l, "value": v} for l, v in zip(labels, values)])},
    label:{{fontSize:11}},emphasis:{{itemStyle:{{shadowBlur:8}}}}}}]
}});
window.addEventListener('resize',function(){{c.resize()}});
}})();"""


def _js_bar(chart_id: str, title: str, labels: list, values: list, yname: str = "") -> str:
    return f"""(function(){{
var c=echarts.init(document.getElementById('{chart_id}'));
c.setOption({{
  title:{{text:{json.dumps(title)},left:'center',textStyle:{{fontSize:13,color:'#1b4a82'}}}},
  tooltip:{{trigger:'axis'}},
  grid:{{top:48,bottom:40,left:50,right:20,containLabel:true}},
  xAxis:{{type:'category',data:{json.dumps(labels)},axisLabel:{{fontSize:10,rotate:18}}}},
  yAxis:{{type:'value',name:{json.dumps(yname)},axisLabel:{{fontSize:10}}}},
  series:[{{type:'bar',data:{json.dumps(values)},itemStyle:{{color:'#2980b9'}},
    barMaxWidth:40,label:{{show:true,position:'top',fontSize:9}}}}]
}});
window.addEventListener('resize',function(){{c.resize()}});
}})();"""


def _badge(label: str, val, unit: str = "", cls: str = "") -> str:
    return f'<div class="badge {cls}"><div class="lbl">{html.escape(label)}</div><div class="val">{html.escape(str(val))}<span style="font-size:11px;color:#888"> {html.escape(unit)}</span></div></div>'


def _render_problems(problems: list[dict]) -> str:
    if not problems:
        return '<p class="prose">暂无结构化问题条目。</p>'
    out = []
    for p in problems:
        urg = p.get("urgency", "中")
        cls = "high" if urg == "高" else ""
        ev = "; ".join(p.get("evidence") or [])
        links = p.get("scope_link_labels") or ", ".join(
            str(x) for x in (p.get("scope_link_ids") or [])
        ) or "—"
        out.append(
            f'<div class="problem {cls}"><strong>{html.escape(p.get("title",""))}</strong>'
            f' <span style="color:#888">| 紧迫度：{html.escape(urg)}</span>'
            f'<p><strong>数据依据：</strong>{html.escape(ev)}</p>'
            f'<p><strong>影响路段：</strong>{html.escape(links)}</p>'
            f'<p><strong>根因：</strong>{html.escape(p.get("root_cause",""))}</p></div>'
        )
    return "\n".join(out)


def _render_recommendations(recs: list[dict]) -> str:
    if not recs:
        return '<p class="prose">暂无改善建议；可在软件中由 AI 生成后通过 ai_sections 传入。</p>'
    out = []
    for i, r in enumerate(recs, 1):
        measures = r.get("measures") or []
        ml = "".join(f"<li>{html.escape(m)}</li>" for m in measures)
        out.append(
            f'<div class="rec"><strong>方案{i}：{html.escape(r.get("title",""))}</strong>'
            f'<p>针对问题 #{r.get("targets_problem","")+1 if isinstance(r.get("targets_problem"), int) else ""} | '
            f'难度 {html.escape(r.get("difficulty",""))} | 工期 {html.escape(r.get("duration",""))}</p>'
            f"<ul>{ml}</ul>"
            f'<p><strong>预期效果：</strong>{html.escape(r.get("expected_effect",""))}</p></div>'
        )
    return "\n".join(out)


def _source_badge(llm_trace: dict[str, Any]) -> str:
    src = llm_trace.get("source", "unknown")
    labels = {
        "llm": ("大模型生成", "llm"),
        "rule_fallback": ("规则引擎占位（未配置 API Key）", "rule"),
        "rule_only": ("规则引擎（未启用 AI）", "rule"),
        "client_provided": ("调用方已提供 AI 章节", "client"),
        "llm_failed": ("大模型调用失败", "rule"),
    }
    text, cls = labels.get(src, (src, "rule"))
    return f'<span class="source-tag {cls}">{html.escape(text)}</span>'


def _render_prompt_section(report_ctx: dict[str, Any]) -> str:
    pb = report_ctx.get("prompt_blocks") or {}
    prompts = report_ctx.get("prompts") or {}
    rows = ""
    for label, key in (
        ("L1 分析对象", "analysis_object"),
        ("L2 业务目标", "business_goal"),
        ("L3 规划目标", "planning_targets"),
        ("L4 约束条件", "constraints"),
        ("L5 工程师补充", "engineer_notes"),
    ):
        val = pb.get(key, "")
        if val:
            rows += f"<tr><td>{html.escape(label)}</td><td>{html.escape(str(val))}</td></tr>"
    extra = {k: v for k, v in pb.items() if k not in (
        "analysis_object", "business_goal", "planning_targets", "constraints", "engineer_notes"
    )}
    if extra:
        rows += f"<tr><td>扩展字段</td><td><pre class='preblock'>{html.escape(json.dumps(extra, ensure_ascii=False, indent=2))}</pre></td></tr>"

    sys_p = prompts.get("system_prompt", "")
    usr_p = prompts.get("user_prompt", "") or prompts.get("full_user_prompt", "")

    return f"""
    <p class="prose">以下为软件多级提示词及实际发送给大模型的完整提示（API Key 不在此展示）。</p>
    <table class="meta-table"><tbody>{rows or '<tr><td colspan="2">（未传入 prompt_blocks）</td></tr>'}</tbody></table>
    <details open><summary>系统提示词（System）</summary><pre class="preblock">{html.escape(sys_p)}</pre></details>
    <details open><summary>用户提示词（User，含数据快照）</summary><pre class="preblock">{html.escape(usr_p)}</pre></details>
    """


def _render_llm_trace_section(llm_trace: dict[str, Any]) -> str:
    if not llm_trace:
        return ""
    model = llm_trace.get("model", "—")
    base = llm_trace.get("base_url", "—")
    usage = llm_trace.get("usage") or {}
    usage_s = html.escape(json.dumps(usage, ensure_ascii=False)) if usage else "—"
    err = llm_trace.get("error", "")
    raw = llm_trace.get("raw_response", "")
    err_html = f'<p class="gap">{html.escape(err)}</p>' if err else ""
    raw_html = ""
    if raw:
        raw_html = (
            '<p class="prose" style="font-size:12px;color:#555">'
            "下方「执行摘要 / 现状分析 / 问题诊断 / 改善建议」均由此 JSON 解析渲染，与规则引擎无关。"
            "</p>"
            f'<details open><summary>大模型原始 JSON 回复</summary><pre class="preblock">{html.escape(raw)}</pre></details>'
        )
    elif llm_trace.get("source") == "rule_fallback":
        raw_html = "<p class=\"prose\">未调用大模型，无原始回复。</p>"

    return f"""
    <table class="meta-table">
      <tr><td>叙事来源</td><td>{_source_badge(llm_trace)}</td></tr>
      <tr><td>API 端点</td><td>{html.escape(str(base))}</td></tr>
      <tr><td>模型</td><td>{html.escape(str(model))}</td></tr>
      <tr><td>Token 用量</td><td>{usage_s}</td></tr>
    </table>
    {err_html}
    {raw_html}
    """


from .network_map_report import leaflet_head_html as _leaflet_head, render_map_sections


def _vc_color(vc: float | None) -> str:
    if vc is None:
        return "#7f8c8d"
    if vc >= 1.0:
        return "#c0392b"
    if vc >= 0.8:
        return "#e67e22"
    if vc >= 0.6:
        return "#f1c40f"
    return "#27ae60"


def _render_network_overview(nb: dict[str, Any]) -> tuple[str, list[str]]:
    """基础路网统计（机动车/慢行/公交）+ 结构图。"""
    if not nb:
        return "", []
    motor = nb.get("motor") or {}
    slow = nb.get("slow") or {}
    pt = nb.get("pt") or {}
    od = nb.get("od") or {}
    badges = ""
    charts = ""
    chart_js: list[str] = []
    if motor.get("ok"):
        badges += _badge("机动车路段", motor.get("total_links", 0), "条")
        badges += _badge("机动车里程", motor.get("total_length_km", 0), "km")
        if motor.get("type_data"):
            charts += '<div class="chart-box" id="chart_net_motor"></div>'
            td = motor["type_data"]
            chart_js.append(
                _js_bar(
                    "chart_net_motor",
                    "机动车路网结构（分等级路段数）",
                    [d["label"] for d in td],
                    [d["count"] for d in td],
                    "条",
                )
            )
    if slow.get("ok"):
        badges += _badge("慢行路段", slow.get("total_links", 0), "条")
        badges += _badge("慢行里程", slow.get("total_length_km", 0), "km")
    if pt.get("ok"):
        badges += _badge("公交线路", pt.get("route_count", 0), "条")
        badges += _badge("公交站点", pt.get("stop_count", 0), "个")
    if od.get("ok"):
        badges += _badge("OD 对数", od.get("pair_count", 0), "对")
        badges += _badge("OD 总需求", od.get("total_demand", 0), "")
        pairs = od.get("top_pairs") or []
        if pairs:
            charts += '<div class="chart-box wide" id="chart_od_top"></div>'
            chart_js.append(
                _js_bar(
                    "chart_od_top",
                    "Top OD 出行需求",
                    [f"{p['f_id']}→{p['t_id']}" for p in pairs[:12]],
                    [p["demand"] for p in pairs[:12]],
                    "需求",
                )
            )
    note = ""
    if nb.get("used_network_fallback"):
        note = '<p class="prose" style="color:#856404">方案表无路网时，已回退读取工具前缀基础路网用于地图与结构统计。</p>'
    html_block = ""
    if badges or charts:
        html_block = f"""
    {note}
    <div class="badge-row">{badges}</div>
    <div class="chart-grid">{charts}</div>
"""
    return html_block, chart_js


def _render_map_section(nb: dict[str, Any], *, scope: str = "all") -> tuple[str, str]:
    map_mode = "slow" if mode_active(scope, "slow") and not mode_active(scope, "motor") else "motor"
    rendered = render_map_sections(nb, map_mode=map_mode)
    html_block = rendered["html"].replace('id="s_maps" class="section"', 'class="map-stack"')
    return html_block, rendered["js"]


def _render_gis_table(ops: list[dict]) -> str:
    if not ops:
        return ""
    rows = "".join(
        f"<tr><td>{html.escape(o.get('op_id',''))}</td>"
        f"<td>{html.escape(o.get('target_id',''))}</td>"
        f"<td>{html.escape(o.get('action',''))}</td>"
        f"<td>{html.escape(o.get('params',''))}</td>"
        f"<td>{html.escape(o.get('note',''))}</td></tr>"
        for o in ops
    )
    return (
        "<table><tr><th>编号</th><th>对象ID</th><th>操作</th><th>参数</th><th>说明</th></tr>"
        + rows
        + "</table>"
    )


def render_html(
    payload: dict[str, Any],
    ai_sections: dict[str, Any],
    report_ctx: dict[str, Any] | None = None,
) -> str:
    meta = payload.get("meta") or {}
    scope = meta.get("scope", "all")
    motor = payload.get("motor") or {}
    slow = payload.get("slow") or {}
    pt = payload.get("pt") or {}
    diag = payload.get("diagnosis") or {}
    network_bundle = payload.get("network_bundle") or {}
    gaps = payload.get("data_gaps") or []
    report_ctx = report_ctx or {}
    llm_trace = report_ctx.get("llm_trace") or {}
    # 大模型成功时：章节与「原始 JSON」同源，避免与规则引擎残留不一致
    if llm_trace.get("source") == "llm" and llm_trace.get("raw_response"):
        try:
            ai_sections = normalize_ai_sections(_extract_json_object(llm_trace["raw_response"]))
        except Exception:
            ai_sections = normalize_ai_sections(ai_sections)
    else:
        ai_sections = normalize_ai_sections(ai_sections)
    pe = ai_sections.get("prompt_echo") or report_ctx.get("prompt_blocks") or {}

    chart_js: list[str] = []
    badges = ""
    charts_html = ""

    net_overview_html, net_chart_js = _render_network_overview(network_bundle)
    chart_js.extend(net_chart_js)

    has_flow = motor.get("has_flow") or sum(motor.get("vc_dist") or []) > 0
    if motor.get("ok") and mode_active(scope, "motor") and has_flow:
        vc = motor.get("vc_dist") or [0, 0, 0, 0]
        labels = ["畅通(<0.6)", "基本畅通", "轻度拥堵", "严重拥堵(≥1.0)"]
        charts_html += '<div class="chart-box" id="chart_vc_dist"></div>'
        chart_js.append(_js_pie("chart_vc_dist", "机动车 V/C 拥堵结构", labels, vc))
        badges += _badge("平均 V/C", motor["avg_vc"], "", "bad" if motor["avg_vc"] > 0.85 else "")
        badges += _badge("拥堵路段占比", motor["congested_pct"], "%", "warn" if motor["congested_pct"] > 15 else "")
        if motor.get("type_data"):
            td = motor["type_data"]
            charts_html += '<div class="chart-box" id="chart_motor_type"></div>'
            chart_js.append(
                _js_bar(
                    "chart_motor_type",
                    "分道路等级平均 V/C",
                    [d["label"] for d in td],
                    [d["avg_vc"] for d in td],
                    "V/C",
                )
            )

    if slow.get("ok") and mode_active(scope, "slow") and slow.get("type_data"):
        td = slow["type_data"]
        charts_html += '<div class="chart-box" id="chart_slow"></div>'
        chart_js.append(
            _js_bar(
                "chart_slow",
                "慢行分类型平均流量",
                [d["label"] for d in td],
                [d["avg_flow"] for d in td],
                "人次/h",
            )
        )
        badges += _badge("慢行路网", slow["network_len_km"], "km")

    if pt.get("ok") and mode_active(scope, "pt"):
        badges += _badge("公交线路", pt["route_count"], "条")
        badges += _badge("断面均流", pt["enroute_avg"], "人次/班")
        if pt.get("link_data"):
            charts_html += '<div class="chart-box" id="chart_pt"></div>'
            chart_js.append(
                _js_bar(
                    "chart_pt",
                    "公交链路类型平均客流",
                    [d["type"] for d in pt["link_data"]],
                    [d["avg_flow"] for d in pt["link_data"]],
                    "人次",
                )
            )

    if diag.get("ok") and diag.get("rows"):
        rows = [
            r
            for r in diag["rows"]
            if r.get("num") is not None and r.get("tier", "macro") == "macro"
        ][:12]
        if rows:
            charts_html += '<div class="chart-box wide" id="chart_diag"></div>'
            labels = [
                (r.get("name") or r["code"]).replace("宏观", "")[:20]
                for r in rows
            ]
            vals = [r["num"] for r in rows]
            units = [r.get("unit") or "" for r in rows]
            y_name = units[0] if len(set(units)) == 1 else ""
            chart_js.append(
                _js_bar(
                    "chart_diag",
                    f"{scope_label(scope)}宏观诊断指标",
                    labels,
                    vals,
                    y_name,
                )
            )

    map_html, map_js = _render_map_section(network_bundle, scope=scope)

    top_table = ""
    if mode_active(scope, "motor") and motor.get("top_sat_links"):
        top_links = motor["top_sat_links"][:10]
        raw_names = [(x.get("name") or "").strip() for x in top_links]
        names_dup = len(set(raw_names)) < len(raw_names)

        def _link_row(lnk: dict) -> str:
            base = (lnk.get("name") or "").strip() or f"路段{lnk['link_id']}"
            nm = f"{base}·{lnk['link_id']}" if names_dup else base
            return (
                f"<tr><td>{html.escape(str(nm))}</td>"
                f"<td>{lnk['link_id']}</td><td>{lnk['v_c']}</td>"
                f"<td>{lnk['volume']}</td><td>{lnk['capacity']}</td></tr>"
            )

        trs = "".join(_link_row(l) for l in top_links)
        top_table = (
            "<h4 style='margin:14px 0 8px;font-size:13px;color:#1b4a82'>表1 Top 饱和路段</h4>"
            "<table><tr><th>路段名称</th><th>link_id</th><th>V/C</th><th>流量</th><th>容量</th></tr>"
            + trs
            + "</table>"
        )

    gap_html = "".join(f'<p class="gap">⚠ {html.escape(g)}</p>' for g in gaps)
    prompt_section = ""
    llm_section = ""
    source_in_title = _source_badge(llm_trace)

    ts = datetime.now().strftime("%Y-%m-%d %H:%M")
    title = meta.get("title", "现状交通诊断报告")

    leaflet = _leaflet_head()
    map_script = f"<script>\n{map_js}\n</script>" if map_js else ""

    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(title)}</title>
{_echarts_script()}
{leaflet}
<style>{_CSS}</style>
</head>
<body>
<header class="hdr">
  <h1>{html.escape(title)}</h1>
  <div class="sub">分析范围：{html.escape(scope_label(scope))} · 项目 {meta.get('project_id')} · 用户 {meta.get('user_id')} · 方案 {meta.get('case_id')} · 生成 {ts}</div>
</header>
<nav class="nav">
  <a href="#summary">摘要</a>
  <a href="#network">基础统计</a>
  <a href="#mapviz">路网地图</a>
  <a href="#charts">指标图表</a>
  <a href="#analysis">现状分析</a>
  <a href="#problems">问题诊断</a>
  <a href="#recs">改善建议</a>
  <a href="#gis">GIS操作</a>
</nav>
<div class="wrap">
  <section class="card" id="summary">
    <div class="sec-title">执行摘要（AI 输出） {source_in_title}</div>
    {gap_html}
    <p class="prose">{html.escape(ai_sections.get('executive_summary',''))}</p>
    <div class="badge-row">{badges}</div>
  </section>
  <section class="card" id="network">
    <div class="sec-title">基础路网与 OD 统计（机动车 / 慢行 / 公交）</div>
    <p class="prose" style="margin-bottom:12px">读取方案前缀表；无路网时自动回退工具前缀，与基础数据分析报告一致。</p>
    {net_overview_html or '<p class="prose">暂无路网/OD 统计数据。</p>'}
  </section>
  <section class="card" id="mapviz">
    {map_html}
  </section>
  <section class="card" id="charts">
    <div class="sec-title">现状指标图表（{html.escape(scope_label(scope))}）</div>
    <p class="prose" style="margin-bottom:12px">运行指标、诊断指标（diagnosis_* RPC 写入）与分配结果（V/C 等）。</p>
    <div class="chart-grid">{charts_html or '<p class="prose">暂无 ECharts 指标图（需分配流量或诊断指标数据）。</p>'}</div>
    {top_table}
  </section>
  <section class="card" id="analysis">
    <div class="sec-title">结果分析（AI 输出）</div>
    <div class="prose"><p>{html.escape(ai_sections.get('status_analysis',''))}</p></div>
  </section>
  <section class="card" id="problems">
    <div class="sec-title">问题诊断（AI 输出）</div>
    {_render_problems(ai_sections.get('problems') or [])}
  </section>
  <section class="card" id="recs">
    <div class="sec-title">改善建议（AI 输出）</div>
    {_render_recommendations(ai_sections.get('recommendations') or [])}
  </section>
  <section class="card" id="gis">
    <div class="sec-title">GIS 可执行操作</div>
    {_render_gis_table(ai_sections.get('gis_operations') or [])}
  </section>
</div>
<footer>TNA 现状交通 AI 诊断报告 · tna_unified_grpc_service/ai_diagnosis</footer>
<script>
{chr(10).join(chart_js)}
</script>
{map_script}
</body>
</html>"""
