from pathlib import Path
import sys

import pandas as pd

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
from diagnostic_db_common import taz_area_sum_km2
from diagnostic_db_common import write_indicator_rows


INPUT_TABLES = {
    "slow_road_way_table": {
        "raw_name": "slow_road_way",
        "required_columns": ["link_id", "type", "length", "lane_num", "init_node", "term_node", "fft", "capacity"],
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
    "slow_macro_walk_density": {
        "indicator_name": "步行设施网络密度",
        "method_name": "calc_1_walk_density",
        "unit": "km/km²",
        "needs_flow": False,
    },
    "slow_macro_bike_density": {
        "indicator_name": "非机动车道网络密度",
        "method_name": "calc_2_bike_density",
        "unit": "km/km²",
        "needs_flow": False,
    },
    "slow_macro_walk_continuity": {
        "indicator_name": "步行设施长度与连续性",
        "method_name": "calc_3_walk_continuity",
        "unit": "km",
        "needs_flow": False,
    },
    "slow_macro_bike_continuity": {
        "indicator_name": "非机动车设施长度与连续性",
        "method_name": "calc_4_bike_continuity",
        "unit": "km",
        "needs_flow": False,
    },
    "slow_macro_area_percap": {
        "indicator_name": "人均慢行道路面积",
        "method_name": "calc_5_area_percap",
        "unit": "m²/人",
        "needs_flow": False,
    },
    "slow_macro_vc_distribution": {
        "indicator_name": "慢行路段饱和度分布",
        "method_name": "calc_6_vc_distribution",
        "unit": "%",
        "needs_flow": True,
    },
    "slow_macro_capacity_impact": {
        "indicator_name": "慢行通行能力影响程度",
        "method_name": "calc_7_capacity_impact",
        "unit": "",
        "needs_flow": False,
    },
}


FLOW_SKIP_REASON = (
    "slow_greedy_link_flow_results 为空或无可关联流量，"
    "本指标已跳过（图表/数值留空）；请先运行 base_slow_network 生成分配流量。"
)


def _length_series_to_km(length_series) -> float:
    """PG slow_road_way.length 一般为米；中位数>10 时按米→km。"""
    s = length_series.astype(float)
    med = float(s.median()) if len(s) else 0.0
    total = float(s.sum())
    return total / 1000.0 if med > 10 else total


def _flow_merge_usable(df_network: pd.DataFrame, df_flow: pd.DataFrame) -> tuple[bool, int, int]:
    flow_rows = 0 if df_flow is None else len(df_flow)
    if flow_rows == 0:
        return False, flow_rows, 0
    merged = pd.merge(df_network, df_flow, on="link_id", how="inner", suffixes=("", "_flow"))
    return len(merged) > 0, flow_rows, len(merged)


def _filter_centroid_connectors(df_network: pd.DataFrame) -> tuple[pd.DataFrame, int]:
    """Exclude centroid connector links from slow macro indicators and charts."""
    if df_network is None or df_network.empty:
        return df_network, 0
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
    return df_network.loc[~mask].copy(), removed


class SlowMacroCalculator:
    """
    慢行交通宏观诊断指标计算器。

    Parameters
    ----------
    df_network : pd.DataFrame    slow_road_way 表数据
    df_flow    : pd.DataFrame    slow_greedy_link_flow_results 表数据（可为空 DataFrame）
    df_taz     : pd.DataFrame    other_taz_socioeconomic 表数据
    walk_type_codes : list[int]  步行路段的 type 代码，空列表=不过滤（全部视为步行）
    bike_type_codes : list[int]  骑行路段的 type 代码，空列表=不过滤（全部视为骑行）
    walk_lane_width_m : float    步行车道宽度（m），用于面积估算，默认 3.0
    bike_lane_width_m : float    骑行车道宽度（m），用于面积估算，默认 3.5
    """

    def __init__(
        self,
        df_network: pd.DataFrame,
        df_flow: pd.DataFrame,
        df_taz: pd.DataFrame,
        walk_type_codes=None,
        bike_type_codes=None,
        walk_lane_width_m=3.0,
        bike_lane_width_m=3.5,
    ):
        self.df_network, self.centroid_connector_removed = _filter_centroid_connectors(df_network.copy())
        self.df_taz = df_taz.copy()
        self.walk_types = [int(x) for x in (walk_type_codes or [])]
        self.bike_types = [int(x) for x in (bike_type_codes or [])]
        self.walk_lane_width = float(walk_lane_width_m)
        self.bike_lane_width = float(bike_lane_width_m)

        if not df_flow.empty:
            merge_cols = [c for c in ["link_id", "flow", "travel_time", "capacity"] if c in df_flow.columns]
            self.df_merged = pd.merge(
                self.df_network,
                df_flow[merge_cols],
                on="link_id",
                how="inner",
                suffixes=("", "_flow"),
            )
            cap_col = "capacity_flow" if "capacity_flow" in self.df_merged.columns else "capacity"
            self.df_merged["v_c"] = (
                self.df_merged["flow"] / self.df_merged[cap_col].replace(0, float("nan"))
            )
        else:
            self.df_merged = pd.DataFrame()

    # ── 内部过滤工具 ──────────────────────────────────────────────

    def _filter(self, df: pd.DataFrame, type_codes: list) -> pd.DataFrame:
        if type_codes:
            return df[df["type"].isin(type_codes)]
        return df

    def _filter_walk(self, df: pd.DataFrame) -> pd.DataFrame:
        return self._filter(df, self.walk_types)

    def _filter_bike(self, df: pd.DataFrame) -> pd.DataFrame:
        return self._filter(df, self.bike_types)

    def _filter_slow_all(self, df: pd.DataFrame) -> pd.DataFrame:
        all_types = list(set(self.walk_types + self.bike_types))
        return self._filter(df, all_types)

    @property
    def _region_area_km2(self) -> float:
        return taz_area_sum_km2(self.df_taz["area"])

    @property
    def _total_pop(self) -> float:
        return float(self.df_taz["pop"].sum())

    # ── 指标计算方法 ──────────────────────────────────────────────

    def calc_1_walk_density(self) -> dict:
        """宏观指标1：步行设施网络密度（km/km²）"""
        df_walk = self._filter_walk(self.df_network)
        walk_km = _length_series_to_km(df_walk["length"])
        area_km2 = self._region_area_km2
        density = round(walk_km / area_km2, 4) if area_km2 > 0 else 0.0
        if density >= 14.0:
            meets = "高强度地区达标（≥14 km/km²）"
        elif density >= 8.0:
            meets = "一般地区达标（≥8 km/km²）"
        else:
            meets = "不达标（<8 km/km²）"
        return {
            "walk_total_km": round(walk_km, 4),
            "region_area_km2": round(area_km2, 4),
            "density": density,
            "meets_standard": meets,
        }

    def calc_2_bike_density(self) -> dict:
        """宏观指标2：非机动车道网络密度（km/km²）"""
        df_bike = self._filter_bike(self.df_network)
        bike_km = _length_series_to_km(df_bike["length"])
        area_km2 = self._region_area_km2
        density = round(bike_km / area_km2, 4) if area_km2 > 0 else 0.0
        return {
            "bike_total_km": round(bike_km, 4),
            "region_area_km2": round(area_km2, 4),
            "density": density,
            "meets_standard": "达标（≥8 km/km²）" if density >= 8.0 else "不达标（<8 km/km²）",
        }

    def _calc_continuity(self, df_links: pd.DataFrame) -> dict:
        """通用网络连续性分析（节点度法）"""
        total_km = _length_series_to_km(df_links["length"])
        seg_count = len(df_links)
        if seg_count == 0:
            return {
                "total_length_km": 0.0,
                "segment_count": 0,
                "node_count": 0,
                "dead_end_count": 0,
                "isolated_segment_count": 0,
                "dead_end_ratio_pct": 0.0,
            }
        all_nodes = pd.concat(
            [df_links["init_node"].rename("node"), df_links["term_node"].rename("node")]
        )
        node_degree = all_nodes.value_counts()
        dead_end_nodes = set(node_degree[node_degree == 1].index)
        dead_end_count = len(dead_end_nodes)
        isolated = df_links[
            df_links["init_node"].isin(dead_end_nodes)
            & df_links["term_node"].isin(dead_end_nodes)
        ]
        isolated_count = len(isolated)
        total_nodes = len(node_degree)
        dead_end_ratio = round(dead_end_count / total_nodes * 100, 2) if total_nodes > 0 else 0.0
        return {
            "total_length_km": round(total_km, 4),
            "segment_count": seg_count,
            "node_count": total_nodes,
            "dead_end_count": dead_end_count,
            "isolated_segment_count": isolated_count,
            "dead_end_ratio_pct": dead_end_ratio,
        }

    def calc_3_walk_continuity(self) -> dict:
        """宏观指标3：步行设施长度与连续性"""
        return self._calc_continuity(self._filter_walk(self.df_network))

    def calc_4_bike_continuity(self) -> dict:
        """宏观指标4：非机动车设施长度与连续性"""
        return self._calc_continuity(self._filter_bike(self.df_network))

    def calc_5_area_percap(self) -> dict:
        """宏观指标5：人均慢行道路面积（m²/人）"""
        df = self.df_network.copy()
        if self.walk_types or self.bike_types:
            df["_width"] = df["type"].apply(
                lambda t: (
                    self.walk_lane_width if t in self.walk_types
                    else (self.bike_lane_width if t in self.bike_types else 0.0)
                )
            )
            df_slow = df[df["_width"] > 0]
        else:
            df["_width"] = self.bike_lane_width
            df_slow = df
        # lane_num ?????????? 0/?????????????? 1 ????????
        # length ???/??????>10 ?????????????
        lane_num = pd.to_numeric(df_slow.get("lane_num", 1), errors="coerce").fillna(1)
        lane_num = lane_num.mask(lane_num <= 0, 1)
        length_num = pd.to_numeric(df_slow["length"], errors="coerce").fillna(0)
        length_m = length_num if (float(length_num.median()) if len(length_num) else 0.0) > 10 else length_num * 1000.0
        total_area_m2 = float((length_m * lane_num * df_slow["_width"]).sum())
        pop = self._total_pop
        area_percap = round(total_area_m2 / pop, 4) if pop > 0 else 0.0
        return {
            "total_slow_area_m2": round(total_area_m2, 2),
            "total_population": int(pop),
            "area_per_capita_m2": area_percap,
        }

    def calc_6_vc_distribution(self) -> dict:
        """宏观指标6：慢行路段饱和度分布（来自分配结果）"""
        df = self.df_merged
        if df.empty:
            return {"overall": {}, "by_type": {}}
        df = df.copy()
        bins = [0, 0.6, 0.8, 0.9, float("inf")]
        labels = ["畅通", "基本畅通", "拥挤", "严重拥堵"]
        df["vc_level"] = pd.cut(df["v_c"], bins=bins, labels=labels, right=True, include_lowest=True)
        total_len = df["length"].sum()
        overall = {
            str(lv): round(float(grp["length"].sum() / total_len * 100), 2)
            for lv, grp in df.groupby("vc_level", observed=False)
        }
        by_type = {}
        for t, gdf in df.groupby("type"):
            t_len = gdf["length"].sum()
            by_type[f"type_{t}"] = {
                str(lv): round(float(grp["length"].sum() / t_len * 100), 2)
                for lv, grp in gdf.groupby("vc_level", observed=False)
            }
        return {"overall": overall, "by_type": by_type}

    def calc_7_capacity_impact(self) -> dict:
        """宏观指标7：慢行通行能力影响程度（来自 capacity_slow_adj 回填列）。"""
        df = self.df_network.copy()
        if "capacity_slow_adj" not in df.columns:
            raise ValueError("slow_road_way 缺少 capacity_slow_adj，请先运行 slow_capacity_adjustment 或 slow_capacity_assignment")
        df["capacity"] = pd.to_numeric(df["capacity"], errors="coerce")
        df["capacity_slow_adj"] = pd.to_numeric(df["capacity_slow_adj"], errors="coerce")
        valid = df[df["capacity"].notna() & df["capacity_slow_adj"].notna()].copy()
        valid = valid[valid["capacity"] > 0]
        total_links = int(len(valid))
        if total_links == 0:
            return {
                "total_links": 0,
                "affected_links": 0,
                "affected_ratio_pct": 0.0,
                "avg_capacity_reduction": 0.0,
                "avg_capacity_reduction_pct": 0.0,
                "max_capacity_reduction": 0.0,
            }
        valid["capacity_reduction"] = (valid["capacity"] - valid["capacity_slow_adj"]).clip(lower=0)
        valid["capacity_reduction_pct"] = valid["capacity_reduction"] / valid["capacity"] * 100.0
        affected = valid[valid["capacity_reduction"] > 1e-9]
        affected_links = int(len(affected))
        return {
            "total_links": total_links,
            "affected_links": affected_links,
            "affected_ratio_pct": round(affected_links / total_links * 100.0, 2),
            "avg_capacity_reduction": round(float(affected["capacity_reduction"].mean()) if affected_links else 0.0, 4),
            "avg_capacity_reduction_pct": round(float(affected["capacity_reduction_pct"].mean()) if affected_links else 0.0, 4),
            "max_capacity_reduction": round(float(affected["capacity_reduction"].max()) if affected_links else 0.0, 4),
        }


# ── 结果行构建 ─────────────────────────────────────────────────────

def _build_result_rows(indicator_code: str, result: dict) -> list:
    rows = []
    sort_order = 1

    if indicator_code == "slow_macro_walk_density":
        rows.append(make_result_row("overall", "overall", "walk_total_km", "步行路段总长度", result["walk_total_km"], result_unit="km", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "region_area_km2", "区域总面积", result["region_area_km2"], result_unit="km²", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "density", "步行设施网络密度", result["density"], result_unit="km/km²", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "meets_standard", "达标评价", result_value_text=result["meets_standard"], sort_order=sort_order))

    elif indicator_code == "slow_macro_bike_density":
        rows.append(make_result_row("overall", "overall", "bike_total_km", "非机动车道总长度", result["bike_total_km"], result_unit="km", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "region_area_km2", "区域总面积", result["region_area_km2"], result_unit="km²", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "density", "非机动车道网络密度", result["density"], result_unit="km/km²", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "meets_standard", "达标评价", result_value_text=result["meets_standard"], sort_order=sort_order))

    elif indicator_code in ("slow_macro_walk_continuity", "slow_macro_bike_continuity"):
        label = "步行" if "walk" in indicator_code else "非机动车"
        rows.append(make_result_row("overall", "overall", "total_length_km", f"{label}设施总长度", result["total_length_km"], result_unit="km", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "segment_count", "路段数量", result["segment_count"], result_unit="条", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "node_count", "节点总数", result["node_count"], result_unit="个", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "dead_end_count", "断点（悬挂节点）数量", result["dead_end_count"], result_unit="个", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "isolated_segment_count", "孤立路段数量", result["isolated_segment_count"], result_unit="条", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "dead_end_ratio_pct", "断点节点比例", result["dead_end_ratio_pct"], result_unit="%", sort_order=sort_order))

    elif indicator_code == "slow_macro_area_percap":
        rows.append(make_result_row("overall", "overall", "total_slow_area_m2", "慢行道路总面积", result["total_slow_area_m2"], result_unit="m²", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "total_population", "区域总人口", result["total_population"], result_unit="人", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "area_per_capita_m2", "人均慢行道路面积", result["area_per_capita_m2"], result_unit="m²/人", sort_order=sort_order))

    elif indicator_code == "slow_macro_vc_distribution":
        for level_name, pct in result.get("overall", {}).items():
            rows.append(make_result_row("overall", "overall", str(level_name), str(level_name), float(pct), result_unit="%", sort_order=sort_order)); sort_order += 1
        for type_key, dist in result.get("by_type", {}).items():
            for level_name, pct in dist.items():
                rows.append(make_result_row("by_type", type_key, str(level_name), str(level_name), float(pct), result_unit="%", sort_order=sort_order)); sort_order += 1

    elif indicator_code == "slow_macro_capacity_impact":
        rows.append(make_result_row("overall", "overall", "total_links", "慢行路段总数", result["total_links"], result_unit="条", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "affected_links", "通行能力受影响路段数", result["affected_links"], result_unit="条", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "affected_ratio_pct", "通行能力受影响路段占比", result["affected_ratio_pct"], result_unit="%", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "avg_capacity_reduction", "受影响路段平均容量下降值", result["avg_capacity_reduction"], result_unit="人次/h", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "avg_capacity_reduction_pct", "受影响路段平均容量下降比例", result["avg_capacity_reduction_pct"], result_unit="%", sort_order=sort_order)); sort_order += 1
        rows.append(make_result_row("overall", "overall", "max_capacity_reduction", "最大容量下降值", result["max_capacity_reduction"], result_unit="人次/h", sort_order=sort_order)); sort_order += 1

    return rows


# ── 通用执行入口 ───────────────────────────────────────────────────

def _run_indicator(
    on_progress,
    cfg,
    indicator_code,
    slow_road_way_table_name="",
    slow_flow_table_name="",
    taz_table_name="",
    out_table_name="",
):
    cfg_dict = load_cfg(cfg)
    db = cfg_dict.get("db_conn_str", {})
    spec = INDICATOR_SPECS[indicator_code]
    walk_types = cfg_dict.get("walk_type_codes", [])
    bike_types = cfg_dict.get("bike_type_codes", [])
    walk_width = cfg_dict.get("walk_lane_width_m", 3.0)
    bike_width = cfg_dict.get("bike_lane_width_m", 3.5)
    logs = []
    conn = None
    result_table = ""
    input_tables = {}
    try:
        safe_progress(on_progress, 1, 10)
        conn = connect_db(db)
        logs.append("db.connect ok")
        safe_progress(on_progress, 2, 25)

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

        df_network = fetch_dataframe(
            conn,
            input_tables["slow_road_way_table"],
            required_columns=INPUT_TABLES["slow_road_way_table"]["required_columns"],
        )
        logs.append(f"read.slow_road_way rows={len(df_network)}")

        if spec["needs_flow"]:
            df_flow = fetch_dataframe(
                conn,
                input_tables["slow_flow_table"],
                required_columns=INPUT_TABLES["slow_flow_table"]["required_columns"],
            )
            logs.append(f"read.slow_flow rows={len(df_flow)}")
        else:
            df_flow = pd.DataFrame()

        df_taz = fetch_dataframe(
            conn,
            input_tables["taz_table"],
            required_columns=INPUT_TABLES["taz_table"]["required_columns"],
        )
        logs.append(f"read.taz rows={len(df_taz)}")
        safe_progress(on_progress, 3, 60)

        if spec["needs_flow"]:
            flow_ok, flow_rows, merged_rows = _flow_merge_usable(df_network, df_flow)
            if not flow_ok:
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

        calculator = SlowMacroCalculator(
            df_network, df_flow, df_taz,
            walk_type_codes=walk_types,
            bike_type_codes=bike_types,
            walk_lane_width_m=walk_width,
            bike_lane_width_m=bike_width,
        )
        result = getattr(calculator, spec["method_name"])()
        result_rows = _build_result_rows(indicator_code, result)
        logs.append(f"calc.{spec['method_name']} ok, rows={len(result_rows)}")

        written = write_indicator_rows(
            conn, result_table, indicator_code, spec["indicator_name"], "GLOBAL", result_rows
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


# ── 公开接口 ──────────────────────────────────────────────────────

def fs_run_walk_density(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_walk_density", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)


def fs_run_bike_density(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_bike_density", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)


def fs_run_walk_continuity(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_walk_continuity", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)


def fs_run_bike_continuity(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_bike_continuity", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)


def fs_run_area_percap(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_area_percap", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)


def fs_run_vc_distribution(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_vc_distribution", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)


def fs_run_capacity_impact(on_progress, cfg, slow_road_way_table_name="", slow_flow_table_name="", taz_table_name="", out_table_name=""):
    return _run_indicator(on_progress, cfg, "slow_macro_capacity_impact", slow_road_way_table_name, slow_flow_table_name, taz_table_name, out_table_name)
