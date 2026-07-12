"""
慢行交通中观与微观诊断指标计算模块

中观指标（片区级别，以 TAZ 为单元）：
  slow_meso_walk_density    — 片区步行设施密度
  slow_meso_bike_density    — 片区非机动车道密度
  slow_meso_walk_continuity — 片区步行设施连续性
  slow_meso_bike_continuity — 片区非机动车道连续性
  slow_meso_area_percap     — 片区人均慢行道路面积

微观指标（路段级别）：
  slow_micro_vc             — 全路段饱和度诊断（所有路段）
  slow_micro_topn_vc        — TOP-N 高负荷路段排名
  slow_micro_capacity_impact_topn — TOP-N 通行能力影响路段

中观指标使用 PostGIS 空间 SQL（ST_Intersects）将慢行路段与 TAZ 多边形关联；
微观指标使用 pandas 处理分配结果。
"""

from pathlib import Path
import sys

import pandas as pd
from psycopg2 import sql

ROOT_DIR = Path(__file__).resolve().parents[1]
if str(ROOT_DIR) not in sys.path:
    sys.path.append(str(ROOT_DIR))

from diagnostic_db_common import DEFAULT_RESULT_TABLE_RAW_NAME
from diagnostic_db_common import build_data_missing_json
from diagnostic_db_common import build_error_json
from diagnostic_db_common import build_inputs_echo
from diagnostic_db_common import build_success_json
from diagnostic_db_common import connect_db
from diagnostic_db_common import fetch_dataframe
from diagnostic_db_common import load_cfg
from diagnostic_db_common import make_result_row
from diagnostic_db_common import resolve_runtime_table_name
from diagnostic_db_common import safe_progress
from diagnostic_db_common import split_table_name
from diagnostic_db_common import write_indicator_rows


INPUT_TABLES = {
    "slow_road_way_table": {
        "raw_name": "slow_road_way",
        "required_columns": ["link_id", "type", "length", "lane_num", "init_node", "term_node"],
    },
    "slow_flow_table": {
        "raw_name": "slow_greedy_link_flow_results",
        "required_columns": ["link_id", "flow", "travel_time", "capacity"],
    },
    "taz_table": {
        "raw_name": "other_taz_socioeconomic",
        "required_columns": ["area", "pop"],
    },
}

INDICATOR_SPECS = {
    # ── 中观 ──────────────────────────────────────────────────
    "slow_meso_walk_density": {
        "indicator_name": "片区步行设施密度",
        "unit": "km/km²",
        "scope": "meso",
        "mode": "spatial_density",
        "type_key": "walk",
    },
    "slow_meso_bike_density": {
        "indicator_name": "片区非机动车道密度",
        "unit": "km/km²",
        "scope": "meso",
        "mode": "spatial_density",
        "type_key": "bike",
    },
    "slow_meso_walk_continuity": {
        "indicator_name": "片区步行设施连续性",
        "unit": "km",
        "scope": "meso",
        "mode": "meso_continuity",
        "type_key": "walk",
    },
    "slow_meso_bike_continuity": {
        "indicator_name": "片区非机动车道连续性",
        "unit": "km",
        "scope": "meso",
        "mode": "meso_continuity",
        "type_key": "bike",
    },
    "slow_meso_area_percap": {
        "indicator_name": "片区人均慢行道路面积",
        "unit": "m²/人",
        "scope": "meso",
        "mode": "spatial_area_percap",
        "type_key": "all",
    },
    # ── 微观 ──────────────────────────────────────────────────
    "slow_micro_vc": {
        "indicator_name": "慢行路段饱和度（全路段）",
        "unit": "",
        "scope": "micro",
        "mode": "micro_vc_all",
        "needs_flow": True,
    },
    "slow_micro_topn_vc": {
        "indicator_name": "TOP-N 高负荷慢行路段",
        "unit": "",
        "scope": "micro",
        "mode": "micro_topn_vc",
        "needs_flow": True,
    },
    "slow_micro_capacity_impact_topn": {
        "indicator_name": "TOP-N 慢行通行能力影响路段",
        "unit": "",
        "scope": "micro",
        "mode": "micro_capacity_impact_topn",
        "needs_flow": True,
    },
}

FLOW_SKIP_REASON = (
    "slow_greedy_link_flow_results 为空或无可关联流量，"
    "本指标已跳过（图表/数值留空）；请先运行 base_slow_network 生成分配流量。"
)


def _flow_merge_usable(df_network: pd.DataFrame, df_flow: pd.DataFrame) -> tuple[bool, int, int]:
    flow_rows = 0 if df_flow is None else len(df_flow)
    if flow_rows == 0:
        return False, flow_rows, 0
    merged = pd.merge(df_network, df_flow, on="link_id", how="inner")
    return len(merged) > 0, flow_rows, len(merged)


def _table_column_exists(conn, schema: str | None, table: str, column: str) -> bool:
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT 1
            FROM information_schema.columns
            WHERE table_schema = COALESCE(%s, current_schema())
              AND table_name = %s
              AND column_name = %s
            LIMIT 1
            """,
            (schema, table, column),
        )
        return cur.fetchone() is not None


def _pick_taz_id_column(conn, schema: str | None, table: str) -> str:
    """Choose a real zone id column before composing SQL."""
    for col in ("id", "area_id", "taz_id", "zone_id", "objectid", "gid"):
        if _table_column_exists(conn, schema, table, col):
            return col
    raise RuntimeError(f"TAZ/小区表 {schema + '.' if schema else ''}{table} 缺少可用 ID 字段（id/area_id/taz_id/zone_id/objectid/gid）")


def _filter_micro_network(df_network: pd.DataFrame, link_ids, bbox, logs: list) -> pd.DataFrame:
    if link_ids is not None:
        id_set = set(int(x) for x in link_ids)
        df_network = df_network[df_network["link_id"].isin(id_set)]
        if df_network.empty:
            raise ValueError(f"link_ids 未找到任何匹配的慢行路段: {sorted(id_set)}")
        logs.append(f"link_ids filter: matched={len(df_network)}, requested={sorted(id_set)}")
    elif bbox is not None:
        w, s, e, n = bbox
        if "longitude" in df_network.columns and "latitude" in df_network.columns:
            df_network = df_network[
                df_network["longitude"].between(w, e) & df_network["latitude"].between(s, n)
            ]
            logs.append(f"bbox filter: matched={len(df_network)}, bbox={bbox}")
            if df_network.empty:
                raise ValueError(f"bbox {bbox} 范围内未找到任何慢行路段")
    return df_network


def _filter_centroid_connectors(df_network: pd.DataFrame, logs: list) -> pd.DataFrame:
    """Exclude centroid connector links from slow micro indicators and charts."""
    if df_network is None or df_network.empty:
        return df_network
    mask = pd.Series(False, index=df_network.index)
    if "type" in df_network.columns:
        type_num = pd.to_numeric(df_network["type"], errors="coerce")
        mask = mask | (type_num == 10)
    if "capacity" in df_network.columns:
        cap_num = pd.to_numeric(df_network["capacity"], errors="coerce")
        mask = mask | (cap_num >= 9_000_000)
    if "centroid_matched_node" in df_network.columns:
        centroid = df_network["centroid_matched_node"]
        mask = mask | centroid.notna() & (centroid.astype(str).str.strip() != "")
    removed = int(mask.sum())
    if removed:
        df_network = df_network.loc[~mask].copy()
        logs.append(f"centroid connectors filtered: removed={removed}, remaining={len(df_network)}")
    return df_network


def _skip_micro_flow_missing(
    conn,
    result_table,
    indicator_code,
    spec,
    input_tables,
    db,
    logs,
    on_progress,
    flow_rows,
    merged_rows,
):
    logs.append(f"skip: flow_unavailable flow_rows={flow_rows} merged_rows={merged_rows}")
    written = write_indicator_rows(
        conn, result_table, indicator_code, spec["indicator_name"], "GLOBAL", []
    )
    logs.append(f"write.result rows={written} (cleared, no flow data)")
    safe_progress(on_progress, 4, 100)
    flow_table = input_tables["slow_flow_table"]
    data = {
        "tables": {"result_table": result_table},
        "counts": {"written_rows": written, "skipped": True},
        "metrics": {
            "empty": True,
            "skipped": True,
            "skip_reason": FLOW_SKIP_REASON,
            "flow_rows": flow_rows,
            "merged_rows": merged_rows,
        },
        "result_rows": [],
    }
    inputs = build_inputs_echo(db, input_tables, result_table)
    skip_msg = (
        f"输入数据缺失：表 `{flow_table}` 无有效流量（flow_rows={flow_rows}，"
        f"merged_rows={merged_rows}），指标已跳过；请先运行 base_slow_network 生成分配流量。"
    )
    return build_data_missing_json(
        indicator_code,
        data,
        logs,
        inputs,
        message=skip_msg,
        missing_tables=[flow_table],
    )

# VC 等级划分
_VC_BINS   = [0, 0.6, 0.8, 0.9, float("inf")]
_VC_LABELS = ["畅通", "基本畅通", "拥挤", "严重拥堵"]


# ═══════════════════════════════════════════════════════════════
#  中观：空间 SQL 查询（PostGIS）
# ═══════════════════════════════════════════════════════════════

def _spatial_density_query(conn, slow_road_way_table: str, taz_table: str, type_codes: list) -> pd.DataFrame:
    """
    批量计算所有 TAZ 内的慢行设施密度（ST_Intersects）。

    Returns
    -------
    DataFrame with columns: taz_id, taz_area_km2, length_km, density
    """
    sw_schema, sw_table = split_table_name(slow_road_way_table)
    tz_schema, tz_table = split_table_name(taz_table)

    sw_ident = sql.Identifier(sw_schema, sw_table) if sw_schema else sql.Identifier(sw_table)
    tz_ident = sql.Identifier(tz_schema, tz_table) if tz_schema else sql.Identifier(tz_table)
    tz_id_col = _pick_taz_id_column(conn, tz_schema, tz_table)

    if type_codes:
        type_filter = sql.SQL("AND s.type = ANY(%s)")
        params = (type_codes,)
    else:
        type_filter = sql.SQL("")
        params = ()

    query = sql.SQL(
        "SELECT"
        "    z.{tz_id}                                     AS taz_id,"
        "    ST_Area(z.geometry::geography) / 1000000.0    AS taz_area_km2,"
        "    COALESCE(SUM(ST_Length(ST_Intersection(s.geometry, z.geometry)::geography) / 1000.0), 0)"
        "                                                  AS length_km,"
        "    COALESCE(SUM(ST_Length(ST_Intersection(s.geometry, z.geometry)::geography) / 1000.0), 0)"
        "        / NULLIF(ST_Area(z.geometry::geography) / 1000000.0, 0)"
        "                                                  AS density "
        "FROM {tz} z "
        "LEFT JOIN {sw} s"
        "    ON ST_Intersects(s.geometry, z.geometry) "
        "    {type_filter} "
        "GROUP BY z.{tz_id}, z.geometry "
        "ORDER BY z.{tz_id}"
    ).format(sw=sw_ident, tz=tz_ident, tz_id=sql.Identifier(tz_id_col), type_filter=type_filter)

    with conn.cursor() as cur:
        cur.execute(query, params or None)
        rows = cur.fetchall()
        cols = [d[0] for d in cur.description]
    return pd.DataFrame(rows, columns=cols)


def _spatial_area_percap_query(
    conn,
    slow_road_way_table: str,
    taz_table: str,
    walk_types: list,
    bike_types: list,
    walk_width: float,
    bike_width: float,
) -> pd.DataFrame:
    """
    批量计算各 TAZ 片区人均慢行道路面积。

    面积近似：length × lane_num × 车道宽度（m） × 1000（km→m）
    """
    sw_schema, sw_table = split_table_name(slow_road_way_table)
    tz_schema, tz_table = split_table_name(taz_table)
    sw_ident = sql.Identifier(sw_schema, sw_table) if sw_schema else sql.Identifier(sw_table)
    tz_ident = sql.Identifier(tz_schema, tz_table) if tz_schema else sql.Identifier(tz_table)
    tz_id_col = _pick_taz_id_column(conn, tz_schema, tz_table)

    all_types = list(set(walk_types + bike_types))
    if all_types:
        type_filter = sql.SQL("AND s.type = ANY(%s)")
        params: tuple = (all_types,)
    else:
        type_filter = sql.SQL("")
        params = ()

    # 车道宽度 CASE WHEN 动态构建
    if walk_types and bike_types:
        width_case = sql.SQL(
            "CASE WHEN s.type = ANY({wt}) THEN {ww}::double precision"
            "     ELSE {bw}::double precision END"
        ).format(
            wt=sql.Literal(walk_types),
            ww=sql.Literal(walk_width),
            bw=sql.Literal(bike_width),
        )
    else:
        default_width = walk_width if walk_types else bike_width
        width_case = sql.Literal(default_width)

    query = sql.SQL(
        "SELECT"
        "    z.{tz_id}                                        AS taz_id,"
        "    z.pop                                            AS population,"
        "    COALESCE(SUM(ST_Length(ST_Intersection(s.geometry, z.geometry)::geography)"
        "        * COALESCE(s.lane_num, 1) * {width_case}), 0)"
        "                                                     AS slow_area_m2,"
        "    COALESCE(SUM(ST_Length(ST_Intersection(s.geometry, z.geometry)::geography)"
        "        * COALESCE(s.lane_num, 1) * {width_case}), 0)"
        "        / NULLIF(z.pop, 0)                           AS area_per_capita_m2 "
        "FROM {tz} z "
        "LEFT JOIN {sw} s"
        "    ON ST_Intersects(s.geometry, z.geometry)"
        "    {type_filter} "
        "GROUP BY z.{tz_id}, z.pop "
        "ORDER BY z.{tz_id}"
    ).format(
        sw=sw_ident, tz=tz_ident,
        tz_id=sql.Identifier(tz_id_col),
        width_case=width_case,
        type_filter=type_filter,
    )

    with conn.cursor() as cur:
        cur.execute(query, params or None)
        rows = cur.fetchall()
        cols = [d[0] for d in cur.description]
    return pd.DataFrame(rows, columns=cols)


def _meso_continuity_pandas(df_network: pd.DataFrame, df_taz_ids: pd.Series, type_codes: list) -> dict:
    """
    利用 pandas 对每个 TAZ 计算路段连续性（节点度法）。

    注：此方法不做空间过滤，假设 slow_road_way 表已有 taz_id 字段；
    如无该字段，则降级为全网节点度统计（中观 fallback）。

    Returns
    -------
    dict: {taz_id -> {"dead_end_count": int, "isolated_segment_count": int, ...}}
    """
    if type_codes:
        df = df_network[df_network["type"].isin(type_codes)].copy()
    else:
        df = df_network.copy()

    results = {}
    if "taz_id" in df.columns:
        groups = df.groupby("taz_id")
    else:
        groups = [("GLOBAL", df)]

    for taz_id, gdf in groups:
        seg_count = len(gdf)
        total_km = float(gdf["length"].sum())
        if seg_count == 0:
            results[taz_id] = {"segment_count": 0, "total_length_km": 0.0, "dead_end_count": 0,
                                "isolated_segment_count": 0, "dead_end_ratio_pct": 0.0}
            continue
        all_nodes = pd.concat([gdf["init_node"].rename("n"), gdf["term_node"].rename("n")])
        degree = all_nodes.value_counts()
        dead_nodes = set(degree[degree == 1].index)
        isolated = gdf[gdf["init_node"].isin(dead_nodes) & gdf["term_node"].isin(dead_nodes)]
        total_nodes = len(degree)
        dead_end_ratio = round(len(dead_nodes) / total_nodes * 100, 2) if total_nodes > 0 else 0.0
        results[taz_id] = {
            "segment_count": seg_count,
            "total_length_km": round(total_km, 4),
            "dead_end_count": len(dead_nodes),
            "isolated_segment_count": len(isolated),
            "dead_end_ratio_pct": dead_end_ratio,
        }
    return results


# ═══════════════════════════════════════════════════════════════
#  微观：pandas 路段分析
# ═══════════════════════════════════════════════════════════════

def _calc_micro_vc(df_network: pd.DataFrame, df_flow: pd.DataFrame) -> pd.DataFrame:
    """
    合并路网与流量表，计算每条路段的 V/C、TTI 及拥堵等级。
    """
    df = pd.merge(
        df_network[["link_id", "type", "length", "fft", "capacity", "name"] if "name" in df_network.columns
                   else ["link_id", "type", "length", "fft", "capacity"]],
        df_flow[["link_id", "flow", "travel_time", "capacity"]],
        on="link_id",
        how="inner",
        suffixes=("", "_flow"),
    )
    cap_col = "capacity_flow" if "capacity_flow" in df.columns else "capacity"
    df["v_c"] = (df["flow"] / df[cap_col].replace(0, float("nan"))).round(4)
    fft_col = "fft" if "fft" in df.columns else None
    if fft_col:
        df["tti"] = (df["travel_time"] / df[fft_col].replace(0, float("nan"))).round(4)
    else:
        df["tti"] = float("nan")
    bins = _VC_BINS
    labels = _VC_LABELS
    df["congestion_level"] = pd.cut(df["v_c"], bins=bins, labels=labels, right=True, include_lowest=True).astype(str)
    return df


# ═══════════════════════════════════════════════════════════════
#  结果行构建
# ═══════════════════════════════════════════════════════════════

def _build_meso_density_rows(df: pd.DataFrame, unit="km/km²") -> list:
    """中观密度结果：每行对应一个 TAZ。context_key = TAZ_{id}"""
    rows = []
    for _, r in df.iterrows():
        taz_id = r["taz_id"]
        rows.append(make_result_row("overall", f"TAZ_{taz_id}", "density",
                                    f"片区{taz_id}设施密度",
                                    float(r["density"]) if r["density"] else 0.0,
                                    result_unit=unit, sort_order=int(taz_id) if str(taz_id).isdigit() else 0))
        rows.append(make_result_row("overall", f"TAZ_{taz_id}", "length_km",
                                    f"片区{taz_id}设施长度",
                                    float(r["length_km"]),
                                    result_unit="km", sort_order=int(taz_id) if str(taz_id).isdigit() else 0))
    return rows


def _build_meso_area_percap_rows(df: pd.DataFrame) -> list:
    rows = []
    for _, r in df.iterrows():
        taz_id = r["taz_id"]
        so = int(taz_id) if str(taz_id).isdigit() else 0
        rows.append(make_result_row("overall", f"TAZ_{taz_id}", "area_per_capita_m2",
                                    f"片区{taz_id}人均慢行面积",
                                    float(r["area_per_capita_m2"]) if r["area_per_capita_m2"] else 0.0,
                                    result_unit="m²/人", sort_order=so))
        rows.append(make_result_row("overall", f"TAZ_{taz_id}", "slow_area_m2",
                                    f"片区{taz_id}慢行道路总面积",
                                    float(r["slow_area_m2"]),
                                    result_unit="m²", sort_order=so))
    return rows


def _build_meso_continuity_rows(cont_dict: dict) -> list:
    rows = []
    for taz_id, stats in cont_dict.items():
        so = int(taz_id) if str(taz_id).isdigit() else 0
        context = f"TAZ_{taz_id}"
        rows.append(make_result_row("overall", context, "total_length_km", f"片区{taz_id}设施总长度",
                                    stats["total_length_km"], result_unit="km", sort_order=so))
        rows.append(make_result_row("overall", context, "dead_end_count", f"片区{taz_id}断点数量",
                                    stats["dead_end_count"], result_unit="个", sort_order=so))
        rows.append(make_result_row("overall", context, "isolated_segment_count", f"片区{taz_id}孤立路段数",
                                    stats["isolated_segment_count"], result_unit="条", sort_order=so))
        rows.append(make_result_row("overall", context, "dead_end_ratio_pct", f"片区{taz_id}断点节点比例",
                                    stats["dead_end_ratio_pct"], result_unit="%", sort_order=so))
    return rows


def _build_micro_vc_rows(df_vc: pd.DataFrame, topn: int = 0) -> list:
    """
    微观饱和度结果行。
    topn > 0: 仅输出前 topn 条（按 v_c 降序）；topn == 0: 全部路段。
    """
    rows = []
    if topn > 0:
        df_sorted = df_vc.sort_values("v_c", ascending=False).head(topn)
    else:
        df_sorted = df_vc.sort_values("v_c", ascending=False)

    for sort_i, (_, r) in enumerate(df_sorted.iterrows(), start=1):
        link_id = int(r["link_id"])
        name = str(r.get("name", link_id))
        context = f"LINK_{link_id}"
        rows.append(make_result_row("overall", context, "v_c", f"路段{name} V/C",
                                    float(r["v_c"]) if pd.notna(r["v_c"]) else None,
                                    result_unit="", sort_order=sort_i))
        rows.append(make_result_row("overall", context, "flow", f"路段{name} 流量",
                                    float(r["flow"]), result_unit="人次/h", sort_order=sort_i))
        rows.append(make_result_row("overall", context, "travel_time", f"路段{name} 行程时间",
                                    float(r["travel_time"]), result_unit="min", sort_order=sort_i))
        if pd.notna(r.get("tti")):
            rows.append(make_result_row("overall", context, "tti", f"路段{name} TTI",
                                        float(r["tti"]), result_unit="", sort_order=sort_i))
        rows.append(make_result_row("overall", context, "congestion_level", f"路段{name} 拥堵等级",
                                    result_value_text=str(r["congestion_level"]), sort_order=sort_i))
    return rows


def _calc_capacity_impact_topn(df_network: pd.DataFrame, df_flow: pd.DataFrame, topn: int) -> pd.DataFrame:
    """计算容量下降与调整后 V/C，输出影响最大的 TOP-N 路段。"""
    if "capacity_slow_adj" not in df_network.columns:
        raise ValueError("slow_road_way 缺少 capacity_slow_adj，请先运行 slow_capacity_adjustment 或 slow_capacity_assignment")
    cols = ["link_id", "type", "length", "capacity", "capacity_slow_adj"]
    if "name" in df_network.columns:
        cols.append("name")
    df = pd.merge(
        df_network[cols],
        df_flow[["link_id", "flow", "travel_time", "capacity"]],
        on="link_id",
        how="inner",
        suffixes=("", "_flow"),
    )
    df["capacity"] = pd.to_numeric(df["capacity"], errors="coerce")
    df["capacity_slow_adj"] = pd.to_numeric(df["capacity_slow_adj"], errors="coerce")
    df["flow"] = pd.to_numeric(df["flow"], errors="coerce")
    df = df[df["capacity"].notna() & df["capacity_slow_adj"].notna() & (df["capacity"] > 0)].copy()
    df["capacity_reduction"] = (df["capacity"] - df["capacity_slow_adj"]).clip(lower=0).round(4)
    df["capacity_reduction_pct"] = (df["capacity_reduction"] / df["capacity"].replace(0, float("nan")) * 100.0).round(4)
    df["adjusted_vc"] = (df["flow"] / df["capacity_slow_adj"].replace(0, float("nan"))).round(4)
    df = df[df["capacity_reduction"] > 1e-9].copy()
    df = df.sort_values(["capacity_reduction", "adjusted_vc"], ascending=[False, False])
    if topn and topn > 0:
        df = df.head(topn)
    df["rank"] = range(1, len(df) + 1)
    return df


def _build_capacity_impact_topn_rows(df_impact: pd.DataFrame) -> list:
    """容量影响 TOP-N 结果行；一个指标输出多条路段、多项字段。"""
    rows = []
    for _, r in df_impact.iterrows():
        rank = int(r["rank"])
        link_id = int(r["link_id"])
        name = str(r.get("name", link_id))
        context = f"LINK_{link_id}"
        rows.append(make_result_row("overall", context, "rank", f"路段{name} 影响排名",
                                    rank, result_unit="", sort_order=rank))
        rows.append(make_result_row("overall", context, "capacity", f"路段{name} 原始通行能力",
                                    float(r["capacity"]), result_unit="人次/h", sort_order=rank))
        rows.append(make_result_row("overall", context, "capacity_slow_adj", f"路段{name} 影响后通行能力",
                                    float(r["capacity_slow_adj"]), result_unit="人次/h", sort_order=rank))
        rows.append(make_result_row("overall", context, "capacity_reduction", f"路段{name} 容量下降值",
                                    float(r["capacity_reduction"]), result_unit="人次/h", sort_order=rank))
        rows.append(make_result_row("overall", context, "capacity_reduction_pct", f"路段{name} 容量下降比例",
                                    float(r["capacity_reduction_pct"]), result_unit="%", sort_order=rank))
        rows.append(make_result_row("overall", context, "flow", f"路段{name} 分配流量",
                                    float(r["flow"]) if pd.notna(r["flow"]) else None, result_unit="人次/h", sort_order=rank))
        rows.append(make_result_row("overall", context, "adjusted_vc", f"路段{name} 影响后 V/C",
                                    float(r["adjusted_vc"]) if pd.notna(r["adjusted_vc"]) else None, result_unit="", sort_order=rank))
    return rows


# ═══════════════════════════════════════════════════════════════
#  通用执行入口
# ═══════════════════════════════════════════════════════════════

def _run_indicator(
    on_progress,
    cfg,
    indicator_code,
    slow_road_way_table_name="",
    slow_flow_table_name="",
    taz_table_name="",
    out_table_name="",
    link_ids=None,
    bbox=None,
):
    cfg_dict = load_cfg(cfg)
    db = cfg_dict.get("db_conn_str", {})
    spec = INDICATOR_SPECS[indicator_code]
    walk_types = [int(x) for x in cfg_dict.get("walk_type_codes", [])]
    bike_types  = [int(x) for x in cfg_dict.get("bike_type_codes",  [])]
    walk_width  = float(cfg_dict.get("walk_lane_width_m", 3.0))
    bike_width  = float(cfg_dict.get("bike_lane_width_m", 3.5))
    topn        = int(cfg_dict.get("topn", 20))

    logs = []
    conn = None
    result_table = ""
    input_tables = {}
    try:
        safe_progress(on_progress, 1, 10)
        conn = connect_db(db)
        logs.append("db.connect ok")

        input_tables["slow_road_way_table"] = resolve_runtime_table_name(
            db, slow_road_way_table_name, INPUT_TABLES["slow_road_way_table"]["raw_name"]
        )
        input_tables["slow_flow_table"] = resolve_runtime_table_name(
            db, slow_flow_table_name, INPUT_TABLES["slow_flow_table"]["raw_name"]
        )
        input_tables["taz_table"] = resolve_runtime_table_name(
            db, taz_table_name, INPUT_TABLES["taz_table"]["raw_name"]
        )
        result_table = resolve_runtime_table_name(db, out_table_name, DEFAULT_RESULT_TABLE_RAW_NAME)
        logs.append(f"tables: inputs={input_tables}, out={result_table}")
        safe_progress(on_progress, 2, 25)

        mode = spec["mode"]
        type_key = spec.get("type_key", "all")
        if type_key == "walk":
            type_codes = walk_types
        elif type_key == "bike":
            type_codes = bike_types
        else:
            type_codes = list(set(walk_types + bike_types))

        result_rows = []

        # ── 中观：空间密度 ──────────────────────────────────────
        if mode == "spatial_density":
            df_density = _spatial_density_query(
                conn,
                input_tables["slow_road_way_table"],
                input_tables["taz_table"],
                type_codes,
            )
            logs.append(f"spatial_density query: {len(df_density)} TAZs")
            result_rows = _build_meso_density_rows(df_density, unit=spec["unit"])

        # ── 中观：空间人均面积 ──────────────────────────────────
        elif mode == "spatial_area_percap":
            df_area = _spatial_area_percap_query(
                conn,
                input_tables["slow_road_way_table"],
                input_tables["taz_table"],
                walk_types, bike_types,
                walk_width, bike_width,
            )
            logs.append(f"spatial_area_percap query: {len(df_area)} TAZs")
            result_rows = _build_meso_area_percap_rows(df_area)

        # ── 中观：连续性（pandas，需 taz_id 字段或全网统计）──────
        elif mode == "meso_continuity":
            df_network = fetch_dataframe(
                conn,
                input_tables["slow_road_way_table"],
                required_columns=INPUT_TABLES["slow_road_way_table"]["required_columns"],
            )
            df_taz = fetch_dataframe(
                conn, input_tables["taz_table"],
                required_columns=INPUT_TABLES["taz_table"]["required_columns"],
            )
            logs.append(f"read.network rows={len(df_network)}, taz rows={len(df_taz)}")
            tz_schema, tz_table = split_table_name(input_tables["taz_table"])
            taz_id_col = _pick_taz_id_column(conn, tz_schema, tz_table)
            cont_dict = _meso_continuity_pandas(df_network, df_taz[taz_id_col], type_codes)
            logs.append(f"continuity computed for {len(cont_dict)} zones")
            result_rows = _build_meso_continuity_rows(cont_dict)

        # ── 微观：全路段 V/C ────────────────────────────────────
        elif mode == "micro_vc_all":
            df_network = fetch_dataframe(
                conn, input_tables["slow_road_way_table"],
                required_columns=INPUT_TABLES["slow_road_way_table"]["required_columns"],
            )
            df_flow = fetch_dataframe(
                conn, input_tables["slow_flow_table"],
                required_columns=INPUT_TABLES["slow_flow_table"]["required_columns"],
            )
            logs.append(f"read.network rows={len(df_network)}, flow rows={len(df_flow)}")
            df_network = _filter_micro_network(df_network, link_ids, bbox, logs)
            df_network = _filter_centroid_connectors(df_network, logs)
            flow_ok, flow_rows, merged_rows = _flow_merge_usable(df_network, df_flow)
            if not flow_ok:
                return _skip_micro_flow_missing(
                    conn, result_table, indicator_code, spec, input_tables, db, logs,
                    on_progress, flow_rows, merged_rows,
                )
            df_vc = _calc_micro_vc(df_network, df_flow)
            logs.append(f"micro_vc computed: {len(df_vc)} links")
            result_rows = _build_micro_vc_rows(df_vc, topn=0)

        # ── 微观：TOP-N 拥堵路段 ────────────────────────────────
        elif mode == "micro_topn_vc":
            df_network = fetch_dataframe(
                conn, input_tables["slow_road_way_table"],
                required_columns=INPUT_TABLES["slow_road_way_table"]["required_columns"],
            )
            df_flow = fetch_dataframe(
                conn, input_tables["slow_flow_table"],
                required_columns=INPUT_TABLES["slow_flow_table"]["required_columns"],
            )
            logs.append(f"read.network rows={len(df_network)}, flow rows={len(df_flow)}")
            df_network = _filter_micro_network(df_network, link_ids, bbox, logs)
            df_network = _filter_centroid_connectors(df_network, logs)
            flow_ok, flow_rows, merged_rows = _flow_merge_usable(df_network, df_flow)
            if not flow_ok:
                return _skip_micro_flow_missing(
                    conn, result_table, indicator_code, spec, input_tables, db, logs,
                    on_progress, flow_rows, merged_rows,
                )
            df_vc = _calc_micro_vc(df_network, df_flow)
            logs.append(f"topn={topn}, total links={len(df_vc)}")
            result_rows = _build_micro_vc_rows(df_vc, topn=topn)

        # ── 微观：TOP-N 容量影响路段 ─────────────────────────────
        elif mode == "micro_capacity_impact_topn":
            df_network = fetch_dataframe(
                conn, input_tables["slow_road_way_table"],
                required_columns=INPUT_TABLES["slow_road_way_table"]["required_columns"],
            )
            df_flow = fetch_dataframe(
                conn, input_tables["slow_flow_table"],
                required_columns=INPUT_TABLES["slow_flow_table"]["required_columns"],
            )
            logs.append(f"read.network rows={len(df_network)}, flow rows={len(df_flow)}")
            df_network = _filter_micro_network(df_network, link_ids, bbox, logs)
            df_network = _filter_centroid_connectors(df_network, logs)
            flow_ok, flow_rows, merged_rows = _flow_merge_usable(df_network, df_flow)
            if not flow_ok:
                return _skip_micro_flow_missing(
                    conn, result_table, indicator_code, spec, input_tables, db, logs,
                    on_progress, flow_rows, merged_rows,
                )
            df_impact = _calc_capacity_impact_topn(df_network, df_flow, topn=topn)
            logs.append(f"capacity_impact_topn={topn}, affected_links={len(df_impact)}")
            result_rows = _build_capacity_impact_topn_rows(df_impact)

        else:
            raise ValueError(f"未知 mode: {mode}")

        safe_progress(on_progress, 3, 70)

        written = write_indicator_rows(
            conn, result_table, indicator_code, spec["indicator_name"],
            "GLOBAL", result_rows,
        )
        logs.append(f"write.result rows={written}")
        safe_progress(on_progress, 4, 100)

        data = {
            "tables": {"result_table": result_table},
            "counts": {"written_rows": written},
            "result_rows": result_rows,
        }
        inputs = build_inputs_echo(db, input_tables, result_table)
        return build_success_json(indicator_code, data, logs, inputs)

    except Exception as exc:
        logs.append(f"error: {exc.__class__.__name__}: {exc}")
        data = {"tables": {"result_table": result_table}, "counts": {"written_rows": 0}}
        inputs = build_inputs_echo(db, input_tables, result_table)
        return build_error_json(indicator_code, f"{exc.__class__.__name__}: {exc}", data, logs, inputs)
    finally:
        if conn is not None:
            conn.close()


# ═══════════════════════════════════════════════════════════════
#  公开接口
# ═══════════════════════════════════════════════════════════════

def fs_run_meso_walk_density(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_meso_walk_density", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_meso_bike_density(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_meso_bike_density", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_meso_walk_continuity(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_meso_walk_continuity", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_meso_bike_continuity(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_meso_bike_continuity", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_meso_area_percap(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_meso_area_percap", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_micro_vc(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_micro_vc", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_micro_topn_vc(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_micro_topn_vc", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)


def fs_run_micro_capacity_impact_topn(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name="", link_ids=None, bbox=None):
    return _run_indicator(on_progress, cfg, "slow_micro_capacity_impact_topn", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name, link_ids=link_ids, bbox=bbox)
