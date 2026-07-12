#!/usr/bin/env python3
"""
诊断指标方案报告桥接 — 读取 diagnosis_indicator_result_rows，生成 HTML。

由 gRPC 调用：
  --report-kind base   → base_scheme_diagnosis_report（基础方案；case_id 为该项目基础方案 ID）
  --report-kind scheme → scheme_diagnosis_report（普通/改造方案；case_id 为当前方案 ID）

  param2 (JSON)：
    type                 motor | slow | pt | all（默认 all）
    scope                macro | meso_micro | all（默认 macro，仅宏观；中微观须显式 scope=meso_micro）
    base_case_id         int，仅 scheme：改扩建对比基准（通常为基础方案 0）
    renovation_summary   object，预留改扩建摘要（写入报告 meta，不参与计算）
    project_name         str，**param2 内**项目名称；缺省 output_path 时用于文件名前缀与页眉展示
    output_path          str
    include_html_content bool（默认 true → summary.attributes.content）
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import traceback
from datetime import datetime

try:
    from .diagnosis_value_utils import (
        filter_rows_for_report,
        format_display_num,
        group_by_indicator,
        indicator_tier,
        is_plausible_value,
        pick_primary_value,
        preferred_macro_rows,
        row_item,
    )
    from .network_map_collect import collect_map_bundle_for_cases
    from .network_map_report import render_map_sections
    from .report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
    from .table_name_cn import (
        table_cn,
        field_cn,
        type_cn,
        data_sources_line,
        tables_for_diagnosis,
        humanize_db_message,
    )
except ImportError:
    from diagnosis_value_utils import (
        filter_rows_for_report,
        format_display_num,
        group_by_indicator,
        indicator_tier,
        is_plausible_value,
        pick_primary_value,
        preferred_macro_rows,
        row_item,
    )
    from network_map_collect import collect_map_bundle_for_cases
    from network_map_report import render_map_sections
    from report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
    from table_name_cn import (
        table_cn,
        field_cn,
        type_cn,
        data_sources_line,
        tables_for_diagnosis,
        humanize_db_message,
    )

INDICATOR_LABELS = {
    "road_macro_density": "路网密度",
    "road_macro_tpi": "交通绩效指数 TPI",
    "road_macro_congestion_ratio": "拥堵里程比例",
    "road_macro_average_speed": "平均速度",
    "road_macro_tti": "行程时间指数 TTI",
    "road_macro_dti": "延误时间指数 DTI",
    "road_macro_speed_relation": "速度关系",
    "road_macro_capacity_distribution": "路段饱和度分布",
    "road_macro_co2": "道路交通碳排放",
    "slow_macro_walk_density": "步行网络密度",
    "slow_macro_bike_density": "骑行网络密度",
    "slow_macro_walk_continuity": "步行设施总长度",
    "slow_macro_bike_continuity": "骑行设施总长度",
    "slow_macro_area_percap": "人均慢行空间",
    "slow_macro_vc_distribution": "慢行 V/C 分布",
    "pt_macro_bus_density": "公交线路密度",
    "pt_macro_pt_pop_coverage_500m": "公交 500m 人口覆盖率",
    "pt_macro_city_bus_area_coverage": "公交区域覆盖率",
    "pt_macro_rail_pop_coverage_800m": "轨道 800m 人口覆盖率",
    # 机动车中微观
    "road_micro_flow": "路段流量",
    "road_micro_speed_kmh": "路段运行速度",
    "road_micro_v_c": "路段 V/C",
    "road_micro_tti": "路段 TTI",
    "road_micro_dti": "路段 DTI",
    "road_micro_source_tracing": "路段溯源",
    # 慢行中微观
    "slow_meso_walk_density": "慢行中观-步行密度",
    "slow_meso_bike_density": "慢行中观-局部骑行网络密度",
    "slow_meso_walk_continuity": "慢行中观-步行连续度",
    "slow_meso_bike_continuity": "慢行中观-骑行连续度",
    "slow_meso_area_percap": "慢行中观-人均空间",
    "slow_micro_vc": "慢行微观路段 V/C",
    "slow_micro_topn_vc": "慢行高负荷路段 TOP-N",
    # 公交中微观
    "pt_meso_total_routes": "公交线路条数",
    "pt_meso_avg_mileage": "线路平均里程",
    "pt_meso_avg_stations": "线路平均站点数",
    "pt_meso_max_mileage": "线路最大里程",
    "pt_meso_max_stations": "线路最大站点数",
}

try:
    from .indicator_report_meta import enrich_indicator_card, get_indicator_meta
except ImportError:
    from indicator_report_meta import enrich_indicator_card, get_indicator_meta

TYPE_PREFIX = {"motor": "road_", "slow": "slow_", "pt": "pt_"}
TIER_LABEL = {"macro": "宏观", "meso_micro": "中微观", "other": "其他"}
_SCOPE_MACRO = "macro"
_SCOPE_MESO = "meso_micro"

MODULE_ORDER = ("road", "slow", "pt")
MODULE_META = {
    "road": {"title": "机动车交通", "color": "#2d6a9f", "dot": "dm", "anchor": "s1"},
    "slow": {"title": "慢行交通", "color": "#27ae60", "dot": "ds", "anchor": "s2"},
    "pt": {"title": "公共交通", "color": "#e67e22", "dot": "dp", "anchor": "s3"},
}

ROAD_ORDER = [
    "road_macro_density",
    "road_macro_tpi",
    "road_macro_congestion_ratio",
    "road_macro_average_speed",
    "road_macro_tti",
    "road_macro_dti",
    "road_macro_speed_relation",
    "road_macro_capacity_distribution",
    "road_macro_co2",
]
SLOW_ORDER = [
    "slow_macro_walk_density",
    "slow_macro_bike_density",
    "slow_macro_walk_continuity",
    "slow_macro_bike_continuity",
    "slow_macro_area_percap",
    "slow_macro_vc_distribution",
]
PT_ORDER = [
    "pt_macro_bus_density",
    "pt_macro_pt_pop_coverage_500m",
    "pt_macro_city_bus_area_coverage",
    "pt_macro_rail_pop_coverage_800m",
]

# 机动车速度合理上限（km/h）；超出视为诊断计算异常，报告不当作有效宏观值
_MAX_PLAUSIBLE_SPEED_KMH = 180.0
_ROAD_EFF_MACRO_HINTS = ("TTI", "DTI", "拥堵", "速度")
_ROAD_EFF_MICRO_CODES = frozenset({
    "road_micro_tti",
    "road_micro_dti",
    "road_micro_speed_kmh",
    "road_micro_v_c",
})
_DISTRIBUTION_CODES = frozenset({
    "road_macro_capacity_distribution",
    "slow_macro_vc_distribution",
})


def _is_absurd_speed_kmh(v: float | None) -> bool:
    return v is not None and (v < 0 or v > _MAX_PLAUSIBLE_SPEED_KMH)


def _escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


def _unescape(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            out.append("\n" if nxt == "n" else "\r" if nxt == "r" else nxt)
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def read_request(path: str) -> dict:
    kv = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            pos = line.find("=")
            if pos < 0:
                continue
            kv[line[:pos]] = _unescape(line[pos + 1:])
    return kv


def write_response(path: str, kv: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        for k, v in kv.items():
            f.write(f"{k}={_escape(str(v))}\n")


def write_progress(path: str, percent: int, title: str) -> None:
    if not path:
        return
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"percent={percent}\ntitle={_escape(title)}\n")
    except Exception:
        pass


def _get_conn(param1: str):
    import psycopg2

    if param1 and param1.strip():
        conn = psycopg2.connect(param1)
        conn.autocommit = True
        return conn
    db_conf = os.environ.get("TNA_DB_CONF", "/opt/algorithms/db.conf")
    for p in (db_conf, "/home/giss/opt_algorithms/db.conf"):
        if not os.path.isfile(p):
            continue
        cfg = {}
        with open(p, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip()
        conn = psycopg2.connect(
            host=cfg.get("host", "localhost"),
            port=int(cfg.get("port", 5432)),
            dbname=cfg.get("dbname", "urban"),
            user=cfg.get("user", "urban"),
            password=cfg.get("password", ""),
        )
        conn.autocommit = True
        return conn
    raise RuntimeError("无法连接数据库：param1 为空且未找到 db.conf")


def _prefix(pid: int, uid: int, cid: int) -> str:
    return f"project{pid}_user{uid}_case{cid}_"


def _tbl(prefix: str, suffix: str) -> str:
    return f'user_project."{prefix}{suffix}"'


def _table_exists(conn, prefix: str, suffix: str) -> bool:
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT 1 FROM information_schema.tables
            WHERE table_schema='user_project' AND table_name=%s LIMIT 1
            """,
            (f"{prefix}{suffix}",),
        )
        return cur.fetchone() is not None


def _fetch_pipeline_status(conn, prefix: str, mode: str = "motor") -> dict:
    # 查询分配/流量表状态，用于报告顶部数据状态说明。
    status = {"flow_rows": 0, "road_vol_rows": 0, "assign_error": ""}
    is_slow = str(mode or "").lower() == "slow"
    flow_suffixes = ["slow_greedy_link_flow_results", "greedy_link_flow_results"] if is_slow else ["greedy_link_flow_results"]
    road_suffixes = ["slow_road_way", "road_way"] if is_slow else ["road_way"]
    try:
        for suf in flow_suffixes:
            if _table_exists(conn, prefix, suf):
                with conn.cursor() as cur:
                    cur.execute(f"SELECT COUNT(*) FROM {_tbl(prefix, suf)} WHERE COALESCE(flow,0)>0")
                    status["flow_rows"] = int(cur.fetchone()[0])
                break
        for suf in road_suffixes:
            if _table_exists(conn, prefix, suf):
                with conn.cursor() as cur:
                    cur.execute(f"SELECT COUNT(*) FROM {_tbl(prefix, suf)} WHERE COALESCE(volume,0)>0 OR COALESCE(v_c,0)>0")
                    status["road_vol_rows"] = int(cur.fetchone()[0])
                break
    except Exception as e:
        status["assign_error"] = str(e)[:220]
    if status["flow_rows"] <= 0 and status["road_vol_rows"] <= 0:
        if is_slow:
            status["assign_error"] = (
                f"常见原因：OD 最短路径不可达、路网不连通，或未生成"
                f"「{table_cn('slow_greedy_link_flow_results')}」·流量 / 「{field_cn('slow_road_way', 'volume')}」"
            )
        else:
            status["assign_error"] = "常见原因：OD 最短路径不可达或路网不连通，请检查质心连杆与节点连通性"
    return status

def _load_rows(conn, prefix: str) -> list[dict]:
    if not _table_exists(conn, prefix, "diagnosis_indicator_result_rows"):
        return []
    tbl = _tbl(prefix, "diagnosis_indicator_result_rows")
    with conn.cursor() as cur:
        cur.execute(
            f"""
            SELECT indicator_code, context_key, result_key, result_item,
                   result_value_num, result_value_text, result_unit
            FROM {tbl}
            ORDER BY indicator_code, sort_order, context_key, result_key, result_item
            """
        )
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]


def _filter_rows(rows: list[dict], type_filter: str, scope: str) -> list[dict]:
    return filter_rows_for_report(rows, scope=scope or _SCOPE_MACRO, type_filter=type_filter)


def _display_value(r: dict) -> str:
    if r.get("result_value_num") is not None:
        try:
            v = float(r["result_value_num"])
            unit = (r.get("result_unit") or "").strip()
            return f"{v:.4g}{(' ' + unit) if unit else ''}"
        except Exception:
            pass
    if r.get("result_value_text"):
        return str(r["result_value_text"])
    return "—"


_RESULT_ITEM_LABELS = {
    "flow": "分配流量",
    "v_c": "V/C",
    "vc": "V/C",
    "tti": "行程时间指数",
    "travel_time": "行程时间",
    "congestion_level": "拥挤等级",
    "density": "密度",
    "value": "指标值",
    "overall": "全网汇总",
    "overall_pct": "全网占比",
}


_DETAIL_LINK_NAMES: dict[str, str] = {}


def _load_link_name_map(conn, prefix: str) -> dict[str, str]:
    out: dict[str, str] = {}
    if conn is None:
        return out
    try:
        for suffix in ("slow_road_way", "road_way"):
            if not _table_exists(conn, prefix, suffix):
                continue
            tbl = _tbl(prefix, suffix)
            with conn.cursor() as cur:
                cur.execute("""
                    SELECT 1 FROM information_schema.columns
                    WHERE table_schema='user_project' AND table_name=%s AND column_name='name'
                    LIMIT 1
                    """, (f"{prefix}{suffix}",))
                if cur.fetchone() is None:
                    continue
                cur.execute(f"SELECT link_id, name FROM {tbl} WHERE name IS NOT NULL AND name<>''")
                for link_id, name in cur.fetchall():
                    key = str(link_id)
                    if key not in out and name:
                        out[key] = str(name)
    except Exception:
        return out
    return out


def _link_name_for_key(key: str) -> str:
    k = str(key or "").strip()
    if k.upper().startswith("LINK_"):
        k = k[5:]
    return _DETAIL_LINK_NAMES.get(k, "")


def _link_label_with_name(key: str) -> str:
    k = str(key or "").strip()
    if not k:
        return ""
    nm = _link_name_for_key(k)
    if k.upper().startswith("LINK_"):
        base = "路段" + k[5:]
    else:
        base = k
    return base + ((" · " + nm) if nm else "")


def _detail_label(code: str, r: dict) -> str:
    """Build a readable label for each detail-table row."""
    base = INDICATOR_LABELS.get(code, code)
    item = (row_item(r) or "").strip()
    item_key = item.lower()
    key = (r.get("result_key") or "").strip()
    ctx = (r.get("context_key") or "").strip()

    def obj_label() -> str:
        k = key or ctx or "GLOBAL"
        if k.upper().startswith("LINK_"):
            nm = _link_name_for_key(k)
            return "\u8def\u6bb5" + k[5:] + ((" · " + nm) if nm else "")
        if k.upper() == "TAZ_GLOBAL" or k.upper() == "GLOBAL":
            return "GLOBAL"
        return k

    if key.lower() in {"overall", "overall_pct", "value"}:
        key = "GLOBAL"
    if ctx.lower() in {"overall", "overall_pct", "value"}:
        ctx = "GLOBAL"

    item_map = {
        "density": "\u5bc6\u5ea6",
        "walk_total_km": "\u6b65\u884c\u8bbe\u65bd\u957f\u5ea6",
        "bike_total_km": "\u9a91\u884c\u8bbe\u65bd\u957f\u5ea6",
        "region_area_km2": "\u7814\u7a76\u533a\u9762\u79ef",
        "total_length_km": "\u53c2\u4e0e\u8fde\u901a\u6027\u5224\u65ad\u603b\u957f\u5ea6",
        "segment_count": "\u8def\u6bb5\u6570",
        "node_count": "\u8282\u70b9\u6570",
        "dead_end_count": "\u65ad\u70b9\u6570",
        "isolated_segment_count": "\u5b64\u7acb\u6bb5\u6570",
        "dead_end_ratio_pct": "\u65ad\u70b9\u6bd4\u4f8b",
        "area_per_capita_m2": "\u4eba\u5747\u6162\u884c\u7a7a\u95f4",
        "total_slow_area_m2": "\u6162\u884c\u9053\u8def\u9762\u79ef",
        "total_population": "\u5e38\u4f4f\u4eba\u53e3",
        "length_km": "\u8bbe\u65bd\u957f\u5ea6",
        "slow_area_m2": "\u6162\u884c\u9053\u8def\u9762\u79ef",
        "flow": "\u5206\u914d\u6d41\u91cf",
        "v_c": "V/C",
        "vc": "V/C",
        "travel_time": "\u884c\u7a0b\u65f6\u95f4",
        "tti": "\u884c\u7a0b\u65f6\u95f4\u6307\u6570",
        "congestion_level": "\u62e5\u6324\u7b49\u7ea7",
        "value": "\u6307\u6807\u503c",
        "overall": "\u5168\u7f51",
        "overall_pct": "\u5168\u7f51\u5360\u6bd4",
    }
    item_label = item_map.get(item_key, item or "\u6307\u6807\u503c")

    meso_codes = {
        "slow_meso_walk_density", "slow_meso_bike_density",
        "slow_meso_walk_continuity", "slow_meso_bike_continuity",
        "slow_meso_area_percap",
    }
    macro_codes = {
        "slow_macro_walk_density", "slow_macro_bike_density",
        "slow_macro_walk_continuity", "slow_macro_bike_continuity",
        "slow_macro_area_percap", "slow_macro_vc_distribution",
    }
    if code in macro_codes and obj_label() == "GLOBAL":
        if code in {"slow_macro_walk_density", "slow_macro_bike_density"} and item_key == "density":
            return base
        if code in {"slow_macro_walk_continuity", "slow_macro_bike_continuity"} and item_key == "total_length_km":
            return base
        if code == "slow_macro_area_percap" and item_key == "area_per_capita_m2":
            return base
    if code in meso_codes or code in macro_codes:
        return f"{base} \u00b7 {obj_label()} \u00b7 {item_label}"

    if code in {"slow_micro_vc", "slow_micro_topn_vc"}:
        if item_key == "flow":
            micro_base = "\u6162\u884c\u5fae\u89c2\u8def\u6bb5\u5206\u914d\u6d41\u91cf"
        elif item_key in {"travel_time", "tti", "congestion_level"}:
            micro_base = "\u6162\u884c\u5fae\u89c2\u8def\u6bb5\u8fd0\u884c\u72b6\u6001"
        else:
            micro_base = "\u6162\u884c\u5fae\u89c2\u8def\u6bb5 V/C"
        if code == "slow_micro_topn_vc" and item_key == "flow":
            micro_base = "\u6162\u884c\u9ad8\u8d1f\u8377\u8def\u6bb5 TOP-N \u5206\u914d\u6d41\u91cf"
        return f"{micro_base} \u00b7 {obj_label()} \u00b7 {item_label}"

    if item:
        return f"{base} \u00b7 {obj_label()} \u00b7 {item_label}"
    return base


def _detail_formula_reference(code: str, r: dict) -> tuple[str | None, str | None]:
    """Row-level formula/reference for detail table.

    A diagnosis indicator can contain several sub-items.  For example, the slow
    V/C indicator has flow, travel_time, tti, congestion_level and v_c rows.  A
    single indicator-level formula is misleading for those sub-items, so detail
    rows carry their own explanation when needed.
    """
    item = (row_item(r) or "").lower()
    key = (r.get("result_key") or "").strip()

    if code == "slow_macro_walk_density":
        if item == "density":
            return "步行设施总里程 ÷ 研究区面积", "常见 2~15 km/km²；用于判断全区步行设施供给密度"
        if item == "walk_total_km":
            return "统计研究区内步行设施总里程", "总里程需结合研究区面积和人口规模判断"
        if item == "region_area_km2":
            return "研究区面积", "作为网络密度计算分母"
    if code == "slow_macro_bike_density":
        if item == "density":
            return "骑行设施总里程 ÷ 研究区面积", "常见 2~15 km/km²；用于判断全区骑行设施供给密度"
        if item == "bike_total_km":
            return "统计研究区内骑行设施总里程", "总里程需结合研究区面积和人口规模判断"
        if item == "region_area_km2":
            return "研究区面积", "作为网络密度计算分母"

    if code in {"slow_macro_walk_continuity", "slow_macro_bike_continuity"}:
        prefix = "步行" if code == "slow_macro_walk_continuity" else "骑行"
        if item == "total_length_km":
            return f"统计研究区内{prefix}设施长度总和（非最长段、非占比）", "越长表示设施规模越大；需结合网络密度、断点数量和研究区面积判断"
        if item == "segment_count":
            return f"统计{prefix}设施路段条数", "条数越多不一定越好，需结合连通性和总长度判断"
        if item == "node_count":
            return f"统计{prefix}网络节点数量", "用于辅助判断网络复杂度"
        if item == "dead_end_count":
            return f"统计{prefix}网络中仅连接一条路段的端点数量", "端点较多可能提示断点或网络破碎，需要结合路网结构复核"
        if item == "isolated_segment_count":
            return f"统计两端均为端点的疑似孤立{prefix}路段数量", "数量越多说明局部孤立风险越明显"
        if item == "dead_end_ratio_pct":
            return f"{prefix}网络端点数量 ÷ 节点总数", "比例越高，说明连通性问题越明显"

    if code == "slow_macro_area_percap":
        if item == "area_per_capita_m2":
            return "全区慢行道路面积 ÷ 常住人口", "常见 2~10 m²/人；用于判断整体慢行空间供给"
        if item == "total_slow_area_m2":
            return "慢行设施长度 × 车道数 × 设施宽度", "用于估算慢行道路总面积"
        if item == "total_population":
            return "研究区常住人口", "作为人均慢行空间计算分母"

    if code in {"slow_meso_walk_density", "slow_meso_bike_density"}:
        prefix = "步行" if code == "slow_meso_walk_density" else "骑行"
        if item == "density":
            return f"小区/TAZ 内{prefix}设施长度 ÷ 小区/TAZ 面积", "用于比较各小区设施供给密度，低值片区通常是供给短板"
        if item == "length_km":
            return f"统计小区/TAZ 内{prefix}设施长度", "需结合小区面积判断，不能只看绝对长度"

    if code == "slow_meso_area_percap":
        if item == "area_per_capita_m2":
            return "小区/TAZ 内慢行道路面积 ÷ 小区/TAZ 人口", "低值片区通常是慢行空间供给短板"
        if item == "slow_area_m2":
            return "小区/TAZ 内慢行设施长度 × 车道数 × 设施宽度", "用于估算该小区慢行道路面积"

    if code in {"slow_meso_walk_continuity", "slow_meso_bike_continuity"}:
        prefix = "步行" if code == "slow_meso_walk_continuity" else "骑行"
        if item == "dead_end_count":
            return f"统计{prefix}网络中疑似断点/端点数量", "数量越多说明局部连通性问题越明显"
        if item == "dead_end_ratio_pct":
            return f"{prefix}网络疑似断点/端点数量 ÷ 节点总数", "比例越高说明连续性风险越大"
        if item == "isolated_segment_count":
            return f"统计疑似孤立{prefix}路段数量", "数量越多说明网络破碎风险越高"
        if item == "total_length_km":
            return f"统计参与连通性判断的{prefix}设施总长度", "用于辅助判断断点比例，不是连续度百分比"

    if code in {"slow_micro_vc", "slow_micro_topn_vc"}:
        if item in {"v_c", "vc"}:
            return "慢行路段分配流量 ÷ 通行能力", "<0.8 较宽松；>1 表示超负荷"
        if item == "flow":
            return "慢行分配算法得到的路段流量", "数值越高表示该路段承担的慢行需求越大"
        if item == "congestion_level":
            return "根据路段 V/C 分档判断拥挤等级", "严重拥堵表示 V/C 较高，需优先复核"
        if item == "travel_time":
            return "根据路段长度、速度/阻抗函数计算通行时间", "数值越高表示通过该路段耗时越长"
        if item == "tti":
            return "拥挤状态通行时间 ÷ 自由流通行时间", ">1 表示受拥挤影响"

    return None, None


def _pick_primary_value(code: str, rows: list[dict]) -> dict:
    return pick_primary_value(code, rows, label=INDICATOR_LABELS.get(code, code))


def _pick_card_primary_value(code: str, rows: list[dict]) -> dict:
    """Pick the value shown on indicator cards."""
    if code in {"slow_meso_walk_density", "slow_meso_bike_density"}:
        density_rows = [
            r for r in rows
            if (row_item(r) or "").lower() == "density"
            and r.get("result_value_num") is not None
            and (r.get("result_key") or "").upper().startswith("TAZ_")
        ]
        if density_rows:
            low = min(density_rows, key=lambda r: float(r.get("result_value_num") or 0))
            return {
                "label": "最低密度片区",
                "num": float(low.get("result_value_num") or 0),
                "text": "",
                "unit": low.get("result_unit") or "km/km²",
                "key": low.get("result_key") or "",
            }

    if code == "slow_meso_area_percap":
        area_rows = [
            r for r in rows
            if (row_item(r) or "").lower() == "area_per_capita_m2"
            and r.get("result_value_num") is not None
            and (r.get("result_key") or "").upper().startswith("TAZ_")
        ]
        if area_rows:
            low = min(area_rows, key=lambda r: float(r.get("result_value_num") or 0))
            return {
                "label": "最低人均空间片区",
                "num": float(low.get("result_value_num") or 0),
                "text": "",
                "unit": low.get("result_unit") or "m²/人",
                "key": low.get("result_key") or "",
            }

    if code in {"slow_meso_walk_continuity", "slow_meso_bike_continuity"}:
        ratio_rows = [
            r for r in rows
            if (row_item(r) or "").lower() == "dead_end_ratio_pct"
            and r.get("result_value_num") is not None
        ]
        if ratio_rows:
            top = max(ratio_rows, key=lambda r: float(r.get("result_value_num") or 0))
            return {
                "label": "最高断头比例",
                "num": float(top.get("result_value_num") or 0),
                "text": "",
                "unit": top.get("result_unit") or "%",
                "key": top.get("result_key") or "TAZ_GLOBAL",
            }

    if code == "slow_macro_vc_distribution":
        severe = [
            r for r in rows
            if r.get("result_value_num") is not None
            and (
                int(r.get("sort_order") or 0) == 4
                or "严重拥堵" in (
                    str(row_item(r))
                    + str(r.get("result_key") or "")
                    + str(r.get("result_label") or "")
                    + str(r.get("result_name") or "")
                )
            )
        ]
        if severe:
            v = float(severe[0].get("result_value_num") or 0)
            return {"label": "严重拥堵占比", "num": v, "text": "", "unit": "%"}
        p = dict(_pick_primary_value(code, rows))
        p["unit"] = ""
        return p

    if code in {"slow_micro_vc", "slow_micro_topn_vc"}:
        vc_rows = [
            r for r in rows
            if (row_item(r) or "").lower() in {"v_c", "vc"}
            and r.get("result_value_num") is not None
        ]
        if vc_rows:
            if code == "slow_micro_topn_vc":
                vals = [float(r.get("result_value_num") or 0) for r in vc_rows]
                top = max(vc_rows, key=lambda r: float(r.get("result_value_num") or 0))
                return {
                    "label": "TOP-N 平均 V/C",
                    "num": sum(vals) / len(vals) if vals else None,
                    "text": "",
                    "unit": "",
                    "key": top.get("result_key") or "",
                }
            top = max(vc_rows, key=lambda r: float(r.get("result_value_num") or 0))
            return {
                "label": "最大 V/C",
                "num": float(top.get("result_value_num") or 0),
                "text": "",
                "unit": "",
                "key": top.get("result_key") or "",
            }
    return _pick_primary_value(code, rows)


def _global_rows(rows: list[dict]) -> list[dict]:
    g = [r for r in rows if (r.get("context_key") or "").upper() == "GLOBAL"]
    return g if g else list(rows)


def _split_by_module(groups: dict[str, list[dict]]) -> dict[str, dict[str, list[dict]]]:
    out: dict[str, dict[str, list[dict]]] = {m: {} for m in MODULE_ORDER}
    for code, rows in groups.items():
        if code.startswith("road_"):
            out["road"][code] = rows
        elif code.startswith("slow_"):
            out["slow"][code] = rows
        elif code.startswith("pt_"):
            out["pt"][code] = rows
    return out


def _fmt_num(v: float | None, digits: int = 2) -> str:
    if v is None:
        return "—"
    av = abs(v)
    if av >= 1e6:
        return f"{v:.3e}"
    if av >= 100:
        return f"{v:.1f}"
    return f"{v:.{digits}f}"


def _build_narratives(
    modules: dict[str, dict[str, list[dict]]],
    report_kind: str,
    cid: int,
    base_cid: int | None,
    compare_rows: list[dict],
    pipeline: dict | None = None,
    type_filter: str = "all",
) -> dict[str, str]:
    road_k = {c: _pick_primary_value(c, modules["road"][c]) for c in ROAD_ORDER if c in modules["road"]}
    slow_k = {c: _pick_primary_value(c, modules["slow"][c]) for c in SLOW_ORDER if c in modules["slow"]}
    pt_k = {c: _pick_primary_value(c, modules["pt"][c]) for c in PT_ORDER if c in modules["pt"]}

    tti = road_k.get("road_macro_tti", {}).get("num")
    dti = road_k.get("road_macro_dti", {}).get("num")
    cong = road_k.get("road_macro_congestion_ratio", {}).get("num")
    spd = road_k.get("road_macro_average_speed", {}).get("num")
    co2 = road_k.get("road_macro_co2", {}).get("num")
    if _is_absurd_speed_kmh(spd):
        spd = None
    pt_cov = pt_k.get("pt_macro_pt_pop_coverage_500m", {}).get("num")
    walk_d = slow_k.get("slow_macro_walk_density", {}).get("num")
    bike_d = slow_k.get("slow_macro_bike_density", {}).get("num")

    scheme_label = "基础方案" if report_kind == "base" else f"方案 case{cid}"
    exec_lines = []
    pl = pipeline or {}
    flow_rows = int(pl.get("flow_rows") or 0)
    vol_rows = int(pl.get("road_vol_rows") or 0)
    if type_filter == "slow":
        if flow_rows <= 0 or vol_rows <= 0:
            exec_lines.append(
                f"<strong>数据状态：</strong>方案 case{cid} 慢行分配<strong>未成功或流量为空</strong>"
                f"（流量表 {flow_rows} 行，有流量路段 {vol_rows} 条）。"
                "慢行 V/C、容量影响路段与中微观运行压力等指标需要先完成慢行分配。"
            )
        else:
            exec_lines.append(
                f"<strong>数据状态：</strong>慢行分配已完成（流量表 {flow_rows} 行，有流量路段 {vol_rows} 条），"
                "慢行宏观与中微观诊断指标可用于报告展示。"
            )
        if report_kind == "scheme" and base_cid is not None:
            exec_lines.extend([
                f"本报告对 <strong>普通方案 case{cid}</strong> 相对 <strong>基础方案 case{base_cid}</strong> 的慢行交通诊断指标变化进行对比分析，"
                "重点评价扩容后步行/骑行网络供给、连续性、人均慢行空间、慢行饱和度与容量影响后的运行压力变化。",
                f"普通方案慢行方面：步行网络密度 <strong>{_fmt_num(walk_d)} km/km²</strong>，骑行网络密度 <strong>{_fmt_num(bike_d)} km/km²</strong>。",
            ])
        else:
            exec_lines.extend([
                f"本报告对 <strong>{scheme_label}</strong> 的慢行交通诊断指标进行系统梳理，"
                "重点评价步行/骑行网络供给、连续性、人均慢行空间、慢行饱和度与容量影响后的运行压力。",
                f"慢行方面：步行网络密度 <strong>{_fmt_num(walk_d)} km/km²</strong>，骑行网络密度 <strong>{_fmt_num(bike_d)} km/km²</strong>。",
            ])
        slow_txt = slow_k.get("slow_macro_vc_distribution", {}).get("text") or ""
        slow_narr = (
            "慢行系统关注网络可达性与空间品质。步行/骑行密度反映路网供给水平；"
            "连续度指标反映断头路与破碎网络风险。人均慢行空间与 V/C 分布共同刻画慢行资源供需匹配。"
            + (f" 当前慢行网络整体以 <strong>{slow_txt}</strong> 等级路段为主。" if slow_txt else "")
        )
        return {"exec": "".join(f"<p>{x}</p>" for x in exec_lines), "motor": "", "slow": slow_narr, "pt": ""}

    if flow_rows <= 0 or vol_rows <= 0:
        exec_lines.append(
            f"<strong>数据状态：</strong>方案 case{cid} 交通分配<strong>未成功或流量为空</strong>"
            f"（{table_cn('greedy_link_flow_results')}={flow_rows} 行，{field_cn('road_way', 'volume')}&gt;0 共 {vol_rows} 条）。"
            "机动车 TTI、DTI、拥堵比例、平均速度、通行能力分布等<strong>依赖分配流量的宏观指标无法计算</strong>。"
            + (f" 最近分配错误：{pl.get('assign_error')}" if pl.get("assign_error") else "")
            + " 请先执行 <code>review_road</code> 或 <code>base_motor_network</code> 并确保 OD 全网可达。"
        )
    else:
        exec_lines.append(
            f"<strong>数据状态：</strong>分配已完成（流量表 {flow_rows} 行，有流量路段 {vol_rows} 条），"
            "机动车宏观指标可计算（见下方图表与指标卡）。"
        )
    exec_lines.extend([
        f"本报告对 <strong>{scheme_label}</strong> 在分配完成后的宏观诊断指标进行系统梳理，"
        "从机动车运行效率、慢行友好性与公共交通服务能力三个维度评价方案交通绩效。",
        f"机动车方面：行程时间指数 TTI=<strong>{_fmt_num(tti)}</strong>，"
        f"拥堵路段比例 <strong>{_fmt_num(cong)}%</strong>，"
        f"全网平均速度 <strong>{_fmt_num(spd)} km/h</strong>"
        + (
            f"，宏观道路交通碳排放 <strong>{_fmt_num(co2)} t CO₂/高峰h</strong>"
            if co2 is not None
            else ""
        )
        + "。",
        f"慢行方面：步行网络密度 <strong>{_fmt_num(walk_d)} km/km²</strong>，骑行网络密度 <strong>{_fmt_num(bike_d)} km/km²</strong>。",
        f"公交方面：500 m 人口覆盖率 <strong>{_fmt_num(pt_cov)}%</strong>。",
    ])
    if compare_rows and base_cid is not None:
        improved = sum(1 for c in compare_rows if c.get("delta") is not None and c["delta"] < 0
                       and c["code"] in ("road_macro_tti", "road_macro_dti", "road_macro_congestion_ratio"))
        worsened = sum(1 for c in compare_rows if c.get("delta") is not None and c["delta"] > 0
                       and c["code"] in ("road_macro_tti", "road_macro_dti", "road_macro_congestion_ratio"))
        exec_lines.append(
            f"相对基础方案 case{base_cid}：拥堵类指标改善 <strong>{improved}</strong> 项、恶化 <strong>{worsened}</strong> 项"
            "（TTI/DTI/拥堵比例越低越优）。"
        )

    motor = (
        f"路网运行效率是方案交通绩效的核心。TTI=<strong>{_fmt_num(tti)}</strong> 反映行程时间相对自由流的变化，"
        f"DTI=<strong>{_fmt_num(dti)}</strong> 体现延误水平；拥堵比例 <strong>{_fmt_num(cong)}%</strong> 表征拥堵路段占比。"
        + (
            f" 宏观碳排放 <strong>{_fmt_num(co2)} t CO₂/高峰h</strong>（VKT×0.18 kg/(pcu·km)，IPCC 乘用车均值）。"
            if co2 is not None
            else ""
        )
        + "建议结合 <strong>通行能力分布</strong> 与 <strong>速度关系</strong> 图，识别瓶颈走廊与等级路不匹配区段。"
    )
    slow_txt = slow_k.get("slow_macro_vc_distribution", {}).get("text") or ""
    slow_narr = (
        f"慢行系统关注网络可达性与空间品质。步行/骑行密度反映路网供给水平；连续度指标反映断头路与破碎网络风险。"
        f"人均慢行空间与 V/C 分布共同刻画慢行资源供需匹配。"
        + (f" 当前慢行网络整体以 <strong>{slow_txt}</strong> 等级路段为主。" if slow_txt else "")
    )
    pt_n = (
        f"公共交通诊断侧重线网密度与人口/用地覆盖。500 m 与 800 m 缓冲区覆盖率是轨道+公交一体化评价的关键口径，"
        f"区域覆盖率反映公交服务与建成区匹配程度。覆盖率偏低时，应优先补强轨道站点与居住区、就业区接驳。"
    )
    return {"exec": "".join(f"<p>{x}</p>" for x in exec_lines), "motor": motor, "slow": slow_narr, "pt": pt_n}


def _extract_distribution_pie(rows: list[dict], *, code: str = "") -> list[dict]:
    """多档分布类指标 → 饼图数据（畅通/拥挤等分档）；去重 overall/全网统归 双写。"""
    import math

    src = preferred_macro_rows(rows, code) if code else rows
    slices: list[dict] = []
    seen: dict[str, float] = {}
    for r in src:
        num = r.get("result_value_num")
        if num is None:
            continue
        try:
            v = float(num)
        except (TypeError, ValueError):
            continue
        if math.isnan(v):
            continue
        name = row_item(r) or r.get("result_key") or ""
        if name in ("value", "overall", "density", "overall_pct", "全网汇总"):
            continue
        if name in seen:
            continue
        seen[name] = v
        slices.append({"name": name, "value": round(v, 2)})
    return slices


def _extract_vc_pie(rows: list[dict]) -> list[dict]:
    """慢行 V/C 分布 → 饼图数据。"""
    return _extract_distribution_pie(rows, code="slow_macro_vc_distribution")


def _segment_label(r: dict) -> str:
    ctx = (r.get("context_key") or "").strip()
    key = (r.get("result_key") or "").strip()
    if ctx and ctx.upper() != "GLOBAL":
        return f"{ctx}" + (f"/{key}" if key and key != ctx else "")
    if key:
        return key
    return "—"


def _segment_chart_label(r: dict, rows: list[dict]) -> str:
    """Chart x-axis label: object + sub-item only, no repeated indicator name."""
    base = _segment_label(r)
    item = (row_item(r) or "").strip()
    if base.upper() == "TAZ_GLOBAL":
        base = "\u5168\u7f51"
    elif base.upper().startswith("LINK_"):
        nm = _link_name_for_key(base)
        base = "\u8def\u6bb5" + base[5:] + ((" · " + nm) if nm else "")
    if not item:
        return base
    key = (r.get("result_key") or "").strip()
    ctx = (r.get("context_key") or "").strip()
    same_scope = [
        x for x in rows
        if (x.get("result_key") or "").strip() == key
        and (x.get("context_key") or "").strip() == ctx
        and row_item(x)
    ]
    if len(same_scope) <= 1:
        return base
    item_map = {
        "total_length_km": "\u603b\u957f\u5ea6",
        "segment_count": "\u8def\u6bb5\u6570",
        "node_count": "\u8282\u70b9\u6570",
        "dead_end_count": "\u65ad\u70b9\u6570",
        "isolated_segment_count": "\u5b64\u7acb\u6bb5\u6570",
        "dead_end_ratio_pct": "\u65ad\u70b9\u6bd4\u4f8b",
        "density": "\u5bc6\u5ea6",
        "length_km": "\u957f\u5ea6",
        "area_per_capita_m2": "\u4eba\u5747\u7a7a\u95f4",
        "slow_area_m2": "\u6162\u884c\u9762\u79ef",
        "flow": "\u6d41\u91cf",
        "v_c": "V/C",
        "vc": "V/C",
        "travel_time": "\u65f6\u95f4",
        "tti": "TTI",
        "congestion_level": "\u72b6\u6001",
    }
    label = item_map.get(item.lower(), item)
    return f"{base}\u00b7{label}"


def _segment_bar_data(rows: list[dict], limit: int = 25, code: str = "") -> dict | None:
    """中微观指标 → Top-N 柱状图数据。"""
    candidates: list[tuple[str, float, str]] = []
    if code == "slow_micro_vc":
        rows = [
            r for r in rows
            if (row_item(r) or "").lower() in {"v_c", "vc"}
        ]
    elif code == "slow_micro_topn_vc":
        rows = [
            r for r in rows
            if (row_item(r) or "").lower() == "flow"
        ]
    for r in rows:
        if r.get("result_value_num") is None:
            continue
        ctx = (r.get("context_key") or "").strip()
        if ctx.upper() == "GLOBAL" and len(rows) > 3:
            continue
        candidates.append((_segment_chart_label(r, rows), float(r["result_value_num"]), (r.get("result_unit") or "").strip()))
    if not candidates:
        for r in _global_rows(rows):
            if r.get("result_value_num") is not None:
                candidates.append(
                    (_segment_chart_label(r, rows), float(r["result_value_num"]), (r.get("result_unit") or "").strip())
                )
    if not candidates:
        return None
    candidates.sort(key=lambda x: abs(x[1]), reverse=True)
    top = candidates[:limit]
    return {
        "labels": [t[0] for t in top],
        "values": [round(t[1], 4) for t in top],
        "unit": top[0][2] if top else "",
    }


def _module_of_code(code: str) -> str:
    if code.startswith("road_"):
        return "road"
    if code.startswith("slow_"):
        return "slow"
    if code.startswith("pt_"):
        return "pt"
    return "other"


def _build_indicator_cards(groups: dict[str, list[dict]], *, scope: str) -> list[dict]:
    """全部诊断指标一览：默认仅宏观；scope=meso_micro 时仅中微观。分布类指标用图表展示，不出文字卡。"""
    cards = []
    for code in sorted(groups.keys()):
        if code in _DISTRIBUTION_CODES and code != "slow_macro_vc_distribution":
            continue
        tier = indicator_tier(code)
        if scope == _SCOPE_MACRO and tier != "macro":
            continue
        if scope == _SCOPE_MESO and tier != "meso_micro":
            continue
        rows = groups[code]
        p = _pick_card_primary_value(code, rows)
        if code in ("road_macro_average_speed",) and _is_absurd_speed_kmh(p.get("num")):
            continue
        card = enrich_indicator_card(code, {
            "code": code,
            "label": INDICATOR_LABELS.get(code, code),
            "tier": tier,
            "tier_label": TIER_LABEL.get(tier, "其他"),
            "module": _module_of_code(code),
            "rows": len(rows),
            "value": _fmt_num(p["num"]) if p["num"] is not None else (p["text"] or "—"),
            "unit": p["unit"],
        }, rows)
        if code in {"slow_macro_walk_continuity", "slow_macro_bike_continuity"}:
            mode = "\u6b65\u884c" if code == "slow_macro_walk_continuity" else "\u9a91\u884c"
            card["formula"] = f"{mode}\u8bbe\u65bd\u53ef\u8fde\u901a\u8def\u6bb5\u957f\u5ea6\u603b\u548c\uff08\u975e\u6700\u957f\u5355\u6bb5\u3001\u975e\u5360\u6bd4\uff09"
            card["reference"] = "\u6570\u503c\u8d8a\u5927\u8868\u793a\u8fde\u7eed\u53ef\u901a\u884c\u8bbe\u65bd\u89c4\u6a21\u8d8a\u5927\uff1b\u9700\u7ed3\u5408\u7814\u7a76\u533a\u9762\u79ef\u3001\u7f51\u7edc\u5bc6\u5ea6\u548c\u65ad\u70b9\u6570\u91cf\u7efc\u5408\u5224\u65ad"
            card["footnote"] = f"\u8ba1\u7b97\uff1a\u7edf\u8ba1\u7814\u7a76\u533a\u5185{mode}\u8bbe\u65bd\u53ef\u8fde\u901a\u8def\u6bb5\u603b\u957f\u5ea6\u3000\u53c2\u8003\uff1a\u6570\u503c\u8d8a\u5927\u8868\u793a\u8fde\u7eed\u53ef\u901a\u884c\u8bbe\u65bd\u89c4\u6a21\u8d8a\u5927\uff0c\u9700\u7ed3\u5408\u65ad\u70b9\u6570\u91cf\u548c\u7f51\u7edc\u5bc6\u5ea6\u5224\u65ad"
        if code == "road_macro_co2":
            card["label"] = "道路交通碳排放估算"
            card["formula"] = "按路段分配流量 × 路段长度 × 机动车排放因子估算；单位为吨 CO₂/高峰小时。"
            if p["num"] is not None and float(p["num"]) > 1000:
                card["footnote"] = (
                    "当前值偏高，通常由分配流量或 OD 需求量级过大导致；"
                    "请结合路段流量、容量和 OD 单位复核。"
                )
            else:
                card["footnote"] = "估算值用于方案间对比，需结合分配流量和 OD 单位复核。"
        if code == "slow_macro_vc_distribution":
            severe_rows = [
                r for r in rows
                if r.get("result_value_num") is not None
                and (
                    int(r.get("sort_order") or 0) == 4
                    or "严重拥堵" in (
                        str(row_item(r))
                        + str(r.get("result_key") or "")
                        + str(r.get("result_label") or "")
                        + str(r.get("result_name") or "")
                    )
                )
            ]
            card["label"] = "慢行严重拥堵占比"
            if severe_rows:
                card["value"] = f"{float(severe_rows[0].get('result_value_num') or 0):.1f}"
                card["unit"] = "%"
            elif "/" in str(card.get("value") or ""):
                card["value"] = str(card.get("value") or "").split("/")[-1].strip()
                card["unit"] = ""
            card["formula"] = "按慢行路段 V/C 分档统计，其中重点关注严重拥堵占比"
            card["reference"] = "严重拥堵占比越高，说明慢行网络运行压力越大"
            card["footnote"] = "计算：按慢行路段 V/C 分档统计，卡片显示严重拥堵占比；完整分档见下方图表"
        if code in {"slow_meso_walk_density", "slow_meso_bike_density"} and p.get("key"):
            card["label"] = "最低局部步行网络密度" if code == "slow_meso_walk_density" else "最低局部骑行网络密度"
            card["formula"] = "按各小区/TAZ 内慢行设施长度 ÷ 小区/TAZ 面积计算局部密度"
            card["reference"] = "卡片显示最低密度片区，用于识别慢行设施供给短板；完整片区列表见详情表和中微观图表"
            card["footnote"] = f"取值片区：{p.get('key')}；这是各小区/TAZ 中的最低值，不是全局平均值。"
        if code == "slow_meso_area_percap" and p.get("key"):
            card["label"] = "最低局部人均慢行空间"
            card["formula"] = "按各小区/TAZ 内慢行道路面积 ÷ 小区/TAZ 人口计算"
            card["reference"] = "卡片显示最低人均空间片区，用于识别慢行空间供给短板"
            card["footnote"] = f"取值片区：{p.get('key')}；这是各小区/TAZ 中的最低值，不是全局平均值。"
        if code in {"slow_meso_walk_continuity", "slow_meso_bike_continuity"} and p.get("key"):
            card["label"] = "步行连通性问题占比" if code == "slow_meso_walk_continuity" else "骑行连通性问题占比"
            card["formula"] = "疑似断点/端点数量 ÷ 网络节点总数"
            card["reference"] = "比例越高说明网络越破碎、连续性风险越明显；断点数量和孤立段见详情表"
            key = p.get("key")
            if str(key).upper() == "TAZ_GLOBAL":
                card["footnote"] = "取值范围：全网统计；这是连通性问题占比，不是设施长度，也不是普通连续度均值。"
            else:
                card["footnote"] = f"取值片区：{key}；这是该片区连通性问题占比，比例越高表示连续性短板越明显。"
        if code == "slow_micro_vc" and p.get("key"):
            card["label"] = "最高慢行路段 V/C"
            card["formula"] = "按慢行路段流量 ÷ 通行能力计算 V/C，并取最大值"
            card["reference"] = "卡片显示压力最大的慢行路段；完整路段列表见详情表和中微观图表"
            card["footnote"] = f"取值路段：{_link_label_with_name(p.get('key'))}；这是慢行路段中的最高 V/C，用于识别运行压力最大路段。"
        if code == "slow_micro_topn_vc" and p.get("key"):
            card["label"] = "TOP-N 高负荷路段平均 V/C"
            card["formula"] = "按 V/C 从高到低排序，取前 N 条慢行高负荷路段，并计算这些路段的平均 V/C"
            card["reference"] = "用于判断高负荷路段组的整体压力；单条最高值见“最高慢行路段 V/C”"
            card["footnote"] = f"TOP-N 表示按 V/C 排名前 N 的慢行路段；卡片显示这组路段的平均 V/C，最高单条路段为 {_link_label_with_name(p.get('key'))}。"
        cards.append(card)
    return cards


def _build_meso_micro_charts(groups: dict[str, list[dict]]) -> list[dict]:
    charts = []
    for code in sorted(groups.keys()):
        if indicator_tier(code) != "meso_micro":
            continue
        rows = groups[code]
        bar = _segment_bar_data(rows, code=code)
        if not bar:
            continue
        mod = _module_of_code(code)
        labels = (bar["labels"] or [])[:10]
        values = (bar["values"] or [])[:10]
        charts.append({
            "id": f"meso_{code}",
            "code": code,
            "title": INDICATOR_LABELS.get(code, code),
            "module": mod,
            "tier_label": TIER_LABEL["meso_micro"],
            "labels": labels,
            "values": values,
            "unit": bar["unit"],
            "rows": len(labels),
        })
    return charts



def _detail_numeric_rank_value(d: dict) -> float | None:
    """Best-effort numeric value used only for detail-table Top10 display."""
    import re
    text = str(d.get("display") or "")
    m = re.search(r"-?\d+(?:\.\d+)?(?:e[+-]?\d+)?", text, re.I)
    if not m:
        return None
    try:
        return abs(float(m.group(0)))
    except Exception:
        return None


def _detail_object_key(d: dict) -> tuple:
    """Group detail rows by indicator object so related sub-items stay together."""
    code = d.get("code") or ""
    ctx = d.get("ctx") or ""
    key = d.get("key") or ""
    # result_key is usually LINK_x / TAZ_x / bucket. If missing, fall back to label.
    obj = key or ctx or d.get("label") or d.get("item") or ""
    return (code, str(ctx), str(obj))


def _limit_detail_rows_topn(detail: list[dict], topn: int = 10) -> list[dict]:
    """Limit HTML detail table only: each indicator keeps Top-N objects.

    For one object (e.g. one link/TAZ), keep all its related sub-item rows, so
    users can still see V/C, flow, capacity, status, etc. Database results and
    charts/cards are not changed.
    """
    from collections import defaultdict
    by_code: dict[str, list[dict]] = defaultdict(list)
    for d in detail:
        by_code[str(d.get("code") or "")].append(d)
    out: list[dict] = []
    for code, rows in by_code.items():
        groups: dict[tuple, list[dict]] = defaultdict(list)
        for r in rows:
            groups[_detail_object_key(r)].append(r)
        if len(groups) <= topn:
            out.extend(rows)
            continue
        ranked = []
        for gkey, grows in groups.items():
            vals = [_detail_numeric_rank_value(x) for x in grows]
            vals = [v for v in vals if v is not None]
            rank = max(vals) if vals else -1.0
            ranked.append((rank, gkey))
        keep = {gkey for _, gkey in sorted(ranked, key=lambda x: x[0], reverse=True)[:topn]}
        out.extend([r for r in rows if _detail_object_key(r) in keep])
    return out


_HIGHER_IS_BETTER_CODES = {
    "slow_macro_area_percap",
    "slow_macro_walk_density",
    "slow_macro_bike_density",
    "slow_macro_walk_continuity",
    "slow_macro_bike_continuity",
    "slow_meso_area_percap",
    "slow_meso_walk_density",
    "slow_meso_bike_density",
    "slow_meso_walk_continuity",
    "slow_meso_bike_continuity",
    "pt_macro_pt_pop_coverage_500m",
    "pt_macro_pt_area_coverage_500m",
    "pt_macro_rail_pop_coverage_800m",
    "pt_macro_rail_area_coverage_800m",
}

_LOWER_IS_BETTER_CODES = {
    "slow_macro_vc_distribution",
    "slow_meso_walk_continuity",
    "slow_meso_bike_continuity",
    "slow_micro_vc",
    "slow_micro_topn_vc",
    "road_macro_tti",
    "road_macro_dti",
    "road_macro_congestion_ratio",
    "road_macro_co2",
}


def _trend_for_delta(code: str, delta: float | None) -> dict:
    if delta is None:
        return {"text": "\u4ec5\u5bf9\u6bd4", "cls": "flat"}
    try:
        d = float(delta)
    except Exception:
        return {"text": "\u4ec5\u5bf9\u6bd4", "cls": "flat"}
    if abs(d) < 1e-9:
        return {"text": "\u57fa\u672c\u4e0d\u53d8", "cls": "flat"}
    if code in _HIGHER_IS_BETTER_CODES:
        return {"text": "\u6539\u5584" if d > 0 else "\u9000\u5316", "cls": "good" if d > 0 else "bad"}
    if code in _LOWER_IS_BETTER_CODES:
        return {"text": "\u6539\u5584" if d < 0 else "\u9000\u5316", "cls": "good" if d < 0 else "bad"}
    return {"text": "\u589e\u52a0" if d > 0 else "\u51cf\u5c11", "cls": "flat"}


def _apply_card_comparison(
    cards: list[dict],
    compare_rows: list[dict],
    *,
    cid: int | None = None,
    base_cid: int | None = None,
) -> list[dict]:
    """Add base/scheme comparison info to indicator cards for scheme reports only."""
    if not compare_rows or base_cid is None:
        return cards
    by_code = {str(r.get("code") or ""): r for r in compare_rows}
    for card in cards:
        code = str(card.get("code") or "")
        c = by_code.get(code)
        if not c:
            continue
        base = c.get("base")
        scheme = c.get("scheme")
        delta = c.get("delta")
        if base is None or scheme is None:
            continue
        unit = card.get("unit") or c.get("unit") or ""
        # ?????????????????/?????????????
        # ???????????=???????
        if code in {"slow_meso_walk_continuity", "slow_meso_bike_continuity"}:
            trend = {"text": "\u6539\u5584" if float(delta) < 0 else "\u9000\u5316", "cls": "good" if float(delta) < 0 else "bad"}
            if abs(float(delta)) < 1e-9:
                trend = {"text": "\u57fa\u672c\u4e0d\u53d8", "cls": "flat"}
        else:
            trend = _trend_for_delta(code, delta)
        card["compare_enabled"] = True
        card["compare_base_label"] = f"\u57fa\u7840 case{base_cid}"
        card["compare_scheme_label"] = f"\u666e\u901a case{cid}" if cid is not None else "\u666e\u901a\u65b9\u6848"
        card["compare_base"] = _fmt_num(float(base))
        card["compare_scheme"] = _fmt_num(float(scheme))
        card["compare_delta"] = _fmt_num(float(delta)) if delta is not None else "?"
        card["compare_delta_signed"] = (("+" if float(delta) > 0 else "") + _fmt_num(float(delta))) if delta is not None else "?"
        card["compare_unit"] = unit
        card["compare_trend"] = trend["text"]
        card["compare_trend_cls"] = trend["cls"]
    return cards

def _compare_detail_object_key(d: dict) -> tuple:
    code = str(d.get("code") or "")
    ctx = str(d.get("ctx") or "")
    key = str(d.get("key") or "")
    obj = key or ctx or str(d.get("label") or "")
    return (code, ctx, obj)


def _build_compare_detail_rows(
    base_groups: dict[str, list[dict]] | None,
    scheme_groups: dict[str, list[dict]],
    *,
    scope: str = _SCOPE_MACRO,
    topn: int = 10,
) -> list[dict]:
    """Build compare detail rows for scheme reports.

    Match by indicator + context + object + item, then keep Top-N changed objects
    per indicator. This is only for HTML display; database rows are unchanged.
    """
    if not base_groups:
        return []
    from collections import defaultdict

    def include_code(code: str) -> bool:
        tier = indicator_tier(code)
        if scope == _SCOPE_MACRO:
            return tier == "macro"
        if scope == _SCOPE_MESO:
            return tier == "meso_micro"
        return tier in ("macro", "meso_micro")

    def row_key(code: str, r: dict) -> tuple:
        return (
            code,
            str(r.get("context_key") or "GLOBAL"),
            str(r.get("result_key") or ""),
            str(row_item(r) or ""),
        )

    def row_trend(code: str, item: str, delta: float) -> dict:
        item_text = str(item or "")
        if abs(delta) < 1e-9:
            return {"text": "\u57fa\u672c\u4e0d\u53d8", "cls": "flat"}
        if code == "slow_macro_vc_distribution":
            if item_text in ("\u7545\u901a", "\u57fa\u672c\u7545\u901a"):
                return {"text": "\u6539\u5584" if delta > 0 else "\u9000\u5316", "cls": "good" if delta > 0 else "bad"}
            if item_text in ("\u62e5\u6324", "\u4e25\u91cd\u62e5\u5835"):
                return {"text": "\u6539\u5584" if delta < 0 else "\u9000\u5316", "cls": "good" if delta < 0 else "bad"}
        if item_text.lower() in ("flow", "volume", "volumn", "demand") or "\u6d41\u91cf" in item_text:
            return {"text": "\u589e\u52a0" if delta > 0 else "\u51cf\u5c11", "cls": "flat"}
        return _trend_for_delta(code, delta)

    base_map: dict[tuple, dict] = {}
    for code, rows in base_groups.items():
        if not include_code(code):
            continue
        for r in rows:
            if r.get("result_value_num") is None:
                continue
            base_map[row_key(code, r)] = r

    rows_out: list[dict] = []
    for code, rows in scheme_groups.items():
        if not include_code(code):
            continue
        tier = indicator_tier(code)
        for sr in rows:
            if sr.get("result_value_num") is None:
                continue
            if not is_plausible_value(code, float(sr.get("result_value_num") or 0), (sr.get("result_unit") or "")):
                continue
            br = base_map.get(row_key(code, sr))
            if not br or br.get("result_value_num") is None:
                continue
            try:
                sv = float(sr.get("result_value_num") or 0)
                bv = float(br.get("result_value_num") or 0)
            except Exception:
                continue
            delta = sv - bv
            fml, ref = _detail_formula_reference(code, sr)
            item = row_item(sr) or ""
            trend = row_trend(code, item, delta)
            key = sr.get("result_key") or sr.get("context_key") or "GLOBAL"
            label = _detail_label(code, sr)
            obj = str(key or "GLOBAL")
            if obj.upper() == "GLOBAL":
                obj = "\u5168\u7f51"
            if item:
                obj = f"{obj} \u00b7 {item}"
            unit = (sr.get("result_unit") or br.get("result_unit") or "").strip()
            rows_out.append({
                "code": code,
                "label": label,
                "tier": TIER_LABEL.get(tier, "\u5176\u4ed6"),
                "module": _module_of_code(code),
                "object": obj,
                "ctx": sr.get("context_key") or "GLOBAL",
                "key": sr.get("result_key") or "",
                "item": item,
                "formula": fml,
                "reference": ref,
                "base": _fmt_num(bv),
                "scheme": _fmt_num(sv),
                "delta": (("+" if delta > 0 else "") + _fmt_num(delta)),
                "unit": unit,
                "trend": trend["text"],
                "trend_cls": trend["cls"],
                "rank_value": abs(delta) if abs(delta) > 1e-12 else abs(sv),
            })

    by_code: dict[str, list[dict]] = defaultdict(list)
    for r in rows_out:
        by_code[str(r.get("code") or "")].append(r)
    final: list[dict] = []
    for code, rows in by_code.items():
        grouped: dict[tuple, list[dict]] = defaultdict(list)
        for r in rows:
            grouped[_compare_detail_object_key(r)].append(r)
        if topn is None or len(grouped) <= topn:
            final.extend(rows)
            continue
        ranked = []
        for gkey, grows in grouped.items():
            ranked.append((max(float(x.get("rank_value") or 0) for x in grows), gkey))
        keep = {gkey for _, gkey in sorted(ranked, key=lambda x: x[0], reverse=True)[:topn]}
        final.extend([r for r in rows if _compare_detail_object_key(r) in keep])
    for r in final:
        r.pop("rank_value", None)
    return final


def _build_chart_payload(
    groups: dict[str, list[dict]],
    compare_rows: list[dict],
    *,
    scope: str = _SCOPE_MACRO,
    cid: int | None = None,
    base_cid: int | None = None,
    base_groups: dict[str, list[dict]] | None = None,
) -> dict:
    modules = _split_by_module(groups)
    base_modules = _split_by_module(base_groups) if base_groups else {m: {} for m in MODULE_ORDER}
    road_bars = []
    for code in ROAD_ORDER:
        if code in _DISTRIBUTION_CODES:
            continue
        if code not in modules["road"]:
            continue
        p = _pick_primary_value(code, modules["road"][code])
        if p["num"] is not None:
            if code == "road_macro_average_speed" and _is_absurd_speed_kmh(p["num"]):
                continue
            road_bars.append({"name": p["label"], "value": round(p["num"], 4), "unit": p["unit"]})

    slow_density = []
    for code in ("slow_macro_walk_density", "slow_macro_bike_density"):
        if code in modules["slow"]:
            p = _pick_primary_value(code, modules["slow"][code])
            if p["num"] is not None:
                slow_density.append({"name": p["label"], "value": round(p["num"], 2)})

    base_slow_density = []
    for code in ("slow_macro_walk_density", "slow_macro_bike_density"):
        if code in base_modules["slow"]:
            p = _pick_primary_value(code, base_modules["slow"][code])
            if p["num"] is not None:
                base_slow_density.append({"name": p["label"], "value": round(p["num"], 2)})

    pt_bars = []
    for code in PT_ORDER:
        if code not in modules["pt"]:
            continue
        p = _pick_primary_value(code, modules["pt"][code])
        if p["num"] is not None:
            pt_bars.append({"name": p["label"], "value": round(p["num"], 2), "unit": p["unit"]})

    vc_pie = _extract_distribution_pie(
        modules["slow"].get("slow_macro_vc_distribution", []),
        code="slow_macro_vc_distribution",
    )
    base_vc_pie = _extract_distribution_pie(
        base_modules["slow"].get("slow_macro_vc_distribution", []),
        code="slow_macro_vc_distribution",
    ) if base_groups else []
    road_cap_pie = _extract_distribution_pie(
        modules["road"].get("road_macro_capacity_distribution", []),
        code="road_macro_capacity_distribution",
    )
    road_saturation_bar = list(road_cap_pie)
    road_congestion_bar: list[dict] = []
    if "road_macro_congestion_ratio" in modules["road"]:
        p_cong = _pick_primary_value(
            "road_macro_congestion_ratio", modules["road"]["road_macro_congestion_ratio"]
        )
        if p_cong.get("num") is not None:
            road_congestion_bar.append(
                {
                    "name": "拥堵里程比例",
                    "value": round(float(p_cong["num"]), 2),
                    "unit": p_cong.get("unit") or "%",
                }
            )

    compare_chart = []
    for c in compare_rows:
        if c.get("scheme") is None:
            continue
        if indicator_tier(str(c.get("code") or "")) != "macro":
            continue
        compare_chart.append({
            "name": c["label"],
            "base": c.get("base"),
            "scheme": c.get("scheme"),
            "delta": c.get("delta"),
        })

    kpis = []
    base_kpis = []
    for code in ("road_macro_tti", "road_macro_congestion_ratio", "road_macro_average_speed",
                 "pt_macro_pt_pop_coverage_500m", "slow_macro_walk_density", "slow_macro_bike_density"):
        mod = "road" if code.startswith("road_") else "pt" if code.startswith("pt_") else "slow"
        if code not in modules[mod]:
            continue
        p = _pick_primary_value(code, modules[mod][code])
        item = {
            "code": code,
            "label": p["label"],
            "value": _fmt_num(p["num"]) if p["num"] is not None else (p["text"] or "—"),
            "num": p["num"],
            "unit": p["unit"],
            "module": mod,
        }
        if base_groups and code in base_groups:
            bp = _pick_primary_value(code, base_groups[code])
            if bp.get("num") is not None and p.get("num") is not None:
                delta = float(p["num"]) - float(bp["num"])
                trend = _trend_for_delta(code, delta)
                item.update({
                    "compare_enabled": True,
                    "compare_base_label": f"基础 case{base_cid}",
                    "compare_scheme_label": f"普通 case{cid}",
                    "compare_base": _fmt_num(float(bp["num"])),
                    "compare_scheme": _fmt_num(float(p["num"])),
                    "compare_delta_signed": ("+" if delta > 0 else "") + _fmt_num(delta),
                    "compare_trend": trend["text"],
                    "compare_trend_cls": trend["cls"],
                })
                base_kpis.append({
                    "code": code,
                    "label": bp["label"],
                    "value": _fmt_num(bp["num"]),
                    "num": bp["num"],
                    "unit": bp["unit"],
                    "module": mod,
                })
        kpis.append(item)

    detail = []
    for code in sorted(groups.keys()):
        tier = indicator_tier(code)
        if scope in (_SCOPE_MACRO, "all") and tier == "macro":
            if code in _DISTRIBUTION_CODES:
                for r in preferred_macro_rows(groups[code], code):
                    num = r.get("result_value_num")
                    if num is None:
                        continue
                    bucket = row_item(r) or r.get("result_key") or "—"
                    if bucket in ("value", "overall", "全网汇总"):
                        continue
                    detail.append({
                        "code": code,
                        "label": f"{INDICATOR_LABELS.get(code, code)} · {bucket}",
                        "tier": TIER_LABEL.get(tier, "其他"),
                        "ctx": r.get("context_key") or "GLOBAL",
                        "key": r.get("result_key") or "",
                        "formula": _detail_formula_reference(code, r)[0],
                        "reference": _detail_formula_reference(code, r)[1],
                        "display": _display_value(r),
                        "module": _module_of_code(code),
                    })
                continue
            p = _pick_primary_value(code, groups[code])
            if p["num"] is None and not p.get("text"):
                continue
            disp = _fmt_num(p["num"]) if p["num"] is not None else (p["text"] or "—")
            if p["unit"] and p["num"] is not None:
                disp = f"{disp} {p['unit']}"
            primary_row = None
            if p["num"] is not None:
                for rr in groups[code]:
                    try:
                        if rr.get("result_value_num") is not None and abs(float(rr["result_value_num"]) - float(p["num"])) < 1e-9:
                            primary_row = rr
                            break
                    except Exception:
                        pass
            if primary_row is None and groups[code]:
                primary_row = groups[code][0]
            fml, ref = _detail_formula_reference(code, primary_row or {})
            detail.append({
                "code": code,
                "label": _detail_label(code, primary_row or {}) if primary_row else INDICATOR_LABELS.get(code, code),
                "tier": TIER_LABEL.get(tier, "其他"),
                "ctx": "GLOBAL",
                "key": p.get("key") or "",
                "formula": fml,
                "reference": ref,
                "display": disp,
                "module": _module_of_code(code),
            })
            continue
        for r in groups[code]:
            num = r.get("result_value_num")
            if num is not None and not is_plausible_value(
                code, float(num), (r.get("result_unit") or "")
            ):
                continue
            detail.append({
                "code": code,
                "label": _detail_label(code, r),
                "tier": TIER_LABEL.get(tier, "其他"),
                "ctx": r.get("context_key") or "",
                "key": r.get("result_key") or "",
                "item": row_item(r),
                "formula": _detail_formula_reference(code, r)[0],
                "reference": _detail_formula_reference(code, r)[1],
                "display": _display_value(r),
                "module": _module_of_code(code),
            })

    indicator_cards = _apply_card_comparison(
        _build_indicator_cards(groups, scope=scope),
        compare_rows,
        cid=cid,
        base_cid=base_cid,
    )
    compare_detail = _build_compare_detail_rows(
        base_groups,
        groups,
        scope=scope,
        topn=10,
    ) if base_groups and base_cid is not None else []
    compare_detail_query_all = _build_compare_detail_rows(
        base_groups,
        groups,
        scope=scope,
        topn=None,
    ) if base_groups and base_cid is not None else []
    meso_charts = _build_meso_micro_charts(groups) if scope != _SCOPE_MACRO else []
    base_meso_charts = _build_meso_micro_charts(base_groups) if base_groups and scope != _SCOPE_MACRO else []

    def _tier_count(mod_dict: dict[str, list]) -> dict[str, int]:
        macro = meso = 0
        for c in mod_dict:
            t = indicator_tier(c)
            if t == "macro":
                macro += 1
            elif t == "meso_micro":
                meso += 1
        return {"macro": macro, "meso_micro": meso, "total": len(mod_dict)}

    road_tc = _tier_count(modules["road"])
    slow_tc = _tier_count(modules["slow"])
    pt_tc = _tier_count(modules["pt"])

    return {
        "road_bars": road_bars,
        "slow_density": slow_density,
        "base_slow_density": base_slow_density,
        "pt_bars": pt_bars,
        "vc_pie": vc_pie,
        "base_vc_pie": base_vc_pie,
        "road_cap_pie": road_cap_pie,
        "road_saturation_bar": road_saturation_bar,
        "road_congestion_bar": road_congestion_bar,
        "compare": compare_chart,
        "kpis": kpis,
        "base_kpis": base_kpis,
        "detail": _limit_detail_rows_topn(detail, topn=10),
        "detail_query_all": detail,
        "compare_detail": compare_detail,
        "compare_detail_query_all": compare_detail_query_all,
        "indicator_cards": indicator_cards,
        "indicator_meta": {code: get_indicator_meta(code) for code in groups},
        "meso_charts": meso_charts,
        "base_meso_charts": base_meso_charts,
        "counts": {
            "road": road_tc["total"],
            "slow": slow_tc["total"],
            "pt": pt_tc["total"],
            "road_macro": road_tc["macro"],
            "road_meso_micro": road_tc["meso_micro"],
            "slow_macro": slow_tc["macro"],
            "slow_meso_micro": slow_tc["meso_micro"],
            "pt_macro": pt_tc["macro"],
            "pt_meso_micro": pt_tc["meso_micro"],
            "macro_total": road_tc["macro"] + slow_tc["macro"] + pt_tc["macro"],
            "meso_micro_total": road_tc["meso_micro"] + slow_tc["meso_micro"] + pt_tc["meso_micro"],
        },
    }


def _build_module_count_summary(counts: dict, *, scope: str = _SCOPE_MACRO, type_filter: str = "all") -> str:
    """各子系统诊断指标数量 — 文字说明（替代饼图）。"""
    cnt = counts or {}
    road_m, road_mm = cnt.get("road_macro") or 0, cnt.get("road_meso_micro") or 0
    slow_m, slow_mm = cnt.get("slow_macro") or 0, cnt.get("slow_meso_micro") or 0
    pt_m, pt_mm = cnt.get("pt_macro") or 0, cnt.get("pt_meso_micro") or 0
    macro_total = cnt.get("macro_total") or (road_m + slow_m + pt_m)
    meso_total = cnt.get("meso_micro_total") or (road_mm + slow_mm + pt_mm)
    scope_note = {
        _SCOPE_MACRO: "宏观",
        _SCOPE_MESO: "中微观",
        "all": "宏观+中微观",
    }.get(scope, "宏观")

    def line(mod: str, m: int, mm: int) -> str:
        if scope == _SCOPE_MACRO:
            return f"<strong>{mod}</strong>：宏观 {m} 项"
        if scope == _SCOPE_MESO:
            return f"<strong>{mod}</strong>：中微观 {mm} 项"
        parts = []
        if m:
            parts.append(f"宏观 {m} 项")
        if mm:
            parts.append(f"中微观 {mm} 项")
        return f"<strong>{mod}</strong>：" + ("，".join(parts) if parts else "暂无")

    module_defs = [
        ("motor", "机动车", road_m, road_mm),
        ("slow", "慢行", slow_m, slow_mm),
        ("pt", "公共交通", pt_m, pt_mm),
    ]
    if type_filter != "all":
        module_defs = [x for x in module_defs if x[0] == type_filter]
    lines = [line(name, m, mm) for _, name, m, mm in module_defs]
    if type_filter != "all":
        total = sum(m for _, _, m, _ in module_defs) if scope == _SCOPE_MACRO else (
            sum(mm for _, _, _, mm in module_defs) if scope == _SCOPE_MESO
            else sum(m + mm for _, _, m, mm in module_defs)
        )
    else:
        total = macro_total if scope == _SCOPE_MACRO else meso_total if scope == _SCOPE_MESO else macro_total + meso_total
    tail = "以下展示该交通方式的指标数值与参考说明。" if type_filter != "all" else "以下按交通方式分组展示各指标数值与参考说明。"
    return (
        f"本方案共纳入 <strong>{scope_note}</strong> 诊断指标 <strong>{total}</strong> 项。"
        + "；".join(lines)
        + f"。{tail}"
    )


CHART_LABELS = {
    "c_road_bar": "道路宏观指标柱状图",
    "c_road_cong": "拥堵里程比例图",
    "c_road_sat": "路段饱和度分布图",
    "c_road_cap": "机动车通行能力分布图",
    "c_road_eff": "运行效率指标图",
    "c_slow_den": "慢行网络密度图",
    "c_slow_vc": "慢行 V/C 分布图",
    "c_pt_bar": "公交覆盖指标图",
    "c_compare_bar": "改扩建对比柱状图",
    "c_compare_delta": "改扩建变化量图",
}

SECTION_META = {
    "s1": ("#s1", "机动车诊断"),
    "s2": ("#s2", "慢行诊断"),
    "s3": ("#s3", "公共交通诊断"),
    "s4": ("#s4", "改扩建效果对比"),
    "s6": ("#s6", "中微观指标图表"),
}

_TYPE_HINT = {"motor": "机动车", "slow": "慢行", "pt": "公交"}
_TYPE_TO_MOD = {"motor": "road", "slow": "slow", "pt": "pt"}


def _motor_flow_available(pipeline: dict | None) -> bool:
    pl = pipeline or {}
    return (pl.get("flow_rows") or 0) > 0 or (pl.get("road_vol_rows") or 0) > 0


def _build_availability(
    charts: dict,
    type_filter: str,
    has_compare: bool,
    *,
    scope: str = _SCOPE_MACRO,
    pipeline: dict | None = None,
) -> dict:
    """判定各图表/章节是否有数据；无数据图表不渲染，仅写入 message。"""
    cnt = charts.get("counts") or {}
    rb = charts.get("road_bars") or []
    eff = [x for x in rb if any(k in x.get("name", "") for k in _ROAD_EFF_MACRO_HINTS)]
    meso_charts = charts.get("meso_charts") or []
    include_meso = scope in (_SCOPE_MESO, "all")
    has_micro_eff = include_meso and any(
        c.get("code") in _ROAD_EFF_MICRO_CODES for c in meso_charts
    )
    compare = charts.get("compare") or []

    charts_avail: dict[str, dict] = {}

    def set_chart(cid: str, ok: bool, reason: str) -> None:
        charts_avail[cid] = {"ok": ok, "reason": reason, "label": CHART_LABELS.get(cid, cid)}

    def chart_relevant(cid: str) -> bool:
        """按报告交通方式过滤缺失提示，避免慢行报告显示机动车/公交占位告警。"""
        if type_filter == "all":
            return True
        if cid.startswith("c_road_"):
            return type_filter == "motor"
        if cid.startswith("c_slow_"):
            return type_filter == "slow"
        if cid.startswith("c_pt_"):
            return type_filter == "pt"
        if cid.startswith("c_compare_"):
            return has_compare
        return True

    set_chart(
        "c_road_bar",
        len(rb) > 0,
        "无道路宏观诊断指标数值，请先执行 review_road 或 diagnosis_road_macro",
    )
    set_chart(
        "c_road_cong",
        len(charts.get("road_congestion_bar") or []) > 0,
        "无拥堵里程比例数据（需分配流量后运行 diagnosis_road_macro）",
    )
    set_chart(
        "c_road_sat",
        len(charts.get("road_saturation_bar") or []) > 0,
        "无路段饱和度分布数据（需分配流量后运行 diagnosis_road_macro）",
    )
    set_chart(
        "c_road_cap",
        False,
        "已改用「路段饱和度分布」柱状图（c_road_sat）",
    )
    if len(eff) > 0:
        set_chart("c_road_eff", True, "")
    else:
        set_chart(
            "c_road_eff",
            False,
            "无 TTI、DTI、拥堵比例或平均速度等运行效率类指标",
        )
    set_chart(
        "c_slow_den",
        len(charts.get("slow_density") or []) > 0,
        "无步行/骑行网络密度指标，请先执行 diagnosis_slow_macro",
    )
    set_chart(
        "c_slow_vc",
        len(charts.get("vc_pie") or []) > 0,
        "无慢行 V/C 分布指标数据",
    )
    set_chart(
        "c_pt_bar",
        len(charts.get("pt_bars") or []) > 0,
        "无公交宏观诊断指标，请先执行 diagnosis_pt_macro",
    )

    if has_compare:
        set_chart(
            "c_compare_bar",
            len(compare) > 0,
            "基础方案与本方案无可对比的数值型宏观指标",
        )
        deltas = [c for c in compare if c.get("delta") is not None]
        set_chart(
            "c_compare_delta",
            len(deltas) > 0,
            "改扩建对比缺少可计算变化量的成对指标值",
        )

    sections: dict[str, dict] = {}
    active_mod = _TYPE_TO_MOD.get(type_filter) if type_filter != "all" else None
    for sid, mod_key in (("s1", "road"), ("s2", "slow"), ("s3", "pt")):
        n = cnt.get(mod_key) or 0
        if active_mod is not None and active_mod != mod_key:
            hint = _TYPE_HINT.get(type_filter, type_filter)
            sections[sid] = {
                "ok": True,
                "reasons": [],
                "skipped": True,
                "skip_reason": f"当前 param2.type={type_filter}，仅输出{hint}维度，本节未渲染",
            }
        elif n == 0:
            mod_title = {"road": "机动车", "slow": "慢行", "pt": "公交"}[mod_key]
            rpc_hint = {
                "road": "review_road / diagnosis_road_macro",
                "slow": "diagnosis_slow_macro",
                "pt": "diagnosis_pt_macro",
            }[mod_key]
            sections[sid] = {
                "ok": False,
                "reasons": [f"无{mod_title}诊断指标，请先执行 {rpc_hint} 或 diagnosis_*_micro/meso"],
            }
        else:
            sections[sid] = {"ok": True, "reasons": []}

    if has_compare:
        sec_ok = len(compare) > 0
        sections["s4"] = {
            "ok": sec_ok,
            "reasons": [] if sec_ok else ["改扩建对比数据不足：基础方案或本方案缺少可对比指标"],
        }

    if include_meso:
        sections["s6"] = {
            "ok": len(meso_charts) > 0,
            "reasons": [] if meso_charts else ["无中微观诊断指标或缺少可绘制的分段数值"],
        }
    else:
        sections["s6"] = {
            "ok": False,
            "reasons": ["当前 scope=macro，未输出中微观诊断（需显式 scope=meso_micro）"],
        }

    # 指定交通方式时，只把当前方式相关图表交给前端。这样 type=slow 时不会再出现
    # 机动车/公交图表的占位提示；type=motor/type=pt 同理。all 保持原有全量行为。
    if type_filter != "all":
        charts_avail = {
            cid: a for cid, a in charts_avail.items()
            if chart_relevant(cid)
        }

    missing_notes: list[str] = []
    report_warnings: list[str] = []
    for cid, a in charts_avail.items():
        if cid == "c_road_cap":
            # c_road_cap 是历史图表占位，已被 c_road_sat 替代；不作为用户可处理的问题输出。
            continue
        if not a["ok"]:
            note = f"{a['label']}（{a['reason']}）"
            missing_notes.append(note)
            report_warnings.append(note)
    for sid, s in sections.items():
        # 指定 type 时，非当前交通方式的章节是主动隐藏，不属于数据缺失，不写入提示。
        if s.get("skipped"):
            continue
        if not s["ok"]:
            title = SECTION_META[sid][1]
            for r in s["reasons"]:
                note = f"{title}（{r}）"
                if note not in report_warnings:
                    report_warnings.append(note)

    return {
        "charts": charts_avail,
        "sections": sections,
        "missing_notes": missing_notes,
        "report_warnings": report_warnings,
    }


def _build_result_message(
    *,
    report_kind: str,
    cid: int,
    indicator_count: int,
    row_count: int,
    base_cid: int | None,
    compare_count: int,
    missing_notes: list[str],
    report_warnings: list[str] | None = None,
) -> str:
    if report_kind == "base":
        msg = f"基础方案诊断指标报告生成成功（{indicator_count} 项指标，{row_count} 行）"
    else:
        msg = f"方案诊断指标报告生成成功（case{cid}，{indicator_count} 项指标，{row_count} 行）"
    if compare_count and base_cid is not None:
        msg += f"；与 case{base_cid} 双方共有可对比指标 {compare_count} 项"
    warns = report_warnings if report_warnings is not None else missing_notes
    if warns:
        msg += "；以下图表或章节因数据缺失未渲染：" + "；".join(warns[:8])
        if len(warns) > 8:
            msg += f" 等共 {len(warns)} 项"
    return msg


def _load_echarts_script() -> str:
    paths = [
        "/tmp/echarts.min.js",
        os.path.join(os.path.dirname(__file__), "echarts.min.js"),
    ]
    for p in paths:
        if os.path.isfile(p):
            with open(p, encoding="utf-8") as f:
                return f"<script>\n{f.read()}\n</script>"
    return '<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>'



import html as _html_mod


def _ai_md_to_html(md: str) -> str:
    lines = (md or "").replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out, para = [], []
    in_ul = False
    def esc(x): return _html_mod.escape(str(x or ""), quote=True)
    def flush_para():
        nonlocal para
        if para:
            out.append("<p>" + "<br>".join(esc(x) for x in para) + "</p>")
            para = []
    def close_ul():
        nonlocal in_ul
        if in_ul:
            out.append("</ul>"); in_ul = False
    for raw in lines:
        line = raw.strip()
        if not line:
            flush_para(); close_ul(); continue
        if line.startswith("#"):
            flush_para(); close_ul()
            level = "h4" if line.startswith("###") else "h3"
            out.append(f"<{level}>" + esc(line.lstrip('#').strip()) + f"</{level}>")
        elif line.startswith(("- ", "* ", "\u2022 ")):
            flush_para()
            if not in_ul:
                out.append("<ul>"); in_ul = True
            out.append("<li>" + esc(line[2:].strip()) + "</li>")
        else:
            para.append(line)
    flush_para(); close_ul()
    return "\n".join(out)


def _split_ai_md(md: str) -> dict[str, str]:
    current = "diagnosis"
    buf = {"diagnosis": [], "advice": []}
    for raw in (md or "").replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        title = raw.strip().lstrip("#").strip()
        if any(k in title for k in ("改善建议", "方案建议", "优化建议", "建议方案", "AI 建议")):
            current = "advice"; continue
        if any(k in title for k in ("问题诊断", "现状诊断", "智能诊断", "现状分析", "执行摘要")):
            if title.startswith("##") and "建议" not in title:
                current = "diagnosis"
            continue
        buf[current].append(raw)
    diagnosis = "\n".join(buf["diagnosis"]).strip()
    advice = "\n".join(buf["advice"]).strip()
    if not diagnosis and not advice:
        diagnosis = (md or "").strip()
    elif not advice and diagnosis:
        advice = diagnosis
        diagnosis = ""
    return {"diagnosis": diagnosis, "advice": advice}


def _ai_dialog_path(param2: dict, report_kind: str, pid: int, uid: int, cid: int) -> str:
    for key in ("ai_dialog_path", "ai_record_path", "ai_md_path", "ai_file", "dialog_path"):
        val = param2.get(key) if isinstance(param2, dict) else None
        if val:
            return str(val)
    suffix = "base_scheme" if report_kind == "base" else "normal_scheme"
    return f"/tmp/ai_dialog/project{pid}_user{uid}_case{cid}_{suffix}.md"


def _ai_dialog_html(param2: dict, report_kind: str, pid: int, uid: int, cid: int, logs: list[str] | None = None) -> str:
    path = _ai_dialog_path(param2 or {}, report_kind, pid, uid, cid)
    try:
        if not os.path.isfile(path):
            if logs is not None:
                logs.append(f"AI 对话记录未找到：{path}")
            return ""
        md = open(path, "r", encoding="utf-8", errors="ignore").read().strip()
        if not md:
            if logs is not None:
                logs.append(f"AI 对话记录为空：{path}")
            return ""
        blocks = [
            '<div id="s_ai_advice" class="section ai-section">'
            '<div class="sec-title"><span class="dot ds"></span>AI 智能诊断与改善建议</div>'
            '<div class="ai-card ai-card-green">' + _ai_md_to_html(md) + "</div></div>"
        ]
        if logs is not None:
            logs.append(f"已读取 AI 对话记录：{path}")
        return "\n".join(blocks)
    except Exception as exc:
        if logs is not None:
            logs.append(f"AI 对话记录读取失败：{path}；{exc}")
        return ""


HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>
__ECHARTS__
__LEAFLET_HEAD__
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Microsoft YaHei',SimSun,sans-serif;background:#f0f2f5;color:#222;font-size:14px}
.banner{background:linear-gradient(135deg,#1a3a5c,#2d6a9f 55%,#1e8bc3);color:#fff;padding:32px 48px 24px;
  display:flex;justify-content:space-between;align-items:flex-end;flex-wrap:wrap;gap:12px}
.banner h1{font-size:22px;letter-spacing:1px}
.banner .sub{font-size:12px;opacity:.8;margin-top:8px;line-height:1.7}
.banner .meta{font-size:11px;opacity:.7;text-align:right;line-height:1.9}
.nav{background:#fff;border-bottom:2px solid #e0e6ef;display:flex;padding:0 32px;position:sticky;top:0;z-index:50;
  box-shadow:0 2px 6px rgba(0,0,0,.06);flex-wrap:wrap}
.nav a{padding:12px 14px;font-size:13px;color:#555;text-decoration:none;border-bottom:3px solid transparent}
.nav a:hover{color:#2d6a9f;border-color:#2d6a9f}
.container{width:100%;max-width:1280px;margin:0 auto;padding:28px 20px;box-sizing:border-box}
.section{max-width:1280px;margin:0 auto 40px;box-sizing:border-box}
.sec-title{display:flex;align-items:center;gap:10px;font-size:17px;font-weight:700;color:#1a3a5c;
  margin-bottom:12px;padding-bottom:10px;border-bottom:2px solid #d0dde8}
.dot{width:5px;height:22px;border-radius:3px}
.dm{background:#2d6a9f}.ds{background:#27ae60}.dp{background:#e67e22}.dd{background:#c0392b}.dr{background:#8e44ad}
.desc{background:#fff;border-radius:8px;padding:14px 18px;margin-bottom:16px;font-size:13px;line-height:1.95;
  color:#444;border-left:4px solid #2d6a9f;box-shadow:0 1px 5px rgba(0,0,0,.05)}
.desc.green{border-color:#27ae60}.desc.orange{border-color:#e67e22}.desc.red{border-color:#c0392b}.desc.purple{border-color:#8e44ad}
.kpi-strip{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:14px}
.kpi-strip .kpi{flex:1 1 120px;min-width:110px;max-width:220px;padding:10px 12px;border-radius:8px}
.kpi-strip .kv{font-size:18px;line-height:1.2}
.kpi-strip .kl{font-size:10px;margin-top:3px;line-height:1.3}
.s0-section-head{margin:18px 0 8px}
.s0-section-head .sec-title{margin:0;padding-bottom:8px;border-bottom:2px solid #d0dde8;font-size:15px}
.module-count-text{font-size:12px;line-height:1.75;color:#555;margin:0 0 12px;padding:10px 14px;
  background:#f7f9fc;border-radius:8px;border-left:3px solid #8e44ad}
.kpi{flex:1;min-width:140px;background:#fff;border-radius:10px;padding:14px 16px;box-shadow:0 2px 7px rgba(0,0,0,.06);
  border-left:4px solid #2d6a9f}
.kpi.g{border-color:#27ae60}.kpi.o{border-color:#e67e22}.kpi.r{border-color:#c0392b}
.kv{font-size:22px;font-weight:700;color:#1a3a5c}.ku{font-size:11px;color:#888;margin-left:4px}
.kl{font-size:11px;color:#999;margin-top:4px}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:16px}
#s4_charts{grid-template-columns:1fr}
.section>.desc,.section>.g2,.section>.card,.section>.meso-grid{width:100%;box-sizing:border-box}
#s5,#s6{width:100%!important;max-width:1280px!important;margin-left:auto!important;margin-right:auto!important;box-sizing:border-box!important}
#kpi_row_slow{display:none!important}
#s5 .card,#s6 .desc,#s6 .meso-grid{width:100%!important;max-width:1280px!important;box-sizing:border-box!important}
.card{background:#fff;border-radius:10px;padding:16px 18px;box-shadow:0 2px 7px rgba(0,0,0,.06)}
.card h3{font-size:13px;color:#555;margin-bottom:8px;font-weight:600}
.cnote{font-size:11px;color:#aaa;margin-top:6px;line-height:1.6}
.dtable{width:100%;border-collapse:collapse;font-size:12px}
.dtable th{background:#2d6a9f;color:#fff;padding:8px 10px;text-align:left}
.dtable td{padding:7px 10px;border-bottom:1px solid #eef2f7}
.dtable tr:nth-child(even) td{background:#f7f9fc}
.reno-pre{background:#fff;border-radius:8px;padding:12px;font-size:12px;overflow:auto}
.report-alerts{background:#fff8e6;border:1px solid #e8d48b;border-radius:8px;padding:14px 18px;
  margin-bottom:20px;font-size:13px;line-height:1.85;color:#5c4a00}
.report-alerts ul{margin:8px 0 0 20px}
.chart-slot{margin-bottom:0}
.chart-unavail{font-size:13px;color:#7b241c;background:#fdecea;border:1px solid #f5b7b1;
  border-radius:6px;padding:16px;line-height:1.65;margin:8px 0;min-height:120px}
.ind-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px;margin-bottom:16px}
.ind-card{background:#fff;border-radius:8px;padding:10px 12px;box-shadow:0 1px 5px rgba(0,0,0,.05);
  border-left:3px solid #2d6a9f;font-size:12px}
.ind-card.slow{border-color:#27ae60}.ind-card.pt{border-color:#e67e22}
.ind-card .it{font-size:10px;color:#999;margin-bottom:4px}
.ind-card .iv{font-size:18px;font-weight:700;color:#1a3a5c}
.ind-card .iu{font-size:10px;color:#888;margin-left:3px}
.ind-card .ic{font-size:10px;color:#aaa;margin-top:4px}
.ind-card .cmp{margin-top:6px;padding-top:5px;border-top:1px dashed #e6ebf2;font-size:10px;color:#748091;line-height:1.55}
.ind-card .cmp b{color:#1a3a5c;font-weight:600}.ind-card .trend-good{color:#1f9d55;font-weight:700}.ind-card .trend-bad{color:#d64545;font-weight:700}.ind-card .trend-flat{color:#6b7280;font-weight:700}
.meso-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.pair-chart{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.pair-title{text-align:center;font-size:12px;font-weight:700;color:#1a3a5c;margin-bottom:4px}
footer{text-align:center;padding:18px;font-size:12px;color:#aaa;border-top:1px solid #e0e6ef;background:#fff}
@media(max-width:900px){.g2,.meso-grid{grid-template-columns:1fr}.banner{padding:20px}}
.ai-section{margin-bottom:28px}
.ai-card{background:#fff;border-radius:8px;padding:18px 22px;box-shadow:0 2px 7px rgba(0,0,0,.06);line-height:1.9;font-size:14px}
.ai-card h3{font-size:15px;color:#12385f;margin:8px 0 10px}.ai-card h4{font-size:14px;color:#12385f;margin:8px 0 8px}
.ai-card p{margin:8px 0}.ai-card ul{margin:8px 0 8px 22px}.ai-card li{margin:5px 0}
.ai-card-red{background:#fff1f0;border-left:4px solid #e74c3c}
.ai-card-green{background:#ecf9f1;border-left:4px solid #27ae60}
__MAP_CSS__
</style>
</head>
<body>
<div class="banner">
  <div>
    <h1>__TITLE__</h1>
    <div class="sub">__SUBTITLE__</div>
  </div>
  <div class="meta">项目名 __PROJECT_NAME__ · 项目 ID __PID__ · 用户 __UID__ · 方案 case__CID__<br>__DATA_SOURCES__<br>生成时间 __TS__</div>
</div>
<nav class="nav" id="nav"></nav>
<div class="container">
  <div id="report_alerts" class="report-alerts" style="display:none"></div>
  <div id="s0" class="section">
    <div class="sec-title"><span class="dot dd"></span>诊断总览与工程师评述</div>
    <div class="desc red" id="narr_exec"></div>
    <div class="s0-section-head">
      <div class="sec-title"><span class="dot dr"></span>全部诊断指标一览（按交通方式分组）</div>
    </div>
    <p class="module-count-text" id="module_count_text"></p>
    <div id="indicator_grid"></div>
  </div>
  __MAP_SECTION__
  __AI_DIALOG_SECTION__
  <div id="s1" class="section">
    <div class="sec-title"><span class="dot dm"></span>机动车宏观诊断</div>
    <div class="desc" id="narr_motor"></div>
    <div class="kpi-strip" id="kpi_row_road"></div>
    <div class="g2" id="s1_docx_charts">
      <div class="chart-slot card" id="slot_c_road_cong"><h3>拥堵里程比例</h3>
        <div id="c_road_cong" style="height:300px"></div>
        <p class="cnote">按 GB/T 29107 口径：中度+严重拥堵路段里程占全网机动车路段里程比例（%）</p></div>
      <div class="chart-slot card" id="slot_c_road_sat"><h3>路段饱和度分布</h3>
        <div id="c_road_sat" style="height:300px"></div>
        <p class="cnote">按 V/C 分档统计各档路段里程占比：畅通&lt;0.6、基本畅通 0.6~0.8、拥挤 0.8~1.0、严重≥1.0</p></div>
    </div>
    <div class="g2" id="s1_charts">
      <div class="chart-slot card" id="slot_c_road_bar"><h3>道路宏观指标对比</h3>
        <div id="c_road_bar" style="height:300px"></div></div>
      <div class="chart-slot card" id="slot_c_road_eff"><h3>运行效率指标（TTI / DTI / 拥堵比例）</h3>
        <div id="c_road_eff" style="height:300px"></div>
        <p class="cnote">TTI、DTI 越接近 1 越畅通；拥堵比例为 V/C&gt;0.8 或速度比&lt;0.5 的路段里程占比</p></div>
    </div>
  </div>
  <div id="s2" class="section">
    <div class="sec-title"><span class="dot ds"></span>慢行宏观诊断</div>
    <div class="desc green" id="narr_slow"></div>
    <div class="kpi-strip" id="kpi_row_slow"></div>
    <div class="g2" id="s2_charts">
      <div class="chart-slot card" id="slot_c_slow_den"><h3>步行 / 骑行网络密度</h3>
        <div id="c_slow_den" style="height:280px"></div></div>
      <div class="chart-slot card" id="slot_c_slow_vc"><h3>慢行路段 V/C 等级分布</h3>
        <div id="c_slow_vc" style="height:280px"></div></div>
    </div>
  </div>
  <div id="s3" class="section">
    <div class="sec-title"><span class="dot dp"></span>公共交通宏观诊断</div>
    <div class="desc orange" id="narr_pt"></div>
    <div class="kpi-strip" id="kpi_row_pt"></div>
    <div class="chart-slot card" id="slot_c_pt_bar"><h3>公交服务与人口覆盖指标</h3>
      <div id="c_pt_bar" style="height:300px"></div>
      <p class="cnote">含线路密度、缓冲区人口覆盖率与区域覆盖率；轨道 800 m 覆盖率反映轨道交通服务半径</p></div>
  </div>
  <div id="s6" class="section">
    <div class="sec-title"><span class="dot dr"></span>中微观指标分段图表</div>
    <div class="desc">按路段、线路或空间单元展示中微观诊断指标数值排名前 10 的对象（有数值才渲染图表）。</div>
    <div class="meso-grid" id="meso_chart_grid"></div>
  </div>
  <div id="s4" class="section" style="display:none">
    <div class="sec-title"><span class="dot dr"></span>改扩建效果对比</div>
    <div class="desc purple" id="narr_compare"></div>
    <div id="reno_box" style="display:none" class="desc purple"><strong>改扩建说明：</strong><pre class="reno-pre" id="reno_pre"></pre></div>
    <div class="g2" id="s4_charts">
      <div class="chart-slot card" id="slot_c_compare_bar"><h3>基础方案 vs 本方案 — 指标对比</h3>
        <div id="c_compare_bar" style="height:320px"></div></div>
      </div>
    </div>
  </div>
  <div id="s5" class="section">
    <div class="sec-title"><span class="dot dd"></span>诊断指标明细表</div>
    <div class="cnote">说明：宏观指标固定展示；中微观/微观指标默认展示 Top10，可输入路名、link_id、LINK_xxx、TAZ_xxx 或指标名称查询。完整诊断结果仍保存在数据库结果表中。</div>
    <div class="card" style="overflow:auto;margin-bottom:12px">
      <h3 style="margin:0 0 10px;font-size:14px;color:#123b63">宏观指标明细</h3>
      <table class="dtable"><thead id="detail_macro_head"><tr>
        <th>子系统</th><th>层级</th><th>指标</th><th>计算方法</th><th>参考范围</th><th>上下文</th><th>数值</th>
      </tr></thead><tbody id="detail_macro_body"></tbody></table>
    </div>
    <div class="card" style="overflow:auto">
      <h3 style="margin:0 0 10px;font-size:14px;color:#123b63">中微观/微观指标查询</h3>
      <div style="display:flex;gap:8px;align-items:center;margin:0 0 10px;flex-wrap:wrap">
        <select id="detail_indicator" style="min-width:260px;padding:8px 10px;border:1px solid #cfd8e3;border-radius:6px;font-size:13px;background:#fff">
          <option value="">全部指标</option>
        </select>
        <input id="detail_query" placeholder="输入路名 / link_id / LINK_xxx / TAZ_xxx / 小区编号查询" style="flex:1;min-width:280px;padding:8px 10px;border:1px solid #cfd8e3;border-radius:6px;font-size:13px" />
        <button id="detail_clear" type="button" style="padding:8px 12px;border:1px solid #cfd8e3;background:#fff;border-radius:6px;cursor:pointer">清空</button>
        <span id="detail_query_info" style="font-size:12px;color:#667085"></span>
      </div>
      <table class="dtable"><thead id="detail_head"><tr>
        <th>子系统</th><th>层级</th><th>指标</th><th>计算方法</th><th>参考范围</th><th>上下文</th><th>数值</th>
      </tr></thead><tbody id="detail_body"></tbody></table>
    </div>
  </div>
</div>
<footer>TNA 方案诊断指标报告 · 数据来源 diagnosis_indicator_result_rows · 仅供交通方案论证参考</footer>
<script>__MAP_JS__</script>
<script>
const D=__DATA_JSON__;
const AV=D.availability||{charts:{},sections:{}};
const modColor={road:'#2d6a9f',slow:'#27ae60',pt:'#e67e22'};
function ec(id){const el=document.getElementById(id);return el?echarts.init(el):null;}
function chartOk(id){return AV.charts[id]&&AV.charts[id].ok;}
function makePairBox(id,leftTitle,rightTitle){
  const el=document.getElementById(id); if(!el) return null;
  el.innerHTML='<div class="pair-chart"><div><div class="pair-title">'+leftTitle+'</div><div id="'+id+'_base" style="height:260px"></div></div><div><div class="pair-title">'+rightTitle+'</div><div id="'+id+'_scheme" style="height:260px"></div></div></div>';
  return {base:id+'_base',scheme:id+'_scheme'};
}
function renderSimpleBar(id, arr, unit, color){
  const inst=ec(id); if(!inst) return;
  inst.setOption({tooltip:{trigger:'axis'},grid:{left:8,right:12,top:12,bottom:36,containLabel:true},xAxis:{type:'category',data:(arr||[]).map(x=>x.name),axisLabel:{fontSize:10,rotate:15}},yAxis:{type:'value',name:unit||''},series:[{type:'bar',data:(arr||[]).map(x=>x.value),itemStyle:{color:color||'#27ae60'},barMaxWidth:36,label:{show:true,position:'top',fontSize:9}}]});
}
function renderPie(id, arr){
  const inst=ec(id); if(!inst) return;
  const data=(arr||[]).map(x=>{const v=Number(x.value||0);return v>=8?x:Object.assign({},x,{label:{show:false},labelLine:{show:false}});});
  inst.setOption({
    tooltip:{trigger:'item',formatter:'{b}: {c}%'},
    legend:{type:'scroll',orient:'horizontal',bottom:0,left:'center',itemWidth:10,itemHeight:8,textStyle:{fontSize:10}},
    series:[{type:'pie',radius:['35%','62%'],center:['50%','44%'],avoidLabelOverlap:true,data:data,
      label:{show:true,formatter:p=>p.percent>=8?(p.name+'\n'+p.percent.toFixed(1)+'%'):'',fontSize:10,overflow:'none'},
      labelLine:{show:true,length:8,length2:8}
    }]
  });
}
function hideNav(href){const a=document.querySelector('.nav a[href="'+href+'"]');if(a)a.style.display='none';}
function markChartUnavailable(chartId){
  const a=AV.charts[chartId]; if(!a||a.ok) return;
  const el=document.getElementById(chartId);
  if(el){
    el.outerHTML='<p class="chart-unavail">'+a.reason+'</p>';
    return;
  }
  const slot=document.getElementById('slot_'+chartId);
  if(slot) slot.remove();
}
function pruneGrid(gridId){
  const g=document.getElementById(gridId);
  if(g&&!g.children.length) g.style.display='none';
}
function hideSection(secId,navHref){
  const s=AV.sections[secId]; if(!s||(s.ok&&!s.skipped)) return;
  hideNav(navHref);
  const sec=document.getElementById(secId);
  if(sec) sec.style.display='none';
}
const modName={road:'机动车',slow:'慢行',pt:'公交'};
const kpiCls={road:'',slow:'g',pt:'o'};
const indCls={road:'',slow:'slow',pt:'pt'};

document.getElementById('narr_exec').innerHTML=D.narratives.exec||'';
document.getElementById('narr_motor').innerHTML=D.narratives.motor||'';
document.getElementById('narr_slow').innerHTML=D.narratives.slow||'';
document.getElementById('narr_pt').innerHTML=D.narratives.pt||'';

const warnings=D.report_warnings||[];
if(warnings.length){
  const box=document.getElementById('report_alerts');
  box.style.display='block';
  box.innerHTML='<strong>数据与图表说明</strong><ul>'+warnings.map(w=>'<li>'+w+'</li>').join('')+'</ul>';
}

const nav=document.getElementById('nav');
[{h:'#s0',t:'总览'},{h:'#s_maps',t:'空间分布'},{h:'#s_ai_advice',t:'AI 建议'},{h:'#s1',t:'机动车'},{h:'#s2',t:'慢行'},{h:'#s3',t:'公交'}]
.forEach(x=>{const a=document.createElement('a');a.href=x.h;a.textContent=x.t;nav.appendChild(a);});
if(AV.sections.s6&&AV.sections.s6.ok){
  const a6=document.createElement('a');a6.href='#s6';a6.textContent='中微观';nav.appendChild(a6);
}
if(D.has_compare){
  document.getElementById('s4').style.display='';
  const a=document.createElement('a');a.href='#s4';a.textContent='改扩建对比';nav.appendChild(a);
  document.getElementById('narr_compare').innerHTML=D.narratives.compare||'';
  if(D.renovation_summary&&Object.keys(D.renovation_summary).length){
    document.getElementById('reno_box').style.display='';
    document.getElementById('reno_pre').textContent=JSON.stringify(D.renovation_summary,null,2);
  }
}
const a2=document.createElement('a');a2.href='#s5';a2.textContent='明细';nav.appendChild(a2);

['c_road_cong','c_road_sat','c_road_bar','c_road_cap','c_road_eff','c_slow_den','c_slow_vc','c_pt_bar','c_compare_bar']
.forEach(markChartUnavailable);
['s1_docx_charts','s1_charts','s2_charts','s4_charts'].forEach(pruneGrid);
hideSection('s1','#s1');
hideSection('s2','#s2');
hideSection('s3','#s3');
if(D.has_compare) hideSection('s4','#s4');
hideSection('s6','#s6');

const mct=document.getElementById('module_count_text');
if(mct) mct.innerHTML=D.module_count_summary||'';

const ig=document.getElementById('indicator_grid');
const modTitle={road:'机动车交通',slow:'慢行交通',pt:'公共交通'};
const modDot={road:'dm',slow:'ds',pt:'dp'};
['road','slow','pt'].forEach(mod=>{
  const cards=(D.charts.indicator_cards||[]).filter(c=>c.module===mod);
  if(!cards.length) return;
  const title=document.createElement('div');
  title.className='sec-title';
  title.style.cssText='font-size:14px;margin:12px 0 8px';
  title.innerHTML='<span class="dot '+modDot[mod]+'"></span>'+modTitle[mod];
  ig.appendChild(title);
  const grid=document.createElement('div');
  grid.className='ind-grid';
  grid.style.marginBottom='18px';
  cards.forEach(c=>{
    const d=document.createElement('div');
    d.className='ind-card '+(indCls[c.module]||'');
    const unitText=((c.unit&&c.value!=='?')?c.unit:'');
    const cmpUnit=c.compare_unit||unitText||'';
    const cmpHtml=c.compare_enabled
      ? '<div class="cmp"><div><b>'+c.compare_scheme_label+'</b>&#65306;'+c.compare_scheme+(cmpUnit?' '+cmpUnit:'')+'</div>'
        +'<div><b>'+c.compare_base_label+'</b>&#65306;'+c.compare_base+(cmpUnit?' '+cmpUnit:'')+'</div>'
        +'<div><b>&#21464;&#21270;</b>&#65306;'+c.compare_delta_signed+(cmpUnit?' '+cmpUnit:'')+' &#183; <span class="trend-'+(c.compare_trend_cls||'flat')+'">'+(c.compare_trend||'&#20165;&#23545;&#27604;')+'</span></div></div>'
      : '';
    const mainValueHtml=c.compare_enabled ? '' : '<div class="iv">'+c.value+'<span class="iu">'+unitText+'</span></div>';
    d.innerHTML='<div class="it">'+c.tier_label+' &#183; '+c.label+'</div>'
      +mainValueHtml
      +cmpHtml
      +'<div class="ic">'+(c.footnote||c.formula||'')+'</div>';
    grid.appendChild(d);
  });
  ig.appendChild(grid);
});

(D.charts.kpis||[]).forEach(k=>{
  const kr=document.getElementById('kpi_row_'+k.module);
  if(!kr) return;
  const d=document.createElement('div');
  d.className='kpi '+(kpiCls[k.module]||'');
  const unit=k.unit||'';
  if(k.compare_enabled){
    d.innerHTML='<div class="kl">'+k.label+'</div>'
      +'<div class="cmp mini"><div><b>'+k.compare_scheme_label+'</b>：'+k.compare_scheme+(unit?' '+unit:'')+'</div>'
      +'<div><b>'+k.compare_base_label+'</b>：'+k.compare_base+(unit?' '+unit:'')+'</div>'
      +'<div><b>变化</b>：'+k.compare_delta_signed+(unit?' '+unit:'')+' · <span class="trend-'+(k.compare_trend_cls||'flat')+'">'+(k.compare_trend||'仅对比')+'</span></div></div>';
  }else{
    d.innerHTML='<div class="kv">'+k.value+'<span class="ku">'+unit+'</span></div><div class="kl">'+k.label+'</div>';
  }
  kr.appendChild(d);
});

const rcong=D.charts.road_congestion_bar||[];
if(chartOk('c_road_cong')&&rcong.length){
  ec('c_road_cong').setOption({
    tooltip:{trigger:'axis',formatter:p=>p[0].name+': '+p[0].value+(rcong[0].unit||'%')},
    grid:{left:24,right:24,top:24,bottom:24,containLabel:true},
    xAxis:{type:'category',data:rcong.map(x=>x.name),axisLabel:{fontSize:12}},
    yAxis:{type:'value',name:'%'},
    series:[{type:'bar',barWidth:'42%',data:rcong.map(x=>x.value),
      itemStyle:{color:'#c0392b'},label:{show:true,position:'top',formatter:p=>p.value+'%'}}]
  });
}
const rsat=D.charts.road_saturation_bar||[];
const satColors=['#27ae60','#2ecc71','#f39c12','#e74c3c'];
if(chartOk('c_road_sat')&&rsat.length){
  ec('c_road_sat').setOption({
    tooltip:{trigger:'axis',formatter:p=>p[0].name+': '+p[0].value+'%'},
    grid:{left:12,right:16,top:16,bottom:12,containLabel:true},
    xAxis:{type:'category',data:rsat.map(x=>x.name),axisLabel:{fontSize:11}},
    yAxis:{type:'value',name:'里程占比 %'},
    series:[{type:'bar',barMaxWidth:48,data:rsat.map((x,i)=>({value:x.value,
      itemStyle:{color:satColors[i%satColors.length]}})),
      label:{show:true,position:'top',formatter:p=>p.value+'%'}}]
  });
}
const rb=D.charts.road_bars||[];
if(chartOk('c_road_bar')){
  ec('c_road_bar').setOption({
    tooltip:{trigger:'axis'},grid:{left:10,right:20,top:10,bottom:10,containLabel:true},
    xAxis:{type:'category',data:rb.map(x=>x.name),axisLabel:{rotate:25,fontSize:10}},
    yAxis:{type:'value'},
    series:[{type:'bar',barMaxWidth:28,data:rb.map((x,i)=>({value:x.value,
      itemStyle:{color:['#1d6fa5','#2d9cdb','#e67e22','#c0392b','#8e44ad','#16a085','#d35400','#7f8c8d'][i%8]}})),
      label:{show:true,position:'top',fontSize:9}}]
  });
}
const eff=rb.filter(x=>/TTI|DTI|拥堵|速度/.test(x.name));
if(chartOk('c_road_eff')&&eff.length){
  ec('c_road_eff').setOption({
    tooltip:{trigger:'axis'},grid:{left:10,right:10,top:10,bottom:10,containLabel:true},
    xAxis:{type:'category',data:eff.map(x=>x.name),axisLabel:{fontSize:10}},
    yAxis:{type:'value'},
    series:[{type:'bar',barMaxWidth:36,data:eff.map(x=>x.value),
      itemStyle:{color:'#c0392b'},label:{show:true,position:'top'}}]
  });
}
const sd=D.charts.slow_density||[];
const bsd=D.charts.base_slow_density||[];
if(chartOk('c_slow_den')){
  if(bsd.length){
    const ids=makePairBox('c_slow_den','&#22522;&#30784; case'+(D.base_case_id||''),'&#26222;&#36890; case'+(D.case_id||''));
    if(ids){renderSimpleBar(ids.base,bsd,'km/km&#178;','#95a5a6');renderSimpleBar(ids.scheme,sd,'km/km&#178;','#27ae60');}
  }else{
    renderSimpleBar('c_slow_den',sd,'km/km&#178;','#27ae60');
  }
}
const vc=D.charts.vc_pie||[];
const bvc=D.charts.base_vc_pie||[];
if(chartOk('c_slow_vc')){
  if(bvc.length){
    const ids=makePairBox('c_slow_vc','&#22522;&#30784; case'+(D.base_case_id||''),'&#26222;&#36890; case'+(D.case_id||''));
    if(ids){renderPie(ids.base,bvc);renderPie(ids.scheme,vc);}
  }else{
    renderPie('c_slow_vc',vc);
  }
}
const ptb=D.charts.pt_bars||[];
if(chartOk('c_pt_bar')){
  ec('c_pt_bar').setOption({
    tooltip:{trigger:'axis'},grid:{left:10,right:10,top:10,bottom:40,containLabel:true},
    xAxis:{type:'category',data:ptb.map(x=>x.name),axisLabel:{rotate:18,fontSize:10}},
    yAxis:{type:'value'},
    series:[{type:'bar',barMaxWidth:32,data:ptb.map(x=>x.value),itemStyle:{color:'#e67e22'},
      label:{show:true,position:'top',fontSize:9}}]
  });
}
if(chartOk('c_compare_bar')&&(D.charts.compare||[]).length){
  const cmp=D.charts.compare;
  ec('c_compare_bar').setOption({
    tooltip:{trigger:'axis'},legend:{top:0},
    grid:{left:10,right:10,top:36,bottom:50,containLabel:true},
    xAxis:{type:'category',data:cmp.map(x=>x.name),axisLabel:{rotate:22,fontSize:9}},
    yAxis:{type:'value'},
    series:[
      {name:'基础方案',type:'bar',data:cmp.map(x=>x.base),itemStyle:{color:'#95a5a6'}},
      {name:'本方案',type:'bar',data:cmp.map(x=>x.scheme),itemStyle:{color:'#2d6a9f'}}
    ]
  });
}
const mesoGrid=document.getElementById('meso_chart_grid');
const baseMesoMap={};
(D.charts.base_meso_charts||[]).forEach(ch=>{baseMesoMap[ch.title]=ch;});
function renderMesoBar(id,ch,color){
  const inst=ec(id); if(!inst) return;
  inst.setOption({tooltip:{trigger:'axis'},grid:{left:14,right:16,top:8,bottom:92,containLabel:true},xAxis:{type:'category',data:ch.labels,axisLabel:{rotate:42,fontSize:8,interval:0,formatter:function(v){return String(v).length>14?String(v).slice(0,14)+'…':v;}}},yAxis:{type:'value',name:ch.unit||''},series:[{type:'bar',barMaxWidth:22,data:ch.values,itemStyle:{color:color||modColor[ch.module]||'#2d6a9f'},label:{show:ch.values.length<=8,position:'top',fontSize:8}}]});
}
(D.charts.meso_charts||[]).forEach(ch=>{
  const card=document.createElement('div');
  card.className='card chart-slot';
  const cid=ch.id;
  const bch=baseMesoMap[ch.title];
  if(bch){
    card.innerHTML='<h3>'+ch.title+' <span style="font-weight:400;color:#999;font-size:11px">('+ch.tier_label+' &#183; '+ch.rows+'&#34892;)</span></h3>'
      +'<div class="pair-chart"><div><div class="pair-title">&#22522;&#30784; case'+(D.base_case_id||'')+'</div><div id="'+cid+'_base" style="height:260px"></div></div><div><div class="pair-title">&#26222;&#36890; case'+(D.case_id||'')+'</div><div id="'+cid+'_scheme" style="height:260px"></div></div></div>';
    mesoGrid.appendChild(card);
    renderMesoBar(cid+'_base',bch,'#95a5a6');
    renderMesoBar(cid+'_scheme',ch,modColor[ch.module]||'#2d6a9f');
  }else{
    card.innerHTML='<h3>'+ch.title+' <span style="font-weight:400;color:#999;font-size:11px">('+ch.tier_label+' &#183; '+ch.rows+'&#34892;)</span></h3>'
      +'<div id="'+cid+'" style="height:260px"></div>';
    mesoGrid.appendChild(card);
    renderMesoBar(cid,ch,modColor[ch.module]||'#2d6a9f');
  }
});

const tb=document.getElementById('detail_body');
const th=document.getElementById('detail_head');
const mtb=document.getElementById('detail_macro_body');
const mth=document.getElementById('detail_macro_head');
const metaMap=D.charts.indicator_meta||{};
const compareDetail=D.charts.compare_detail||[];
const isCompareDetail=compareDetail.length>0;
const queryDetailRows=isCompareDetail?(D.charts.compare_detail_query_all||compareDetail):(D.charts.detail_query_all||D.charts.detail||[]);
const displayDetailRows=isCompareDetail?compareDetail:(D.charts.detail||[]);
function isMacroRow(r){return String(r.tier||'').includes('宏观')||String(r.tier||'').toLowerCase()==='macro';}
function rowSearchText(r){return [r.code,r.label,r.tier,r.module,r.object,r.ctx,r.key,r.item,r.formula,r.reference,r.base,r.scheme,r.delta,r.unit,r.display].map(x=>x||'').join(' ').toLowerCase();}
function detailHeadHtml(isCompare){return isCompare
  ? '<tr><th>&#23376;&#31995;&#32479;</th><th>&#23618;&#32423;</th><th>&#25351;&#26631;</th><th>&#35745;&#31639;&#26041;&#27861;</th><th>&#21442;&#32771;&#33539;&#22260;</th><th>&#22522;&#30784; case</th><th>&#26222;&#36890; case</th><th>&#21464;&#21270;</th></tr>'
  : '<tr><th>&#23376;&#31995;&#32479;</th><th>&#23618;&#32423;</th><th>&#25351;&#26631;</th><th>&#35745;&#31639;&#26041;&#27861;</th><th>&#21442;&#32771;&#33539;&#22260;</th><th>&#19978;&#19979;&#25991;</th><th>&#25968;&#20540;</th></tr>';}
function makeDetailTr(r,isCompare){
  const m=metaMap[r.code]||{};
  const tr=document.createElement('tr');
  if(isCompare){
    const unit=r.unit?(' '+r.unit):'';
    tr.innerHTML='<td>'+(modName[r.module]||r.module)+'</td><td>'+(r.tier||'')+'</td><td>'+r.label+'</td>'
      +'<td style="font-size:11px;max-width:220px">'+(r.formula||m.formula||'暂无说明')+'</td>'
      +'<td style="font-size:11px;color:#666">'+(r.reference||m.reference||'暂无参考')+'</td>'
      +'<td>'+r.base+unit+'</td><td>'+r.scheme+unit+'</td><td>'+r.delta+unit+'</td>';
  }else{
    tr.innerHTML='<td>'+(modName[r.module]||r.module)+'</td><td>'+(r.tier||'')+'</td><td>'+r.label+'</td>'
      +'<td style="font-size:11px;max-width:220px">'+(r.formula||m.formula||'暂无说明')+'</td>'
      +'<td style="font-size:11px;color:#666">'+(r.reference||m.reference||'暂无参考')+'</td>'
      +'<td>'+(r.ctx||'')+'</td><td>'+(r.display||'')+'</td>';
  }
  return tr;
}
function renderRows(body,rows,isCompare,emptyText){
  if(!body) return;
  body.innerHTML='';
  if(!rows.length){const tr=document.createElement('tr');tr.innerHTML='<td colspan="8" style="color:#667085;padding:12px">'+emptyText+'</td>';body.appendChild(tr);return;}
  rows.forEach(r=>body.appendChild(makeDetailTr(r,isCompare)));
}
if(mth) mth.innerHTML=detailHeadHtml(isCompareDetail);
if(th) th.innerHTML=detailHeadHtml(isCompareDetail);
const macroRows=queryDetailRows.filter(isMacroRow);
const microRows=queryDetailRows.filter(r=>!isMacroRow(r));
const displayMicroRows=displayDetailRows.filter(r=>!isMacroRow(r));
renderRows(mtb,macroRows,isCompareDetail,'暂无宏观明细');
const DEFAULT_DETAIL_LIMIT=50;
const qInput=document.getElementById('detail_query');
const qSelect=document.getElementById('detail_indicator');
const qClear=document.getElementById('detail_clear');
const qInfo=document.getElementById('detail_query_info');
function indicatorBaseLabel(r){return String(r.label||'').split(' · ')[0];}
function fillIndicatorOptions(){
  if(!qSelect) return;
  const seen=new Set();
  microRows.forEach(r=>{
    const code=String(r.code||'');
    const label=indicatorBaseLabel(r)||code;
    const key=code||label;
    if(!key||seen.has(key)) return;
    seen.add(key);
    const opt=document.createElement('option');
    opt.value=key;
    opt.textContent=label;
    qSelect.appendChild(opt);
  });
}
function matchIndicator(r,sel){
  if(!sel) return true;
  return String(r.code||'')===sel || indicatorBaseLabel(r)===sel || String(r.label||'').indexOf(sel)>=0;
}
function renderQuery(){
  const q=(qInput&&qInput.value||'').trim().toLowerCase();
  const sel=qSelect&&qSelect.value||'';
  let rows=microRows.filter(r=>matchIndicator(r,sel));
  if(q) rows=rows.filter(r=>rowSearchText(r).includes(q));
  const total=rows.length;
  const showRows=(q||sel)?rows:rows.slice(0,DEFAULT_DETAIL_LIMIT);
  renderRows(tb,showRows,isCompareDetail,q||sel?'未查询到匹配的中微观/微观指标':'暂无中微观/微观明细');
  if(qInfo){
    if(q||sel) qInfo.textContent='筛选结果 '+showRows.length+' / '+total+' 行';
    else qInfo.textContent='默认显示前 '+DEFAULT_DETAIL_LIMIT+' 条，共可查询 '+microRows.length+' 行';
  }
}
fillIndicatorOptions();
if(qSelect) qSelect.addEventListener('change',renderQuery);
if(qInput) qInput.addEventListener('input',renderQuery);
if(qClear) qClear.addEventListener('click',()=>{if(qInput) qInput.value=''; if(qSelect) qSelect.value=''; renderQuery();});
renderQuery();
window.addEventListener('resize',()=>document.querySelectorAll('[id^="c_"],[id^="meso_"]').forEach(el=>{
  const i=echarts.getInstanceByDom(el);if(i)i.resize();
}));
</script>
</body>
</html>"""


def _compare_global(base_groups: dict, scheme_groups: dict) -> list[dict]:
    """改扩建对比：双方均有的指标，且均有可对比代表值。

    注意：这里生成的 compare_rows 同时供“指标卡片”和“对比图表”使用。
    卡片需要覆盖宏观+中微观；对比图表为了避免过密，在 _build_chart_payload
    里再筛选宏观指标。
    """
    rows = []
    for code in sorted(set(base_groups) & set(scheme_groups)):
        # Use the same primary value as the indicator cards. Otherwise V/C cards
        # may be compared by flow rows, and distribution cards may be compared by
        # a non-card bucket, causing misleading "degraded after capacity expansion".
        b_pick = _pick_card_primary_value(code, base_groups[code])
        s_pick = _pick_card_primary_value(code, scheme_groups[code])
        b = b_pick.get("num")
        s = s_pick.get("num")
        if b is None or s is None:
            continue
        rows.append(
            {
                "code": code,
                "label": INDICATOR_LABELS.get(code, code),
                "base": b,
                "scheme": s,
                "delta": s - b,
            }
        )
    return rows


def _render_html(
    *,
    report_kind: str,
    pid: int,
    uid: int,
    cid: int,
    project_name: str,
    base_cid: int | None,
    groups: dict[str, list[dict]],
    compare_rows: list[dict],
    renovation_summary: dict,
    base_groups: dict[str, list[dict]] | None = None,
    type_filter: str,
    scope: str = _SCOPE_MACRO,
    conn=None,
    map_logs: list[str] | None = None,
    pipeline: dict | None = None,
    param2: dict | None = None,
) -> str:
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    prefix = _prefix(pid, uid, cid)
    title = (
        f"{project_name} — 基础方案诊断指标报告"
        if report_kind == "base"
        else f"{project_name} — 方案诊断指标报告"
    )
    scope_note = {
        _SCOPE_MACRO: "宏观诊断指标",
        _SCOPE_MESO: "中微观诊断指标",
        "all": "宏观+中微观诊断指标",
    }.get(scope, "宏观诊断指标")
    subtitle = f"{scope_note} · {type_cn(type_filter)}"
    data_src = data_sources_line(tables_for_diagnosis(type_filter))
    if report_kind == "scheme" and base_cid is not None:
        title += f"（对比基础方案 case{base_cid}）"
        subtitle += f" · 改扩建对比基准 case{base_cid}"

    modules = _split_by_module(groups)
    narratives = _build_narratives(
        modules, report_kind, cid, base_cid, compare_rows, pipeline=pipeline, type_filter=type_filter
    )
    if compare_rows and base_cid is not None:
        narratives["compare"] = (
            f"下图对比基础方案（case{base_cid}）与本方案（case{cid}）在相同口径下的宏观诊断指标。"
            "变化量为正表示本方案数值升高。对 TTI、DTI、拥堵比例等指标，<strong>降低通常代表改善</strong>。"
        )

    global _DETAIL_LINK_NAMES
    _DETAIL_LINK_NAMES = _load_link_name_map(conn, prefix)
    if base_cid is not None:
        _DETAIL_LINK_NAMES.update({k: v for k, v in _load_link_name_map(conn, _prefix(pid, uid, base_cid)).items() if k not in _DETAIL_LINK_NAMES})
    charts = _build_chart_payload(groups, compare_rows, scope=scope, cid=cid, base_cid=base_cid, base_groups=base_groups)
    availability = _build_availability(
        charts, type_filter, bool(compare_rows), scope=scope, pipeline=pipeline
    )
    data = {
        "narratives": narratives,
        "charts": charts,
        "availability": availability,
        "report_warnings": availability.get("report_warnings") or [],
        "has_compare": bool(compare_rows),
        "renovation_summary": renovation_summary or {},
        "pipeline": pipeline or {},
        "scope": scope,
        "module_count_summary": _build_module_count_summary(
            charts.get("counts") or {}, scope=scope, type_filter=type_filter
        ),
    }

    map_bundle: dict = {}
    compare_layers: list[dict] = []
    if conn is not None:
        try:
            if report_kind == "scheme" and base_cid is not None:
                base_bundle, base_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [base_cid, 3], map_logs or [], type_filter=type_filter
                )
                scheme_bundle, scheme_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [cid, 3], map_logs or [], type_filter=type_filter
                )
                compare_layers = [
                    {
                        "key": "base",
                        "label": f"基础方案 case{base_cid}",
                        "case_id": base_cid,
                        "bundle": base_bundle,
                        "pipeline": base_meta.get("pipeline"),
                    },
                    {
                        "key": "scheme",
                        "label": f"本方案 case{cid}",
                        "case_id": cid,
                        "bundle": scheme_bundle,
                        "pipeline": scheme_meta.get("pipeline"),
                    },
                ]
                if map_logs is not None and base_meta.get("map_note"):
                    map_logs.append(f"地图基础方案: {base_meta['map_note']}")
                if map_logs is not None and scheme_meta.get("map_note"):
                    map_logs.append(f"地图本方案: {scheme_meta['map_note']}")
            elif report_kind == "base" and not _motor_flow_available(pipeline):
                ref_cid = 3 if int(cid) != 3 else 2
                cur_bundle, cur_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [cid, ref_cid], map_logs or [], type_filter=type_filter
                )
                ref_bundle, ref_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [ref_cid, cid], map_logs or [], type_filter=type_filter
                )
                compare_layers = [
                    {
                        "key": "current",
                        "label": f"本方案 case{cid}",
                        "case_id": cid,
                        "bundle": cur_bundle,
                        "pipeline": cur_meta.get("pipeline"),
                    },
                    {
                        "key": "reference",
                        "label": f"参考方案 case{ref_cid}（分配成功示意）",
                        "case_id": ref_cid,
                        "bundle": ref_bundle,
                        "pipeline": ref_meta.get("pipeline"),
                    },
                ]
                if map_logs is not None:
                    map_logs.append(
                        f"基础方案 case{cid} 无分配流量，空间图与参考方案 case{ref_cid} 并排对比（统一 V/C 口径）"
                    )
            else:
                map_bundle, map_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [cid, 3], map_logs or [], type_filter=type_filter
                )
                if map_logs is not None and map_meta.get("map_note"):
                    map_logs.append(map_meta["map_note"])
        except Exception as exc:
            if map_logs is not None:
                map_logs.append(f"地图采集失败: {exc}")

    if compare_layers:
        if report_kind == "scheme":
            map_title = "空间分布：路网流量对比与 OD 期望线"
        else:
            map_title = "空间分布：路网流量对比与 OD 期望线"
        map_render = render_map_sections(
            None,
            compare_layers=compare_layers,
            section_title=map_title,
        )
    else:
        map_title = f"空间分布：路网流量与 OD 期望线"
        map_render = render_map_sections(
            map_bundle,
            pipeline=pipeline,
            section_title=map_title,
        )

    html = HTML_TEMPLATE
    html = html.replace("__ECHARTS__", _load_echarts_script())
    html = html.replace("__LEAFLET_HEAD__", map_render.get("leaflet_head") or "")
    html = html.replace("__MAP_CSS__", map_render.get("css") or "")
    html = html.replace("__MAP_SECTION__", map_render.get("html") or "")
    html = html.replace("__MAP_JS__", map_render.get("js") or "")
    html = html.replace("__TITLE__", title)
    html = html.replace("__SUBTITLE__", subtitle)
    html = html.replace("__PROJECT_NAME__", project_name)
    html = html.replace("__DATA_SOURCES__", data_src)
    html = html.replace("__PID__", str(pid))
    html = html.replace("__UID__", str(uid))
    html = html.replace("__CID__", str(cid))
    html = html.replace("__PREFIX__", prefix)
    html = html.replace("__TS__", ts)
    ai_dialog_section = _ai_dialog_html(param2 or {}, report_kind, pid, uid, cid, map_logs)
    html = html.replace("__AI_DIALOG_SECTION__", ai_dialog_section or "")
    html = html.replace("__DATA_JSON__", json.dumps(data, ensure_ascii=False))
    return html, availability


def _build_response_kv(
    *,
    stage: str,
    msg: str,
    html_path: str,
    prefix: str,
    row_count: int,
    indicator_count: int,
    base_cid: int | None,
    cid: int,
    logs: list[str],
    html: str,
    include_html: bool,
    report_warnings: list[str] | None = None,
) -> dict:
    payload = {
        "code": 1,
        "message": msg,
        "summary.stage": stage,
        "summary.attr.html_path": html_path,
        "summary.attr.table_prefix": prefix,
        "summary.attr.case_id": str(cid),
        "data.table.diagnosis_result": _tbl(prefix, "diagnosis_indicator_result_rows"),
        "data.table.html_report": html_path,
        "data.count.diag_rows": str(row_count),
        "data.count.indicator_codes": str(indicator_count),
    }
    if report_warnings:
        payload["summary.attr.report_warnings"] = json.dumps(report_warnings, ensure_ascii=False)
        payload["summary.attr.skipped_chart_count"] = str(
            sum(1 for w in report_warnings if "图" in w or "柱状" in w or "雷达" in w or "饼图" in w)
        )
    if base_cid is not None:
        payload["summary.attr.base_case_id"] = str(base_cid)
        payload["summary.attr.has_renovation_compare"] = "true"
    for i, line in enumerate(logs[-40:]):
        payload[f"summary.log.{i}"] = line
    if include_html:
        payload["summary.attr.content"] = html
    return payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report-kind", required=True, choices=("base", "scheme"))
    parser.add_argument("--request-file", required=True)
    parser.add_argument("--response-file", required=True)
    parser.add_argument("--progress-file", default="")
    args = parser.parse_args()

    kind = args.report_kind
    stage = "base_scheme_diagnosis_report" if kind == "base" else "scheme_diagnosis_report"
    req = read_request(args.request_file)
    pid = int(req.get("project_id", 0))
    uid = int(req.get("user_id", 0))
    cid = int(req.get("case_id", 0))
    param1 = req.get("param1", "")
    param2 = parse_report_param2(req.get("param2"))

    if pid <= 0 or uid <= 0:
        write_response(args.response_file, {"code": -1, "message": "project_id 与 user_id 须为正整数"})
        return
    if cid < 0:
        write_response(
            args.response_file,
            {"code": -1, "message": "case_id 须 >= 0（当前方案 ID，基础方案与普通方案规则相同）"},
        )
        return

    type_filter = (param2.get("type") or "all").strip().lower()
    scope = (param2.get("scope") or _SCOPE_MACRO).strip().lower()
    base_cid = param2.get("base_case_id")
    if base_cid is not None and base_cid != "":
        base_cid = int(base_cid)
    else:
        base_cid = None if kind == "base" else base_cid
    renovation_summary = param2.get("renovation_summary") or {}
    include_html = param2.get("include_html_content", True)
    if isinstance(include_html, str):
        include_html = include_html.strip().lower() not in ("0", "false", "no")

    if kind == "base":
        fallback_path = f"/tmp/tna_base_scheme_diag_report_{pid}_{uid}_case{cid}.html"
        report_stem = f"base_scheme_diagnosis_report_case{cid}"
    else:
        suffix = f"_vs_case{base_cid}" if base_cid is not None else ""
        fallback_path = f"/tmp/tna_scheme_diag_report_{pid}_{uid}_case{cid}{suffix}.html"
        report_stem = f"scheme_diagnosis_report_case{cid}{suffix}"
    output_path = resolve_report_output_path(
        param2,
        report_stem=report_stem,
        fallback_path=fallback_path,
    )

    logs: list[str] = []
    try:
        write_progress(args.progress_file, 5, "连接数据库...")
        conn = _get_conn(param1)
        prefix = _prefix(pid, uid, cid)
        write_progress(args.progress_file, 30, "读取诊断指标结果...")
        rows = _filter_rows(_load_rows(conn, prefix), type_filter, scope)
        if not rows:
            write_response(
                args.response_file,
                {
                    "code": -2,
                    "message": f"诊断结果表为空或不存在：user_project.{prefix}diagnosis_indicator_result_rows；请先运行 review_road 或 diagnosis_*_macro",
                    "summary.stage": stage,
                },
            )
            conn.close()
            return

        groups = group_by_indicator(rows)
        compare_rows: list[dict] = []
        base_groups_for_compare: dict[str, list[dict]] | None = None
        if kind == "scheme" and base_cid is not None:
            write_progress(args.progress_file, 55, "加载基础方案对比数据...")
            base_prefix = _prefix(pid, uid, base_cid)
            base_rows = _filter_rows(_load_rows(conn, base_prefix), type_filter, scope)
            base_groups_for_compare = group_by_indicator(base_rows)
            compare_rows = _compare_global(base_groups_for_compare, groups)
            logs.append(f"改扩建对比：base_case_id={base_cid}，对比指标 {len(compare_rows)} 项")

        write_progress(args.progress_file, 85, "生成 HTML 报告...")
        pipeline = _fetch_pipeline_status(conn, prefix, type_filter)
        logs.append(
            f"分配状态：flow_rows={pipeline.get('flow_rows')} road_vol_rows={pipeline.get('road_vol_rows')}"
        )
        project_label = project_display_name(param2, pid)
        html, availability = _render_html(
            report_kind=kind,
            pid=pid,
            uid=uid,
            cid=cid,
            project_name=project_label,
            base_cid=base_cid,
            groups=groups,
            compare_rows=compare_rows,
            renovation_summary=renovation_summary if kind == "scheme" else {},
            base_groups=base_groups_for_compare,
            type_filter=type_filter,
            scope=scope,
            conn=conn,
            map_logs=logs,
            pipeline=pipeline,
            param2=param2,
        )
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(html)

        missing_notes = availability.get("missing_notes") or []
        report_warnings = availability.get("report_warnings") or []
        if missing_notes:
            logs.append("未渲染图表/章节：" + "；".join(missing_notes[:6]))

        msg = _build_result_message(
            report_kind=kind,
            cid=cid,
            indicator_count=len(groups),
            row_count=len(rows),
            base_cid=base_cid,
            compare_count=len(compare_rows),
            missing_notes=missing_notes,
            report_warnings=report_warnings,
        )

        write_progress(args.progress_file, 99, "完成")
        write_response(
            args.response_file,
            _build_response_kv(
                stage=stage,
                msg=msg,
                html_path=output_path,
                prefix=prefix,
                row_count=len(rows),
                indicator_count=len(groups),
                base_cid=base_cid,
                cid=cid,
                logs=logs,
                html=html,
                include_html=include_html,
                report_warnings=report_warnings,
            ),
        )
        conn.close()
    except Exception:
        err = traceback.format_exc()
        write_response(
            args.response_file,
            {
                "code": -99,
                "message": f"诊断指标报告生成失败：{err.splitlines()[-1] if err else '未知错误'}",
                "summary.stage": stage,
            },
        )


if __name__ == "__main__":
    main()







