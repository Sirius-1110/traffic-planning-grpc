"""质心连杆独立服务 — SQL 与 Greedy TNM_Net CentroidPrebuild 对齐（阶段1）。"""
from __future__ import annotations

import json
import os
from typing import Any, Callable

import psycopg2


def _qt(name: str) -> str:
    if "." in name:
        schema, table = name.split(".", 1)
        return f'"{schema}"."{table}"'
    return f'"{name}"'


def _load_db_conf(param1: str = "") -> dict[str, Any]:
    if param1 and param1.strip():
        kv: dict[str, str] = {}
        for part in param1.replace(";", "\n").splitlines():
            part = part.strip()
            if "=" in part and not part.startswith("#"):
                k, v = part.split("=", 1)
                kv[k.strip()] = v.strip()
        if kv:
            out = {k: v for k, v in kv.items() if k in ("host", "port", "dbname", "user", "password", "role")}
            if "port" in out:
                out["port"] = int(out["port"])
            return out
        return {"dsn": param1}
    for path in (
        os.environ.get("TNA_DB_CONF", ""),
        "/home/giss/opt_algorithms/db.conf",
        "/opt/algorithms/db.conf",
    ):
        if path and os.path.isfile(path):
            kv = {}
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
            out = {k: v for k, v in kv.items() if k in ("host", "port", "dbname", "user", "password", "role")}
            if "port" in out:
                out["port"] = int(out["port"])
            return out
    raise FileNotFoundError("未找到 db.conf")


def _connect(db_kw: dict[str, Any]):
    dsn = str(db_kw.get("dsn", "")).strip()
    if dsn:
        return psycopg2.connect(dsn)
    return psycopg2.connect(
        host=db_kw.get("host", "localhost"),
        port=db_kw.get("port", 5432),
        dbname=db_kw.get("dbname", "postgres"),
        user=db_kw.get("user", "postgres"),
        password=db_kw.get("password", ""),
    )


def _parse_param2(param2: str) -> dict[str, Any]:
    extra: dict[str, Any] = {}
    if param2 and param2.strip():
        try:
            extra = json.loads(param2)
        except json.JSONDecodeError:
            extra = {}
    ap = extra.get("algorithm_params") if isinstance(extra.get("algorithm_params"), dict) else {}

    def pick(*keys: str, default: Any = None) -> Any:
        for k in keys:
            if k in extra and extra[k] is not None:
                return extra[k]
            if k in ap and ap[k] is not None:
                return ap[k]
        return default

    max_conn = int(pick("max_connectors_per_zone", default=5) or 5)
    max_conn = max(1, min(20, max_conn))
    force = pick("force_rebuild", default=False)
    if isinstance(force, str):
        force = force.lower() not in ("0", "false", "no")
    network_kind = str(pick("network_kind", default="motor")).strip().lower()
    table_prefix_mode = str(pick("table_prefix_mode", default="tool")).strip().lower()

    # Backward-compatible shorthand used by the C++ gRPC auto-prebuild path.
    # Existing local_bridge only passes table_prefix_mode / connector mode /
    # max_connectors_per_zone, so "scheme_slow" lets slow assignment reuse the
    # same bridge without changing the C++/header ABI:
    #   scheme      -> project..._road_way
    #   scheme_slow -> project..._slow_road_way
    #   tool_slow   -> project..._slow_road_way under tool prefix
    if table_prefix_mode in ("scheme_slow", "slow_scheme"):
        table_prefix_mode = "scheme"
        network_kind = "slow"
    elif table_prefix_mode in ("tool_slow", "slow_tool"):
        table_prefix_mode = "tool"
        network_kind = "slow"

    return {
        "network_kind": network_kind,
        "table_prefix_mode": table_prefix_mode,
        "connector_mode": str(pick("connector_mode", default="multi_osm")).strip().lower(),
        "max_connectors_per_zone": max_conn,
        "force_rebuild": bool(force),
    }


def _build_prefix(project_id: int, user_id: int, case_id: int, table_prefix_mode: str) -> str:
    if table_prefix_mode == "tool":
        return f"project{project_id}_user{user_id}_"
    return f"project{project_id}_user{user_id}_case{case_id}_"


def _table_names(schema: str, prefix: str, network_kind: str) -> dict[str, str]:
    way_suffix = "slow_road_way" if network_kind == "slow" else "road_way"
    return {
        "road_way": f"{schema}.{prefix}{way_suffix}",
        "road_point": f"{schema}.{prefix}road_point",
        "road_community": f"{schema}.{prefix}road_community",
    }


def _column_exists(cur, schema: str, table: str, column: str) -> bool:
    cur.execute(
        """
        SELECT 1 FROM information_schema.columns
        WHERE table_schema=%s AND table_name=%s AND column_name=%s LIMIT 1
        """,
        (schema, table, column),
    )
    return cur.fetchone() is not None


def _table_exists(cur, schema: str, table: str) -> bool:
    cur.execute(
        """
        SELECT 1 FROM information_schema.tables
        WHERE table_schema=%s AND table_name=%s LIMIT 1
        """,
        (schema, table),
    )
    return cur.fetchone() is not None


def _single_insert_sql(way_q: str, point_q: str, community_q: str, zones_cte: str) -> str:
    return f"""
WITH bm AS (
  SELECT GREATEST(
    COALESCE(MAX(init_node),0),
    COALESCE(MAX(term_node),0),
    COALESCE((SELECT MAX(node_id) FROM {point_q}),0)
  ) AS base_max FROM {way_q} WHERE "type" <> 10
),
lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM {way_q}),
{zones_cte}
base AS (
  SELECT (z.area_id::bigint + bm.base_max + 1)::bigint AS init_node, n.node_id::bigint AS term_node,
         9999999::float8 AS capacity, 0.15::float8 AS b, 4::float8 AS power, 0::float8 AS toll, 10::bigint AS type,
         0.000001::float8 AS fft, 30::float8 AS speedlimit,
         (ST_Distance(ST_Transform(z.gc, 3857), ST_Transform(n.geometry, 3857)) / 1000)::numeric(10,4) AS length,
         ST_MakeLine(z.gc, n.geometry) AS geometry,
         z.area_id::bigint AS centroid_matched_node
  FROM zones z
  CROSS JOIN bm
  CROSS JOIN LATERAL (
    SELECT node_id, geometry FROM {point_q} ORDER BY z.gc <-> geometry LIMIT 1
  ) AS n
),
both_dirs AS (
  SELECT * FROM base
  UNION ALL
  SELECT term_node, init_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node FROM base
)
INSERT INTO {way_q} (link_id, init_node, term_node, capacity, b, power, toll, "type", fft, speedlimit, length, geometry, centroid_matched_node)
SELECT (lm.link_max + row_number() OVER ())::bigint, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node
FROM both_dirs, lm
"""


def _multi_insert_sql(way_q: str, point_q: str, community_q: str, zones_cte: str, max_conn: int, has_osm: bool) -> str:
    osm_key = (
        'COALESCE(NULLIF(w."link_osmid"::text, \'\'), w."link_id"::text)'
        if has_osm
        else 'w."link_id"::text'
    )
    return f"""
WITH bm AS (
  SELECT GREATEST(
    COALESCE(MAX(init_node),0),
    COALESCE(MAX(term_node),0),
    COALESCE((SELECT MAX(node_id) FROM {point_q}),0)
  ) AS base_max FROM {way_q} WHERE "type" <> 10
),
lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM {way_q}),
{zones_cte}
link_candidates AS (
  SELECT z.area_id, w.link_id, w.init_node, w.term_node, w.geometry AS link_geom,
         w."type"::int AS road_type,
         {osm_key} AS osm_key,
         ST_Distance(ST_Transform(z.gc, 3857), ST_Transform(w.geometry, 3857)) AS dist_m,
         z.gc,
         CASE WHEN ST_Distance(ST_Transform(z.gc,3857), ST_Transform(ST_StartPoint(w.geometry),3857))
                   <= ST_Distance(ST_Transform(z.gc,3857), ST_Transform(ST_EndPoint(w.geometry),3857))
              THEN w.init_node ELSE w.term_node END AS anchor_node
  FROM zones z
  CROSS JOIN bm
  JOIN {way_q} w ON w."type" IN (1,2,3) AND w.geometry IS NOT NULL
),
link_dedup AS (
  SELECT DISTINCT ON (area_id, osm_key)
         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node, road_type
  FROM link_candidates
  ORDER BY area_id, osm_key, dist_m
),
anchor_dedup AS (
  SELECT DISTINCT ON (area_id, anchor_node)
         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node, road_type
  FROM link_dedup
  ORDER BY area_id, anchor_node, dist_m
),
mandatory_by_type AS (
  SELECT DISTINCT ON (area_id, road_type)
         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node, road_type
  FROM anchor_dedup
  WHERE road_type IN (1,2,3)
  ORDER BY area_id, road_type, dist_m
),
mandatory_cnt AS (
  SELECT area_id, COUNT(*)::int AS n_mandatory FROM mandatory_by_type GROUP BY area_id
),
extra_candidates AS (
  SELECT a.*, ROW_NUMBER() OVER (PARTITION BY a.area_id ORDER BY a.dist_m) AS rn
  FROM anchor_dedup a
  WHERE NOT EXISTS (
    SELECT 1 FROM mandatory_by_type m WHERE m.area_id = a.area_id AND m.link_id = a.link_id
  )
),
picked AS (
  SELECT area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node
  FROM mandatory_by_type
  UNION ALL
  SELECT e.area_id, e.link_id, e.init_node, e.term_node, e.link_geom, e.dist_m, e.gc, e.osm_key, e.anchor_node
  FROM extra_candidates e
  JOIN mandatory_cnt mc ON mc.area_id = e.area_id
  WHERE e.rn <= GREATEST(0, {max_conn} - mc.n_mandatory)
),
anchors AS (
  SELECT p.area_id, p.gc, p.dist_m, p.anchor_node,
         ST_MakeLine(p.gc, ST_ClosestPoint(p.link_geom, p.gc)) AS geometry
  FROM picked p
),
base AS (
  SELECT (a.area_id + bm.base_max + 1)::bigint AS init_node, a.anchor_node::bigint AS term_node,
         9999999::float8 AS capacity, 0.15::float8 AS b, 4::float8 AS power, 0::float8 AS toll, 10::bigint AS type,
         0.000001::float8 AS fft, 30::float8 AS speedlimit,
         (a.dist_m / 1000)::numeric(10,4) AS length,
         a.geometry,
         a.area_id AS centroid_matched_node
  FROM anchors a
  CROSS JOIN bm
),
both_dirs AS (
  SELECT * FROM base
  UNION ALL
  SELECT term_node, init_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node FROM base
)
INSERT INTO {way_q} (link_id, init_node, term_node, capacity, b, power, toll, "type", fft, speedlimit, length, geometry, centroid_matched_node)
SELECT (lm.link_max + row_number() OVER ())::bigint, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node
FROM both_dirs, lm
"""


def run_build_centroid_connectors(
    project_id: int,
    user_id: int,
    case_id: int,
    param1: str,
    param2: str,
    on_progress: Callable[[int, str], None] | None = None,
) -> tuple[int, str, dict[str, Any]]:
    if project_id <= 0 or user_id <= 0:
        return -1, "参数错误：project_id、user_id 须为正整数。", {}

    cfg = _parse_param2(param2)
    if cfg["table_prefix_mode"] != "tool" and case_id < 0:
        return -1, "方案前缀须 case_id >= 0。", {}

    db_kw = _load_db_conf(param1)
    schema = str(db_kw.get("role") or os.environ.get("TNA_DB_SCHEMA") or "user_project").strip()
    prefix = _build_prefix(project_id, user_id, case_id, cfg["table_prefix_mode"])
    tables = _table_names(schema, prefix, cfg["network_kind"])
    way = tables["road_way"]
    point = tables["road_point"]
    community = tables["road_community"]
    way_schema, way_table = way.split(".", 1)
    point_schema, point_table = point.split(".", 1)
    comm_schema, comm_table = community.split(".", 1)

    def prog(pct: int, title: str) -> None:
        if on_progress:
            on_progress(pct, title)

    prog(5, "正在校验路网与小区表...")
    conn = _connect(db_kw)
    try:
        with conn.cursor() as cur:
            if not _table_exists(cur, way_schema, way_table):
                return -2, f"路网表不存在：{way}", {}
            if not _table_exists(cur, comm_schema, comm_table):
                return -2, f"交通小区表不存在：{community}", {}
            if not _table_exists(cur, point_schema, point_table):
                return -2, f"路网节点表不存在：{point}", {}
            for col in ("geometry", "type", "centroid_matched_node"):
                if not _column_exists(cur, way_schema, way_table, col):
                    return -2, f"路网表缺少字段 {col}：{way}", {}
            if not _column_exists(cur, comm_schema, comm_table, "geometry"):
                return -2, f"交通小区表缺少 geometry：{community}", {}
            if not _column_exists(cur, point_schema, point_table, "geometry"):
                return -2, f"路网节点表缺少 geometry：{point}", {}

            cur.execute(f'SELECT COUNT(*)::int FROM {_qt(way)} WHERE geometry IS NULL')
            geom_null = int(cur.fetchone()[0])
            if geom_null > 0:
                return -2, f"路网表存在 geometry 为 NULL 的路段（{geom_null} 条）。", {}

            cur.execute(f'SELECT COUNT(*)::int FROM {_qt(way)} WHERE "type" = 10')
            existing = int(cur.fetchone()[0])

            if existing > 0 and not cfg["force_rebuild"]:
                cur.execute(
                    f"""
                    SELECT COALESCE(MAX(cnt), 0)::int FROM (
                      SELECT COUNT(*)::int AS cnt FROM {_qt(way)}
                      WHERE "type" = 10 GROUP BY centroid_matched_node
                    ) s
                    """
                )
                max_per_zone = int(cur.fetchone()[0])
                prog(100, "已有质心连杆，跳过重建")
                return 1, "成功（跳过重建）", {
                    "data": {
                        "tables": {"network_table": way},
                        "counts": {
                            "type10_rows": existing,
                            "type10_before": existing,
                            "type10_after": existing,
                            "skipped_rebuild": 1,
                            "max_links_per_zone": max_per_zone,
                        },
                    },
                    "summary": {
                        "stage": "build_centroid_connectors",
                        "attributes": {
                            "connector_mode": cfg["connector_mode"],
                            "table_prefix": cfg["table_prefix_mode"],
                            "network_kind": cfg["network_kind"],
                            "force_rebuild": "0",
                        },
                    },
                }

            prog(20, "正在清理旧质心连杆...")
            cur.execute(f'DELETE FROM {_qt(way)} WHERE "type" = 10')

            zones_cte = (
                "zones AS (\n"
                f"  SELECT ra.area_id::bigint AS area_id, ST_Centroid(ra.\"geometry\") AS gc\n"
                f"  FROM {_qt(community)} ra\n"
                "  WHERE ra.\"geometry\" IS NOT NULL\n"
                "),\n"
            )
            way_q, point_q, comm_q = _qt(way), _qt(point), _qt(community)
            has_osm = _column_exists(cur, way_schema, way_table, "link_osmid")
            prog(50, "正在写入质心连杆 type=10...")
            if cfg["connector_mode"] == "multi_osm":
                sql = _multi_insert_sql(way_q, point_q, comm_q, zones_cte, cfg["max_connectors_per_zone"], has_osm)
            else:
                sql = _single_insert_sql(way_q, point_q, comm_q, zones_cte)
            cur.execute(sql)
            cur.execute(f'SELECT COUNT(*)::int FROM {_qt(way)} WHERE "type" = 10')
            after = int(cur.fetchone()[0])
            if after <= 0:
                conn.rollback()
                return -2, "质心连杆生成后 type=10 数量为 0，请检查小区面与路网几何。", {}

        conn.commit()
        prog(95, "质心连杆写入完成")
        return 1, "成功", {
            "data": {
                "tables": {"network_table": way},
                "counts": {
                    "type10_rows": after,
                    "type10_before": existing,
                    "type10_after": after,
                    "skipped_rebuild": 0,
                },
            },
            "summary": {
                "stage": "build_centroid_connectors",
                "attributes": {
                    "connector_mode": cfg["connector_mode"],
                    "table_prefix": cfg["table_prefix_mode"],
                    "network_kind": cfg["network_kind"],
                    "max_connectors_per_zone": str(cfg["max_connectors_per_zone"]),
                    "force_rebuild": "1" if cfg["force_rebuild"] else "0",
                },
            },
        }
    except Exception as e:
        conn.rollback()
        return -99, f"质心连杆服务异常：{e.__class__.__name__}: {e}", {}
    finally:
        conn.close()
