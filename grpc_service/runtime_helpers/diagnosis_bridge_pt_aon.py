#!/usr/bin/env python3
"""TNA diagnosis bridge.

This helper is called by the C++ gRPC service with a request KV file and writes
the response as KV.  The pt_* implementation below is intentionally independent
from the old diagnosis application because the current transit AON model uses
project*_pt_* and project*_pt_*_result tables instead of other_bus_route.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import traceback
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple

try:
    import psycopg2
    import psycopg2.extras
    from psycopg2 import sql
except Exception:  # pragma: no cover - server dependency
    psycopg2 = None
    sql = None


SCHEMA = "user_project"
DEFAULT_DB_CONF = "/home/giss/opt_algorithms/db.conf"
PT_MACRO_INPUT_SUFFIX = "pt_macro_input"
OBSOLETE_PT_MACRO_INDICATORS = ("pt_macro_rail_pop_coverage_800m",)
INPUT_SUFFIX_ALIASES = {
    "bus_point": ("bus_point",),
    "bus_way": ("bus_way",),
}
RESULT_ROW_META = {
    "pt_macro_bus_density": ("overall", "density"),
    "pt_macro_pt_pop_coverage_500m": ("500m", "rate"),
    "pt_macro_city_bus_area_coverage": ("500m", "rate"),
    "pt_macro_infeasible_ratio": ("overall", "rate"),
    "pt_macro_wait_cost_share": ("overall", "rate"),
    "pt_macro_high_vc_stop_link_ratio": ("overall", "rate"),
    "pt_macro_max_stop_link_vc": ("overall", "vc"),
    "pt_macro_avg_stop_link_vc": ("overall", "vc"),
}


@dataclass
class Indicator:
    code: str
    name: str
    unit: str
    dimension: str
    calculator: Callable[["DiagnosisContext"], Optional[float]]
    required_tables: Tuple[str, ...] = ()
    skip_reason: str = ""


@dataclass
class IndicatorResult:
    code: str
    name: str
    unit: str
    dimension: str
    context_key: str
    status: int
    message: str
    value: Optional[float] = None
    missing_tables: Tuple[str, ...] = ()
    extra: Optional[Dict[str, Any]] = None


class DiagnosisError(Exception):
    pass


def escape_value(value: Any) -> str:
    text = "" if value is None else str(value)
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


def unescape_value(value: str) -> str:
    out: List[str] = []
    i = 0
    while i < len(value):
        if value[i] == "\\" and i + 1 < len(value):
            nxt = value[i + 1]
            if nxt == "n":
                out.append("\n")
            elif nxt == "r":
                out.append("\r")
            else:
                out.append(nxt)
            i += 2
        else:
            out.append(value[i])
            i += 1
    return "".join(out)


def read_kv(path: str) -> Dict[str, str]:
    data: Dict[str, str] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.lstrip("\ufeff")
            data[key] = unescape_value(value)
    return data


def write_kv(path: str, data: Dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        for key in sorted(data.keys()):
            fh.write(f"{key}={escape_value(data[key])}\n")


def parse_positive_int(value: str, name: str, allow_zero: bool = False) -> int:
    try:
        parsed = int(value)
    except Exception:
        raise DiagnosisError(f"入参错误：{name} 必须是整数")
    if allow_zero:
        if parsed < 0:
            raise DiagnosisError(f"入参错误：{name} 不能小于 0")
    elif parsed <= 0:
        raise DiagnosisError(f"入参错误：{name} 必须大于 0")
    return parsed


def parse_param2(text: str) -> Dict[str, Any]:
    if not text or not text.strip():
        return {}
    try:
        data = json.loads(text)
    except Exception as exc:
        raise DiagnosisError(f"入参错误：param2 不是合法 JSON：{exc}")
    if not isinstance(data, dict):
        raise DiagnosisError("入参错误：param2 必须是 JSON 对象")
    return data


def parse_list(value: Any) -> List[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value if str(item).strip()]
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return []
        try:
            parsed = json.loads(text)
            if isinstance(parsed, list):
                return [str(item) for item in parsed if str(item).strip()]
        except Exception:
            pass
        return [item.strip() for item in text.split(",") if item.strip()]
    return [str(value)]


def load_db_conf(path: str = DEFAULT_DB_CONF) -> Dict[str, str]:
    conf: Dict[str, str] = {}
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                conf[key.strip()] = value.strip().strip("'\"")
    return conf


def conninfo_from_request(param1: str) -> str:
    if param1 and param1.strip():
        return param1.strip()
    conf = load_db_conf()
    host = conf.get("host") or conf.get("DB_HOST") or "localhost"
    port = conf.get("port") or conf.get("DB_PORT") or "5432"
    dbname = conf.get("dbname") or conf.get("database") or conf.get("DB_NAME") or "urban"
    user = conf.get("user") or conf.get("username") or conf.get("DB_USER") or "urban"
    password = conf.get("password") or conf.get("DB_PASSWORD") or os.environ.get("TNA_DB_PASSWORD", "")
    return f"host={host} port={port} dbname={dbname} user={user} password={password}"


def qname(table: str) -> Any:
    return sql.SQL("{}.{}").format(sql.Identifier(SCHEMA), sql.Identifier(table))


def fetchone_number(cur: Any, query: Any, params: Sequence[Any] = ()) -> float:
    cur.execute(query, params)
    row = cur.fetchone()
    if not row:
        return 0.0
    value = row[0]
    return 0.0 if value is None else float(value)


def haversine_km(lon1: float, lat1: float, lon2: float, lat2: float) -> float:
    radius = 6371.0088
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    d_phi = math.radians(lat2 - lat1)
    d_lam = math.radians(lon2 - lon1)
    a = math.sin(d_phi / 2.0) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(d_lam / 2.0) ** 2
    return 2.0 * radius * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))


class DiagnosisContext:
    def __init__(self, conn: Any, project_id: int, user_id: int, case_id: int, param2: Dict[str, Any]) -> None:
        self.conn = conn
        self.project_id = project_id
        self.user_id = user_id
        self.case_id = case_id
        self.param2 = param2
        self.prefix = f"project{project_id}_user{user_id}_case{case_id}_"
        self.table_cache: Dict[str, bool] = {}
        self.summary_cache: Optional[Dict[str, float]] = None
        self.macro_input_cache: Dict[str, Optional[float]] = {}
        self.route_metrics_cache: Optional[List[Dict[str, Any]]] = None
        self.line_ids = parse_list(param2.get("line_ids"))
        self.context_key = "GLOBAL" if not self.line_ids else "lines_" + "_".join(self.line_ids[:20])

    def table(self, suffix: str) -> str:
        return self.prefix + self.physical_suffix(suffix)

    def exact_table(self, suffix: str) -> str:
        return self.prefix + suffix

    def exact_table_exists(self, suffix: str) -> bool:
        key = "exact:" + suffix
        if key in self.table_cache:
            return self.table_cache[key]
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT EXISTS (
                  SELECT 1 FROM information_schema.tables
                  WHERE table_schema = %s AND table_name = %s
                )
                """,
                (SCHEMA, self.exact_table(suffix)),
            )
            exists = bool(cur.fetchone()[0])
        self.table_cache[key] = exists
        return exists

    def physical_suffix(self, suffix: str) -> str:
        candidates = INPUT_SUFFIX_ALIASES.get(suffix, (suffix,))
        for candidate in candidates:
            if self.exact_table_exists(candidate):
                return candidate
        return candidates[0]

    def table_exists(self, suffix: str) -> bool:
        if suffix in self.table_cache:
            return self.table_cache[suffix]
        exists = any(
            self.exact_table_exists(candidate)
            for candidate in INPUT_SUFFIX_ALIASES.get(suffix, (suffix,))
        )
        self.table_cache[suffix] = exists
        return exists

    def missing_tables(self, suffixes: Iterable[str]) -> Tuple[str, ...]:
        return tuple(self.table(suffix) for suffix in suffixes if not self.table_exists(suffix))

    def column_exists(self, suffix: str, column: str) -> bool:
        if not self.table_exists(suffix):
            return False
        with self.conn.cursor() as cur:
            cur.execute(
                """
                SELECT EXISTS (
                  SELECT 1 FROM information_schema.columns
                  WHERE table_schema = %s AND table_name = %s AND column_name = %s
                )
                """,
                (SCHEMA, self.table(suffix), column),
            )
            return bool(cur.fetchone()[0])

    def first_existing_column(self, suffix: str, candidates: Sequence[str]) -> str:
        for column in candidates:
            if self.column_exists(suffix, column):
                return column
        raise DiagnosisError(f"输入表 {self.table(suffix)} 缺少字段：{', '.join(candidates)}")

    def summary(self) -> Dict[str, float]:
        if self.summary_cache is not None:
            return self.summary_cache
        values: Dict[str, float] = {}
        if not self.table_exists("pt_summary_result"):
            self.summary_cache = values
            return values
        with self.conn.cursor() as cur:
            cur.execute(
                sql.SQL("SELECT metric_key, metric_value FROM {}").format(qname(self.table("pt_summary_result")))
            )
            for key, value in cur.fetchall():
                try:
                    values[str(key)] = float(value)
                except Exception:
                    continue
        self.summary_cache = values
        return values

    def macro_input_value(self, indicator_code: str) -> Optional[float]:
        if indicator_code in self.macro_input_cache:
            return self.macro_input_cache[indicator_code]
        value: Optional[float] = None
        if self.table_exists(PT_MACRO_INPUT_SUFFIX):
            with self.conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
                cur.execute(
                    sql.SQL("SELECT * FROM {} WHERE indicator_code::text = %s LIMIT 1").format(
                        qname(self.table(PT_MACRO_INPUT_SUFFIX))
                    ),
                    (indicator_code,),
                )
                row = cur.fetchone()
            if row:
                normalized = {str(k).lower(): v for k, v in dict(row).items()}
                for key in ("value", "result_value_num", "metric_value"):
                    raw = normalized.get(key)
                    if raw is not None and str(raw).strip() != "":
                        value = float(raw)
                        break
                if value is None:
                    numerator = normalized.get("numerator")
                    denominator = normalized.get("denominator")
                    if numerator is not None and denominator is not None and float(denominator) != 0.0:
                        value = float(numerator) / float(denominator)
        self.macro_input_cache[indicator_code] = value
        return value

    def scalar_table_count(self, suffix: str) -> float:
        if not self.table_exists(suffix):
            return 0.0
        with self.conn.cursor() as cur:
            return fetchone_number(cur, sql.SQL("SELECT COUNT(*) FROM {}").format(qname(self.table(suffix))))

    def scalar_distinct_count(self, suffix: str, column: str) -> float:
        if not self.table_exists(suffix):
            return 0.0
        if not self.column_exists(suffix, column):
            return 0.0
        with self.conn.cursor() as cur:
            return fetchone_number(
                cur,
                sql.SQL("SELECT COUNT(DISTINCT {}) FROM {}").format(sql.Identifier(column), qname(self.table(suffix))),
            )

    def scalar_query(self, suffix: str, query_body: str) -> Optional[float]:
        if not self.table_exists(suffix):
            return None
        with self.conn.cursor() as cur:
            cur.execute(sql.SQL(query_body).format(qname(self.table(suffix))))
            row = cur.fetchone()
            if not row or row[0] is None:
                return None
            return float(row[0])

    def route_metrics(self) -> List[Dict[str, Any]]:
        if self.route_metrics_cache is not None:
            return self.route_metrics_cache
        if not (self.table_exists("pt_shape") and self.table_exists("bus_point")):
            self.route_metrics_cache = []
            return []

        seq_column = self.first_existing_column("pt_shape", ["stop_sequence", "stop sequence", "sequence", "seq"])
        stop_id_column = self.first_existing_column("bus_point", ["node_id"])
        x_column = None
        y_column = None
        for candidate in ["x", "lon", "lng", "longitude"]:
            if self.column_exists("bus_point", candidate):
                x_column = candidate
                break
        for candidate in ["y", "lat", "latitude"]:
            if self.column_exists("bus_point", candidate):
                y_column = candidate
                break
        has_geometry = self.column_exists("bus_point", "geometry")
        if x_column and y_column:
            x_expr = sql.SQL("st.{}::double precision").format(sql.Identifier(x_column))
            y_expr = sql.SQL("st.{}::double precision").format(sql.Identifier(y_column))
        elif has_geometry:
            x_expr = sql.SQL("ST_X(st.geometry::geometry)")
            y_expr = sql.SQL("ST_Y(st.geometry::geometry)")
        else:
            raise DiagnosisError(f"输入表 {self.table('bus_point')} 缺少坐标字段：x/y 或 geometry")
        if self.column_exists("pt_shape", "shape_id"):
            with self.conn.cursor() as cur:
                cur.execute(
                    sql.SQL(
                        "SELECT COUNT(*)::bigint, "
                        "COUNT(DISTINCT shape_id::text)::bigint FROM {}"
                    ).format(qname(self.table("pt_shape")))
                )
                total_rows, distinct_shape_ids = cur.fetchone()
            if total_rows and total_rows == distinct_shape_ids:
                shape_expr = sql.SQL("s.route_id::text")
            else:
                shape_expr = sql.SQL("s.shape_id::text")
        elif self.column_exists("pt_shape", "dir"):
            shape_expr = sql.SQL("(s.route_id::text || '_dir' || s.dir::text)")
        else:
            raise DiagnosisError("输入表 pt_shape 缺少 shape_id 字段；当前成都结构需提供 dir 字段用于派生 shape_id")
        with self.conn.cursor() as cur:
            query = sql.SQL(
                """
                SELECT s.route_id::text, {} AS shape_id, s.stop_id::text,
                       s.{}::integer, {}, {}
                FROM {} s
                JOIN {} st ON st.{}::text = s.stop_id::text
                WHERE {} IS NOT NULL AND {} IS NOT NULL
                """
            ).format(
                shape_expr,
                sql.Identifier(seq_column),
                x_expr,
                y_expr,
                qname(self.table("pt_shape")),
                qname(self.table("bus_point")),
                sql.Identifier(stop_id_column),
                x_expr,
                y_expr,
            )
            params: List[Any] = []
            if self.line_ids:
                query += sql.SQL(" AND s.route_id::text = ANY(%s)")
                params.append(self.line_ids)
            query += sql.SQL(" ORDER BY s.route_id::text, shape_id, s.{}::integer").format(sql.Identifier(seq_column))
            cur.execute(query, params)
            rows = cur.fetchall()

        shapes: Dict[Tuple[str, str], List[Tuple[int, str, float, float]]] = {}
        for route_id, shape_id, stop_id, seq, lon, lat in rows:
            shapes.setdefault((route_id, shape_id), []).append((int(seq), stop_id, float(lon), float(lat)))

        route_acc: Dict[str, Dict[str, Any]] = {}
        for (route_id, _shape_id), stops in shapes.items():
            ordered = sorted(stops, key=lambda item: item[0])
            length_km = 0.0
            prev: Optional[Tuple[int, str, float, float]] = None
            unique_stops = set()
            for item in ordered:
                unique_stops.add(item[1])
                if prev is not None:
                    length_km += haversine_km(prev[2], prev[3], item[2], item[3])
                prev = item
            current = route_acc.setdefault(route_id, {"route_id": route_id, "length_km": 0.0, "stations": 0})
            current["length_km"] = max(current["length_km"], length_km)
            current["stations"] = max(current["stations"], len(unique_stops))

        self.route_metrics_cache = list(route_acc.values())
        return self.route_metrics_cache


def metric_from_summary(key: str, fallback_suffix: Optional[str] = None, fallback_column: Optional[str] = None) -> Callable[[DiagnosisContext], Optional[float]]:
    def calc(ctx: DiagnosisContext) -> Optional[float]:
        values = ctx.summary()
        if key in values:
            return values[key]
        if fallback_suffix and fallback_column:
            return ctx.scalar_distinct_count(fallback_suffix, fallback_column)
        if fallback_suffix:
            return ctx.scalar_table_count(fallback_suffix)
        return None
    return calc


def walk_link_count(ctx: DiagnosisContext) -> Optional[float]:
    values = ctx.summary()
    if "num_of_walk_links" in values:
        return values["num_of_walk_links"]
    if ctx.table_exists("pt_walk"):
        return ctx.scalar_table_count("pt_walk")
    return 0.0


def infeasible_ratio(ctx: DiagnosisContext) -> Optional[float]:
    values = ctx.summary()
    total = values.get("total_demand", 0.0)
    infeasible = values.get("infeasible_flow", 0.0)
    if total <= 0:
        return None
    return infeasible / total


def top_link_flow(ctx: DiagnosisContext) -> Optional[float]:
    if not ctx.table_exists("pt_link_result"):
        return None
    with ctx.conn.cursor() as cur:
        return fetchone_number(cur, sql.SQL("SELECT COALESCE(MAX(flow), 0) FROM {}").format(qname(ctx.table("pt_link_result"))))


def macro_input_metric(indicator_code: str) -> Callable[[DiagnosisContext], Optional[float]]:
    def calc(ctx: DiagnosisContext) -> Optional[float]:
        return ctx.macro_input_value(indicator_code)
    return calc


def wait_cost_share(ctx: DiagnosisContext) -> Optional[float]:
    values = ctx.summary()
    total_wait = values.get("total_wait_cost")
    total_cost = values.get("total_system_cost")
    if total_wait is None or total_cost is None or total_cost <= 0:
        return None
    return total_wait / total_cost


def max_path_cost(ctx: DiagnosisContext) -> Optional[float]:
    values = ctx.summary()
    if "max_path_cost" in values:
        return values["max_path_cost"]
    return ctx.scalar_query("pt_path_result", "SELECT MAX(path_cost) FROM {} WHERE path_cost IS NOT NULL")


def avg_path_links(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query("pt_path_result", "SELECT AVG(num_links) FROM {} WHERE num_links IS NOT NULL")


def max_stop_link_vc(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query("pt_stop_link_vc_result", "SELECT MAX(vc_ratio) FROM {} WHERE vc_ratio IS NOT NULL")


def avg_stop_link_vc(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query("pt_stop_link_vc_result", "SELECT AVG(vc_ratio) FROM {} WHERE vc_ratio IS NOT NULL")


def high_vc_stop_link_count(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query("pt_stop_link_vc_result", "SELECT COUNT(*) FROM {} WHERE COALESCE(vc_ratio, 0) > 1.0")


def high_vc_stop_link_ratio(ctx: DiagnosisContext) -> Optional[float]:
    if not ctx.table_exists("pt_stop_link_vc_result"):
        return None
    with ctx.conn.cursor() as cur:
        cur.execute(
            sql.SQL(
                "SELECT COUNT(*), SUM(CASE WHEN COALESCE(vc_ratio, 0) > 1.0 THEN 1 ELSE 0 END) FROM {}"
            ).format(qname(ctx.table("pt_stop_link_vc_result")))
        )
        total, high = cur.fetchone()
    total = float(total or 0)
    if total <= 0:
        return None
    return float(high or 0) / total


def avg_shape_frequency(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query(
        "pt_shape",
        "SELECT AVG(frequency) FROM (SELECT DISTINCT route_id, dir, frequency FROM {}) x WHERE frequency IS NOT NULL",
    )


def max_shape_frequency(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query(
        "pt_shape",
        "SELECT MAX(frequency) FROM (SELECT DISTINCT route_id, dir, frequency FROM {}) x WHERE frequency IS NOT NULL",
    )


def avg_stop_spacing(ctx: DiagnosisContext) -> Optional[float]:
    rows = ctx.route_metrics()
    vals = [row["length_km"] / max(row["stations"] - 1, 1) for row in rows if row.get("stations", 0) > 1]
    return sum(vals) / len(vals) if vals else None


def active_route_count(ctx: DiagnosisContext) -> Optional[float]:
    return ctx.scalar_query(
        "pt_link_result",
        "SELECT COUNT(DISTINCT route_id) FROM {} WHERE link_type='ENROUTE' AND COALESCE(flow, 0) > 0",
    )


def meso_total_routes(ctx: DiagnosisContext) -> Optional[float]:
    return float(len(ctx.route_metrics()))


def meso_avg_mileage(ctx: DiagnosisContext) -> Optional[float]:
    rows = ctx.route_metrics()
    return sum(row["length_km"] for row in rows) / len(rows) if rows else None


def meso_avg_stations(ctx: DiagnosisContext) -> Optional[float]:
    rows = ctx.route_metrics()
    return sum(row["stations"] for row in rows) / len(rows) if rows else None


def meso_max_mileage(ctx: DiagnosisContext) -> Optional[float]:
    rows = ctx.route_metrics()
    return max((row["length_km"] for row in rows), default=None)


def meso_max_stations(ctx: DiagnosisContext) -> Optional[float]:
    rows = ctx.route_metrics()
    return max((row["stations"] for row in rows), default=None)


PT_MACRO_INDICATORS: List[Indicator] = [
    Indicator("pt_macro_total_demand", "公交总需求", "人次", "macro", metric_from_summary("total_demand"), ("pt_summary_result",)),
    Indicator("pt_macro_assigned_demand", "公交已分配需求", "人次", "macro", metric_from_summary("assigned_demand"), ("pt_summary_result",)),
    Indicator("pt_macro_infeasible_flow", "公交不可达需求", "人次", "macro", metric_from_summary("infeasible_flow"), ("pt_summary_result",)),
    Indicator("pt_macro_infeasible_ratio", "公交不可达需求比例", "比例", "macro", infeasible_ratio, ("pt_summary_result",)),
    Indicator("pt_macro_route_count", "公交线路数", "条", "macro", metric_from_summary("num_of_routes", "pt_route", "route_id"), ("pt_route",)),
    Indicator("pt_macro_stop_count", "公交站点数", "个", "macro", metric_from_summary("num_of_stops", "bus_point", "node_id"), ("bus_point",)),
    Indicator("pt_macro_walk_link_count", "公交步行换乘弧数", "条", "macro", walk_link_count, ()),
    Indicator("pt_macro_top_link_flow", "公交最大链路客流", "人次", "macro", top_link_flow, ("pt_link_result",)),
    Indicator("pt_macro_hyperpath_count", "公交超路径数量", "条", "macro", metric_from_summary("num_of_hyperpaths"), ("pt_summary_result",)),
    Indicator("pt_macro_positive_flow_links", "有客流公交链路数", "条", "macro", metric_from_summary("positive_flow_links"), ("pt_summary_result",)),
    Indicator("pt_macro_total_passenger_link_flow", "公交链路客流累计量", "人次·链路", "macro", metric_from_summary("total_passenger_link_flow"), ("pt_summary_result",)),
    Indicator("pt_macro_avg_path_cost", "平均超路径成本", "分钟", "macro", metric_from_summary("avg_path_cost"), ("pt_summary_result",)),
    Indicator("pt_macro_avg_wait_cost", "平均等待成本", "分钟", "macro", metric_from_summary("avg_wait_cost"), ("pt_summary_result",)),
    Indicator("pt_macro_wait_cost_share", "等待成本占比", "比例", "macro", wait_cost_share, ("pt_summary_result",)),
    Indicator("pt_macro_max_path_cost", "最大超路径成本", "分钟", "macro", max_path_cost, ("pt_path_result",)),
    Indicator("pt_macro_avg_path_links", "平均超路径链路数", "条", "macro", avg_path_links, ("pt_path_result",)),
    Indicator("pt_macro_max_stop_link_vc", "最大站间流量饱和度", "V/C", "macro", max_stop_link_vc, ("pt_stop_link_vc_result",)),
    Indicator("pt_macro_avg_stop_link_vc", "平均站间流量饱和度", "V/C", "macro", avg_stop_link_vc, ("pt_stop_link_vc_result",)),
    Indicator("pt_macro_high_vc_stop_link_count", "V/C大于1站间数", "条", "macro", high_vc_stop_link_count, ("pt_stop_link_vc_result",)),
    Indicator("pt_macro_high_vc_stop_link_ratio", "V/C大于1站间占比", "比例", "macro", high_vc_stop_link_ratio, ("pt_stop_link_vc_result",)),
    Indicator("pt_macro_bus_density", "公交线网密度", "km/km²", "macro", macro_input_metric("pt_macro_bus_density"), (PT_MACRO_INPUT_SUFFIX,)),
    Indicator("pt_macro_pt_pop_coverage_500m", "公交500米人口覆盖率", "%", "macro", macro_input_metric("pt_macro_pt_pop_coverage_500m"), (PT_MACRO_INPUT_SUFFIX,)),
    Indicator("pt_macro_city_bus_area_coverage", "公交区域覆盖率", "%", "macro", macro_input_metric("pt_macro_city_bus_area_coverage"), (PT_MACRO_INPUT_SUFFIX,)),
]

PT_MESO_INDICATORS: List[Indicator] = [
    Indicator("pt_meso_total_routes", "线路总数", "条", "meso", meso_total_routes, ("pt_shape", "bus_point")),
    Indicator("pt_meso_avg_mileage", "平均线路里程", "km", "meso", meso_avg_mileage, ("pt_shape", "bus_point")),
    Indicator("pt_meso_avg_stations", "平均站点数", "个", "meso", meso_avg_stations, ("pt_shape", "bus_point")),
    Indicator("pt_meso_max_mileage", "最长线路里程", "km", "meso", meso_max_mileage, ("pt_shape", "bus_point")),
    Indicator("pt_meso_max_stations", "最多站点数", "个", "meso", meso_max_stations, ("pt_shape", "bus_point")),
    Indicator("pt_meso_avg_stop_spacing", "平均站间距", "km", "meso", avg_stop_spacing, ("pt_shape", "bus_point")),
    Indicator("pt_meso_avg_frequency", "平均线路频率", "veh/h", "meso", avg_shape_frequency, ("pt_shape",)),
    Indicator("pt_meso_max_frequency", "最高线路频率", "veh/h", "meso", max_shape_frequency, ("pt_shape",)),
    Indicator("pt_meso_active_routes", "有客流线路数", "条", "meso", active_route_count, ("pt_link_result",)),
]


def select_indicators(module: str, param2: Dict[str, Any]) -> List[Indicator]:
    catalog = PT_MACRO_INDICATORS if module == "pt_macro" else PT_MESO_INDICATORS if module == "pt_meso" else []
    if not catalog:
        raise DiagnosisError(f"暂不支持的诊断模块：{module}")
    requested = set(parse_list(param2.get("indicator_codes")))
    if not requested:
        return catalog
    selected = [item for item in catalog if item.code in requested]
    if not selected:
        raise DiagnosisError("入参错误：indicator_codes 中没有可识别的指标编码")
    return selected


def calculate_indicator(ctx: DiagnosisContext, indicator: Indicator) -> IndicatorResult:
    missing = ctx.missing_tables(indicator.required_tables)
    if missing:
        return IndicatorResult(
            indicator.code,
            indicator.name,
            indicator.unit,
            indicator.dimension,
            ctx.context_key,
            2,
            "输入表缺失，指标已跳过",
            missing_tables=missing,
        )
    if indicator.skip_reason:
        return IndicatorResult(
            indicator.code,
            indicator.name,
            indicator.unit,
            indicator.dimension,
            ctx.context_key,
            2,
            indicator.skip_reason,
        )
    try:
        value = indicator.calculator(ctx)
    except Exception as exc:
        return IndicatorResult(
            indicator.code,
            indicator.name,
            indicator.unit,
            indicator.dimension,
            ctx.context_key,
            0,
            f"指标计算失败：{exc}",
        )
    if value is None or (isinstance(value, float) and (math.isnan(value) or math.isinf(value))):
        return IndicatorResult(
            indicator.code,
            indicator.name,
            indicator.unit,
            indicator.dimension,
            ctx.context_key,
            2,
            "输入数据不足，指标已跳过",
        )
    return IndicatorResult(
        indicator.code,
        indicator.name,
        indicator.unit,
        indicator.dimension,
        ctx.context_key,
        1,
        "指标计算成功",
        float(value),
    )


def ensure_result_table(conn: Any, table: str) -> None:
    with conn.cursor() as cur:
        cur.execute(
            sql.SQL(
                """
                CREATE TABLE IF NOT EXISTS {} (
                    id bigserial PRIMARY KEY,
                    indicator_code varchar NOT NULL,
                    indicator_name text NOT NULL,
                    context_key text NOT NULL DEFAULT '',
                    result_group varchar NOT NULL DEFAULT '',
                    result_key text NOT NULL DEFAULT '',
                    result_item varchar NOT NULL,
                    result_item_name text NOT NULL,
                    result_value_num double precision,
                    result_value_text text,
                    result_unit varchar,
                    sort_order integer NOT NULL DEFAULT 0,
                    created_at timestamp with time zone NOT NULL DEFAULT now()
                )
                """
            ).format(qname(table))
        )
        columns = {
            "indicator_name": "text NOT NULL DEFAULT ''",
            "context_key": "text NOT NULL DEFAULT ''",
            "result_group": "varchar NOT NULL DEFAULT ''",
            "result_key": "text NOT NULL DEFAULT ''",
            "result_item": "varchar NOT NULL DEFAULT ''",
            "result_item_name": "text NOT NULL DEFAULT ''",
            "result_value_num": "double precision",
            "result_value_text": "text",
            "result_unit": "varchar",
            "sort_order": "integer NOT NULL DEFAULT 0",
            "created_at": "timestamp with time zone NOT NULL DEFAULT now()",
        }
        for column_name, column_type in columns.items():
            cur.execute(
                sql.SQL("ALTER TABLE {} ADD COLUMN IF NOT EXISTS {} {}").format(
                    qname(table),
                    sql.Identifier(column_name),
                    sql.SQL(column_type),
                )
            )


def delete_existing_indicator_rows(conn: Any, table: str, indicator_codes: Sequence[str], context_key: str) -> None:
    if not indicator_codes:
        return
    with conn.cursor() as cur:
        cur.execute(
            sql.SQL("DELETE FROM {} WHERE indicator_code = ANY(%s) AND context_key = %s").format(qname(table)),
            (list(indicator_codes), context_key),
        )


def append_results(conn: Any, table: str, results: List[IndicatorResult]) -> None:
    if not results:
        return
    with conn.cursor() as cur:
        insert = sql.SQL(
            """
            INSERT INTO {} (
                indicator_code, indicator_name, context_key, result_group,
                result_key, result_item, result_item_name, result_value_num,
                result_value_text, result_unit, sort_order, created_at
            )
            VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
            """
        ).format(qname(table))
        now = datetime.now()
        for sort_order, item in enumerate(results, start=1):
            text_value = "" if item.status == 1 else item.message
            result_key, result_item = RESULT_ROW_META.get(item.code, (item.code, item.code))
            cur.execute(
                insert,
                (
                    item.code,
                    item.name,
                    item.context_key,
                    item.dimension,
                    result_key,
                    result_item,
                    item.name,
                    item.value,
                    text_value,
                    item.unit,
                    sort_order,
                    now,
                ),
            )


def build_message(module: str, total: int, computed: int, skipped: int, failed: int, reasons: List[str]) -> Tuple[int, str]:
    module_name = "公交宏观诊断" if module == "pt_macro" else "公交中微观诊断"
    if computed == total and failed == 0 and skipped == 0:
        return 1, f"{module_name}成功：{computed}/{total} 项指标已写入结果表"
    unique_reasons = []
    for reason in reasons:
        if reason and reason not in unique_reasons:
            unique_reasons.append(reason)
    reason_text = "；".join(unique_reasons[:3])
    if computed > 0 and failed == 0 and skipped > 0:
        return 2, f"{module_name}部分完成：已算出 {computed}/{total} 项，{skipped} 项因输入数据缺失或口径不满足已跳过。原因：{reason_text}"
    if computed > 0:
        return 1, f"{module_name}部分成功：已算出 {computed} 项，跳过 {skipped} 项，失败 {failed} 项。原因：{reason_text}"
    if skipped > 0 and failed == 0:
        return 2, f"{module_name}部分完成：0/{total} 项算出，{skipped} 项因输入数据缺失或口径不满足已跳过。原因：{reason_text}"
    return -99, f"{module_name}失败：全部 {total} 个指标未算出。原因：{reason_text or '未知错误'}"


def run(module: str, request: Dict[str, str]) -> Dict[str, Any]:
    if psycopg2 is None or sql is None:
        return {
            "code": -99,
            "message": "诊断服务内部错误：Python 环境缺少 psycopg2，无法连接 PostgreSQL",
            "summary.stage": f"diagnosis_{module}",
        }
    try:
        project_id = parse_positive_int(request.get("project_id", ""), "project_id")
        user_id = parse_positive_int(request.get("user_id", ""), "user_id")
        case_id = parse_positive_int(request.get("case_id", ""), "case_id", allow_zero=True)
        param2 = parse_param2(request.get("param2", ""))
        indicators = select_indicators(module, param2)
    except DiagnosisError as exc:
        return {
            "code": -1,
            "message": str(exc),
            "summary.stage": f"diagnosis_{module}",
        }

    conninfo = conninfo_from_request(request.get("param1", ""))
    result_table = f"project{project_id}_user{user_id}_case{case_id}_diagnosis_indicator_result_rows"
    full_result_table = f'{SCHEMA}."{result_table}"'

    try:
        conn = psycopg2.connect(conninfo)
        # Diagnosis treats each indicator independently.  Autocommit prevents one
        # failed SQL probe from leaving the connection in an aborted transaction
        # and blocking the remaining indicators/result rows.
        conn.autocommit = True
        ctx = DiagnosisContext(conn, project_id, user_id, case_id, param2)

        if module == "pt_meso" and ctx.line_ids:
            if not ctx.table_exists("pt_route") and not ctx.table_exists("pt_shape"):
                raise DiagnosisError("输入表缺失：pt_route/pt_shape 均不存在，无法校验 line_ids")
            with conn.cursor() as cur:
                source_suffix = "pt_route" if ctx.table_exists("pt_route") else "pt_shape"
                cur.execute(
                    sql.SQL("SELECT COUNT(DISTINCT route_id::text) FROM {} WHERE route_id::text = ANY(%s)").format(
                        qname(ctx.table(source_suffix))
                    ),
                    (ctx.line_ids,),
                )
                matched = int(cur.fetchone()[0] or 0)
            if matched == 0:
                raise DiagnosisError("入参错误：line_ids 中的线路 ID 在当前公交线路表中不存在")

        results = [calculate_indicator(ctx, indicator) for indicator in indicators]
        ensure_result_table(conn, result_table)
        delete_codes = [item.code for item in results]
        if module == "pt_macro":
            delete_codes.extend(OBSOLETE_PT_MACRO_INDICATORS)
        delete_existing_indicator_rows(conn, result_table, delete_codes, ctx.context_key)
        append_results(conn, result_table, results)
    except DiagnosisError as exc:
        return {
            "code": -1,
            "message": str(exc),
            "data.table.result_table": full_result_table,
            "summary.stage": f"diagnosis_{module}",
        }
    except Exception as exc:
        return {
            "code": -99,
            "message": f"诊断服务内部错误：{exc}",
            "data.table.result_table": full_result_table,
            "summary.stage": f"diagnosis_{module}",
            "summary.attr.traceback": traceback.format_exc(limit=3),
        }
    finally:
        try:
            conn.close()  # type: ignore[name-defined]
        except Exception:
            pass

    computed = sum(1 for item in results if item.status == 1)
    skipped = sum(1 for item in results if item.status == 2)
    failed = sum(1 for item in results if item.status == 0)
    code, message = build_message(module, len(results), computed, skipped, failed, [item.message for item in results if item.status != 1])

    response: Dict[str, Any] = {
        "code": code,
        "message": message,
        "data.table.result_table": full_result_table,
        "data.count.indicator_total": len(results),
        "data.count.indicator_computed": computed,
        "data.count.indicator_skipped": skipped,
        "data.count.indicator_failed": failed,
        "summary.stage": f"diagnosis_{module}",
        "summary.attr.context_key": results[0].context_key if results else "GLOBAL",
        "summary.attr.skipped_indicators": ",".join(item.code for item in results if item.status == 2),
        "summary.attr.failures": ";".join(f"{item.code}:{item.message}" for item in results if item.status == 0),
    }
    for item in results:
        response[f"summary.attr.{item.code}_status"] = item.status
        response[f"summary.attr.{item.code}_message"] = item.message
        if item.missing_tables:
            response[f"summary.attr.{item.code}_missing_tables"] = ",".join(item.missing_tables)
        if item.value is not None:
            response[f"data.metric.{item.code}"] = item.value
    return response


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", required=True)
    parser.add_argument("--repo-root")
    parser.add_argument("--request-file", required=True)
    parser.add_argument("--response-file", required=True)
    args = parser.parse_args(argv)

    request = read_kv(args.request_file)
    response = run(args.module, request)
    write_kv(args.response_file, response)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
