"""从 PostgreSQL 采集现状诊断报告所需的图表与 KPI 数据。"""
from __future__ import annotations

from typing import Any

from . import db
from .indicator_catalog import resolve_indicator_codes, indicator_method_label
from .scope_utils import filter_diagnosis_rows, normalize_scope, scope_label
from .spatial_filter import (
    SpatialScope,
    diagnosis_row_matches_spatial,
    motor_link_filter_sql,
    parse_spatial_scope,
)
from .table_reader import collect_workspace_tables
from .network_viz import collect_network_bundle, resolve_prefixes

MOTOR_TYPE_LABELS: dict[int, str] = {
    1: "快速路",
    2: "主干路",
    3: "次干路",
    4: "支路",
    5: "其他",
    10: "质心连杆",
}


def _motor_type_label(type_code: Any) -> str:
    try:
        return MOTOR_TYPE_LABELS.get(int(type_code), f"Type{type_code}")
    except (TypeError, ValueError):
        return "未标注"


def _safe(v, dec: int = 2) -> float:
    try:
        return round(float(v), dec) if v is not None else 0.0
    except (TypeError, ValueError):
        return 0.0


_MAX_PLAUSIBLE_SPEED_KMH = 180.0
_MAX_PLAUSIBLE_DENSITY_KM2 = 500.0  # km/km²


def _diag_value_utils():
    import sys
    from pathlib import Path

    candidates = [
        Path(__file__).resolve().parents[1] / "src" / "Greedy" / "grpc_server" / "helpers",
        Path("/home/giss/opt_algorithms/tna_total_service/helpers"),
        Path(__file__).resolve().parent,
    ]
    for helpers in candidates:
        if helpers.is_dir() and (helpers / "diagnosis_value_utils.py").is_file():
            p = str(helpers)
            if p not in sys.path:
                sys.path.insert(0, p)
            break
    import diagnosis_value_utils as dv

    return dv


def _diagnosis_row_plausible(row: dict[str, Any]) -> bool:
    """过滤诊断入库异常值，避免 LLM 快照引用荒谬数字。"""
    code = str(row.get("code") or "")
    num = row.get("num")
    if num is None:
        return True
    try:
        v = float(num)
    except (TypeError, ValueError):
        return True
    if code in ("road_macro_average_speed", "road_micro_speed_kmh"):
        return 0 <= v <= _MAX_PLAUSIBLE_SPEED_KMH
    if code in ("road_macro_density", "slow_macro_walk_density", "slow_macro_bike_density"):
        return 0 <= v <= _MAX_PLAUSIBLE_DENSITY_KM2
    return True


def _normalize_base_case_id(pb: dict[str, Any]) -> int | None:
    raw = pb.get("base_case_id")
    if raw is None or str(raw).strip() == "":
        return None
    try:
        cid = int(raw)
    except (TypeError, ValueError):
        return None
    return None if cid <= 0 else cid


def _infer_base_case_id(
    conn, project_id: int, user_id: int, scheme_case_id: int, logs: list[str]
) -> int | None:
    """case_id=0 非基础方案；在同项目已分配方案中选最小有 road_way 的 case 作对比基准。"""
    best: int | None = None
    for cid in range(1, 64):
        if cid == scheme_case_id:
            continue
        prefix = db.build_prefix(project_id, user_id, cid)
        if not db.table_exists(conn, prefix, "road_way"):
            continue
        _, rows = db.query(
            conn, f"SELECT COUNT(*)::int FROM {db.table_name(prefix, 'road_way')}"
        )
        cnt = int(rows[0][0]) if rows else 0
        if cnt > 0 and (best is None or cid < best):
            best = cid
    if best is not None:
        logs.append(f"base_case_id auto-inferred={best} (was 0/missing)")
    return best


def _pick_diag_global_num(rows: list[dict], code: str) -> tuple[float | None, str]:
    dv = _diag_value_utils()
    raw = [
        {
            "indicator_code": r.get("code"),
            "context_key": r.get("context") or "GLOBAL",
            "result_key": r.get("key") or "",
            "result_item": r.get("item") or r.get("key") or "",
            "result_value_num": r.get("num"),
            "result_value_text": r.get("text") or "",
            "result_unit": r.get("unit") or "",
        }
        for r in rows
        if r.get("code") == code
    ]
    p = dv.pick_primary_value(code, raw)
    if p.get("num") is not None:
        return float(p["num"]), str(p.get("unit") or "")
    return None, ""


def collect_motor(
    conn,
    prefix: str,
    logs: list[str],
    spatial: SpatialScope | None = None,
) -> dict[str, Any]:
    tbl = db.table_name(prefix, "road_way")
    spatial = spatial or SpatialScope()
    extra_where, extra_params = motor_link_filter_sql(spatial)
    where_base = "w.type IS NOT NULL"
    if extra_where:
        where_base = f"{where_base} AND ({extra_where})"
    r: dict[str, Any] = {
        "ok": False,
        "type_data": [],
        "total_links": 0,
        "avg_vc": 0.0,
        "avg_speed": 0.0,
        "congested_pct": 0.0,
        "vc_dist": [0, 0, 0, 0],
        "top_sat_links": [],
        "spatial_label": spatial.label,
    }
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT w.type,
                   COUNT(*) AS cnt,
                   ROUND(AVG(w.v_c)::numeric, 4) AS avg_vc,
                   ROUND(AVG(CASE WHEN w.v_c > 0 THEN w.speedlimit ELSE w.speedlimit END)::numeric, 1) AS avg_spd,
                   SUM(CASE WHEN w.v_c < 0.6  THEN 1 ELSE 0 END) AS va,
                   SUM(CASE WHEN w.v_c >= 0.6 AND w.v_c < 0.8 THEN 1 ELSE 0 END) AS vb,
                   SUM(CASE WHEN w.v_c >= 0.8 AND w.v_c < 1.0 THEN 1 ELSE 0 END) AS vc,
                   SUM(CASE WHEN w.v_c >= 1.0 THEN 1 ELSE 0 END) AS vd
            FROM {tbl} w
            WHERE {where_base}
            GROUP BY w.type ORDER BY w.type
            """,
            extra_params or None,
        )
        type_map = MOTOR_TYPE_LABELS
        type_data = []
        vc_dist = [0, 0, 0, 0]
        weighted_vc = weighted_spd = total_cnt = 0
        for row in rows:
            rt, cnt, avg_vc, avg_spd, va, vb, vc, vd = row
            cnt = int(cnt or 0)
            label = type_map.get(int(rt), f"Type{rt}")
            entry: dict[str, Any] = {
                "label": label,
                "count": cnt,
                "avg_vc": _safe(avg_vc, 3),
                "avg_spd": _safe(avg_spd, 1),
            }
            if int(rt) == 10:
                entry["is_centroid_connector"] = True
                entry["note"] = (
                    "交通小区接入路网的质心连杆(type=10)，非道路等级；V/C≈0 为正常现象，"
                    "不得作为断头路/支路改造或拓扑修复对象"
                )
            type_data.append(entry)
            total_cnt += cnt
            weighted_vc += _safe(avg_vc, 3) * cnt
            weighted_spd += _safe(avg_spd, 1) * cnt
            vc_dist[0] += int(va or 0)
            vc_dist[1] += int(vb or 0)
            vc_dist[2] += int(vc or 0)
            vc_dist[3] += int(vd or 0)

        top_where = "v_c IS NOT NULL AND capacity > 0"
        top_params: list[Any] = list(extra_params)
        if extra_where:
            top_where = f"{top_where} AND ({extra_where.replace('w.', '')})"
        _, top_rows = db.query(
            conn,
            f"""
            SELECT link_id,
                   COALESCE(NULLIF(TRIM(name), ''), '路段' || link_id::text) AS link_name,
                   w.type,
                   COALESCE(NULLIF(TRIM(direction), ''), '未标注') AS direction,
                   ROUND(v_c::numeric, 3), ROUND(volume::numeric, 0), ROUND(capacity::numeric, 0)
            FROM {tbl} w
            WHERE {top_where}
            ORDER BY v_c DESC NULLS LAST
            LIMIT 20
            """,
            top_params or None,
        )
        top_sat = [
            {
                "link_id": int(r[0]),
                "name": str(r[1] or f"路段{r[0]}"),
                "type": int(r[2]) if r[2] is not None else None,
                "type_label": _motor_type_label(r[2]),
                "direction": str(r[3] or "未标注"),
                "v_c": _safe(r[4], 3),
                "volume": _safe(r[5], 0),
                "capacity": _safe(r[6], 0),
            }
            for r in top_rows
        ]
        total_links = sum(vc_dist) or total_cnt
        congested = vc_dist[2] + vc_dist[3]
        r.update(
            {
                "ok": True,
                "type_data": type_data,
                "total_links": total_links,
                "avg_vc": round(weighted_vc / max(total_cnt, 1), 3),
                "avg_speed": round(weighted_spd / max(total_cnt, 1), 1),
                "vc_dist": vc_dist,
                "congested_pct": round(congested / max(total_links, 1) * 100, 1),
                "top_sat_links": top_sat,
            }
        )
        logs.append(
            f"motor: links={total_links}, avg_vc={r['avg_vc']}, congested={r['congested_pct']}%"
            + (f" [{spatial.label}]" if not spatial.is_global else "")
        )
    except Exception as e:
        logs.append(f"motor failed: {e}" + (f" (spatial={spatial.label})" if not spatial.is_global else ""))
    return r


def collect_slow(conn, prefix: str, logs: list[str]) -> dict[str, Any]:
    tbl_way = db.table_name(prefix, "slow_road_way")
    tbl_flow = db.table_name(prefix, "slow_greedy_link_flow_results")
    r: dict[str, Any] = {
        "ok": False,
        "type_data": [],
        "network_len_km": 0.0,
        "avg_flow": 0.0,
        "max_flow": 0.0,
        "high_flow_pct": 0.0,
        "top_flow_links": [],
        "top_vc_links": [],
    }
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT w.type, COUNT(*) AS cnt,
                   ROUND(SUM(w.length/1000.0)::numeric, 2) AS len_km,
                   ROUND(AVG(f.flow)::numeric, 1) AS avg_flow,
                   ROUND(MAX(f.flow)::numeric, 1) AS max_flow
            FROM {tbl_way} w
            LEFT JOIN {tbl_flow} f ON w.link_id = f.link_id
            WHERE w.type IS NOT NULL
            GROUP BY w.type ORDER BY w.type
            """,
        )
        type_map = {2: "步行道", 3: "非机动车道", 4: "混合慢行", 10: "其他"}
        type_data = []
        net_len = 0.0
        for row in rows:
            t, cnt, lkm, avg_fl, max_fl = row
            type_data.append(
                {
                    "label": type_map.get(int(t), f"Type{t}"),
                    "count": int(cnt or 0),
                    "len_km": _safe(lkm),
                    "avg_flow": _safe(avg_fl, 1),
                    "max_flow": _safe(max_fl, 1),
                }
            )
            net_len += _safe(lkm)
        _, fr = db.query(
            conn,
            f"""
            SELECT ROUND(AVG(flow)::numeric,1), ROUND(MAX(flow)::numeric,1),
                   COUNT(*), SUM(CASE WHEN flow > 500 THEN 1 ELSE 0 END)
            FROM {tbl_flow} WHERE flow IS NOT NULL
            """,
        )
        gf = fr[0] if fr else (0, 0, 1, 0)
        high_pct = round(_safe(gf[3]) / max(int(gf[2] or 1), 1) * 100, 1)
        r.update(
            {
                "ok": True,
                "type_data": type_data,
                "network_len_km": round(net_len, 2),
                "avg_flow": _safe(gf[0], 1),
                "max_flow": _safe(gf[1], 1),
                "high_flow_pct": high_pct,
            }
        )
        try:
            _, top_flow = db.query(conn, f"SELECT w.link_id, COALESCE(w.name, '') AS name, ROUND(f.flow::numeric,1) AS flow FROM {tbl_flow} f JOIN {tbl_way} w ON w.link_id = f.link_id WHERE COALESCE(w.type,0) <> 10 AND COALESCE(f.flow,0) > 0 ORDER BY f.flow DESC LIMIT 10")
            r["top_flow_links"] = [{"link_id": int(x[0]), "name": x[1] or f"LINK_{x[0]}", "flow": _safe(x[2], 1)} for x in top_flow]
        except Exception as e:
            logs.append(f"slow top_flow failed: {e}")
        try:
            _, top_vc = db.query(conn, f"SELECT link_id, COALESCE(name, '') AS name, ROUND(v_c::numeric,4) AS v_c, ROUND(volume::numeric,1) AS volume, ROUND(capacity::numeric,1) AS capacity FROM {tbl_way} WHERE COALESCE(type,0) <> 10 AND COALESCE(v_c,0) > 0 ORDER BY v_c DESC LIMIT 10")
            r["top_vc_links"] = [{"link_id": int(x[0]), "name": x[1] or f"LINK_{x[0]}", "v_c": _safe(x[2], 4), "volume": _safe(x[3], 1), "capacity": _safe(x[4], 1)} for x in top_vc]
        except Exception as e:
            logs.append(f"slow top_vc failed: {e}")
        logs.append(f"slow: len_km={r['network_len_km']}, avg_flow={r['avg_flow']}")
    except Exception as e:
        logs.append(f"slow failed: {e}")
    return r


def collect_pt(conn, prefix: str, logs: list[str]) -> dict[str, Any]:
    r: dict[str, Any] = {
        "ok": False,
        "route_count": 0,
        "stop_count": 0,
        "summary": {},
        "iter": {},
        "link_data": [],
        "top_links": [],
        "route_flow_top": [],
        "top_vc_links": [],
        "vc_distribution": [],
        "path_summary": {},
        "top_paths": [],
        "enroute_avg": 0.0,
        "max_section_flow": 0.0,
        "missing_tables": [],
    }
    try:
        missing = [
            suffix
            for suffix in (
                "pt_route",
                "bus_point",
                "pt_summary_result",
                "pt_iter_result",
                "pt_link_result",
                "pt_path_result",
                "pt_stop_link_vc_result",
            )
            if not db.table_exists(conn, prefix, suffix)
        ]
        r["missing_tables"] = missing
        if "pt_route" not in missing:
            _, rr = db.query(conn, f"SELECT COUNT(*) FROM {db.table_name(prefix, 'pt_route')}")
            route_cnt = int(rr[0][0]) if rr else 0
        else:
            route_cnt = 0
        if "bus_point" not in missing:
            _, sr = db.query(conn, f"SELECT COUNT(*) FROM {db.table_name(prefix, 'bus_point')}")
            stop_cnt = int(sr[0][0]) if sr else 0
        else:
            stop_cnt = 0

        summary: dict[str, float] = {}
        if "pt_summary_result" not in missing:
            _, sm_rows = db.query(
                conn,
                f"""
                SELECT metric_key, metric_value
                FROM {db.table_name(prefix, 'pt_summary_result')}
                ORDER BY metric_key
                """,
            )
            summary = {str(k): _safe(v, 4) for k, v in sm_rows}
            route_cnt = int(summary.get("num_of_routes") or route_cnt)
            stop_cnt = int(summary.get("num_of_stops") or stop_cnt)

        iter_summary: dict[str, Any] = {}
        if "pt_iter_result" not in missing:
            _, it_rows = db.query(
                conn,
                f"""
                SELECT ROUND(cpu_time::numeric, 4),
                       num_hyperpaths,
                       ROUND(infeasible_flow::numeric, 4)
                FROM {db.table_name(prefix, 'pt_iter_result')}
                LIMIT 1
                """,
            )
            if it_rows:
                cpu_time, num_hyperpaths, infeasible_flow = it_rows[0]
                iter_summary = {
                    "cpu_time": _safe(cpu_time, 4),
                    "num_hyperpaths": int(num_hyperpaths or 0),
                    "infeasible_flow": _safe(infeasible_flow, 4),
                }

        link_data: list[dict[str, Any]] = []
        top_links: list[dict[str, Any]] = []
        route_flow_top: list[dict[str, Any]] = []
        if "pt_link_result" not in missing:
            _, lr = db.query(
                conn,
                f"""
                SELECT link_type, COUNT(*) AS cnt,
                       SUM(CASE WHEN COALESCE(flow, 0) > 0 THEN 1 ELSE 0 END) AS positive_cnt,
                       ROUND(AVG(flow)::numeric,1) AS avg_flow,
                       ROUND(MAX(flow)::numeric,1) AS max_flow,
                       ROUND(SUM(flow)::numeric,1) AS total_flow
                FROM {db.table_name(prefix, 'pt_link_result')}
                WHERE flow IS NOT NULL
                GROUP BY link_type ORDER BY link_type
                """,
            )
            link_data = [
                {
                    "type": str(row[0]),
                    "count": int(row[1] or 0),
                    "positive_count": int(row[2] or 0),
                    "avg_flow": _safe(row[3], 1),
                    "max_flow": _safe(row[4], 1),
                    "total_flow": _safe(row[5], 1),
                }
                for row in lr
            ]
            _, top_rows = db.query(
                conn,
                f"""
                SELECT link_id, link_type, tail_stop_id, head_stop_id,
                       route_id, shape_id, seq,
                       ROUND(flow::numeric, 4) AS flow,
                       ROUND(cost::numeric, 4) AS cost
                FROM {db.table_name(prefix, 'pt_link_result')}
                WHERE COALESCE(flow, 0) > 0
                ORDER BY flow DESC NULLS LAST
                LIMIT 12
                """,
            )
            top_links = [
                {
                    "link_id": int(row[0]),
                    "type": str(row[1]),
                    "from_stop": str(row[2] or ""),
                    "to_stop": str(row[3] or ""),
                    "route_id": str(row[4] or ""),
                    "shape_id": str(row[5] or ""),
                    "seq": int(row[6] or 0),
                    "flow": _safe(row[7], 4),
                    "cost": _safe(row[8], 4),
                }
                for row in top_rows
            ]
            _, route_rows = db.query(
                conn,
                f"""
                SELECT route_id,
                       ROUND(SUM(flow)::numeric, 4) AS total_flow,
                       ROUND(MAX(flow)::numeric, 4) AS max_flow,
                       COUNT(*) AS link_count
                FROM {db.table_name(prefix, 'pt_link_result')}
                WHERE link_type='ENROUTE' AND COALESCE(flow, 0) > 0
                GROUP BY route_id
                ORDER BY total_flow DESC NULLS LAST
                LIMIT 10
                """,
            )
            route_flow_top = [
                {
                    "route_id": str(row[0] or ""),
                    "total_flow": _safe(row[1], 4),
                    "max_flow": _safe(row[2], 4),
                    "link_count": int(row[3] or 0),
                }
                for row in route_rows
            ]

        path_summary: dict[str, Any] = {}
        top_paths: list[dict[str, Any]] = []
        if "pt_path_result" not in missing:
            _, ps = db.query(
                conn,
                f"""
                SELECT COUNT(*) AS path_count,
                       ROUND(SUM(path_flow)::numeric, 4) AS total_path_flow,
                       ROUND(AVG(path_cost)::numeric, 4) AS avg_path_cost,
                       ROUND(MAX(path_cost)::numeric, 4) AS max_path_cost,
                       ROUND(AVG(wait_cost)::numeric, 4) AS avg_wait_cost,
                       ROUND(AVG(num_links)::numeric, 4) AS avg_num_links,
                       MAX(num_links) AS max_num_links
                FROM {db.table_name(prefix, 'pt_path_result')}
                """,
            )
            if ps:
                row = ps[0]
                path_summary = {
                    "path_count": int(row[0] or 0),
                    "total_path_flow": _safe(row[1], 4),
                    "avg_path_cost": _safe(row[2], 4),
                    "max_path_cost": _safe(row[3], 4),
                    "avg_wait_cost": _safe(row[4], 4),
                    "avg_num_links": _safe(row[5], 4),
                    "max_num_links": int(row[6] or 0),
                }
            _, tp = db.query(
                conn,
                f"""
                SELECT origin_stop_id, destination_stop_id, path_index,
                       ROUND(path_flow::numeric, 4),
                       ROUND(path_cost::numeric, 4),
                       ROUND(wait_cost::numeric, 4),
                       num_links
                FROM {db.table_name(prefix, 'pt_path_result')}
                ORDER BY path_flow DESC NULLS LAST
                LIMIT 10
                """,
            )
            top_paths = [
                {
                    "origin_stop": str(row[0] or ""),
                    "destination_stop": str(row[1] or ""),
                    "path_index": int(row[2] or 0),
                    "path_flow": _safe(row[3], 4),
                    "path_cost": _safe(row[4], 4),
                    "wait_cost": _safe(row[5], 4),
                    "num_links": int(row[6] or 0),
                }
                for row in tp
            ]

        top_vc_links: list[dict[str, Any]] = []
        vc_distribution: list[dict[str, Any]] = []
        if "pt_stop_link_vc_result" not in missing:
            _, vc_rows = db.query(
                conn,
                f"""
                SELECT from_stop, to_stop, from_stop_name, to_stop_name,
                       ROUND(flow::numeric, 4), ROUND(capacity::numeric, 4),
                       ROUND(vc_ratio::numeric, 4), route_count, shape_count
                FROM {db.table_name(prefix, 'pt_stop_link_vc_result')}
                ORDER BY vc_ratio DESC NULLS LAST
                LIMIT 10
                """,
            )
            top_vc_links = [
                {
                    "from_stop": str(row[0] or ""),
                    "to_stop": str(row[1] or ""),
                    "from_stop_name": str(row[2] or ""),
                    "to_stop_name": str(row[3] or ""),
                    "flow": _safe(row[4], 4),
                    "capacity": _safe(row[5], 4),
                    "vc_ratio": _safe(row[6], 4),
                    "route_count": int(row[7] or 0),
                    "shape_count": int(row[8] or 0),
                }
                for row in vc_rows
            ]
            _, vc_dist_rows = db.query(
                conn,
                f"""
                SELECT bucket, COUNT(*) AS link_count
                FROM (
                  SELECT CASE
                    WHEN COALESCE(vc_ratio, 0) < 0.8 THEN '<0.8'
                    WHEN COALESCE(vc_ratio, 0) < 1.0 THEN '0.8-1.0'
                    WHEN COALESCE(vc_ratio, 0) < 2.0 THEN '1.0-2.0'
                    WHEN COALESCE(vc_ratio, 0) < 5.0 THEN '2.0-5.0'
                    ELSE '>=5.0'
                  END AS bucket
                  FROM {db.table_name(prefix, 'pt_stop_link_vc_result')}
                ) x
                GROUP BY bucket
                ORDER BY CASE bucket WHEN '<0.8' THEN 1 WHEN '0.8-1.0' THEN 2 WHEN '1.0-2.0' THEN 3 WHEN '2.0-5.0' THEN 4 ELSE 5 END
                """
            )
            vc_distribution = [
                {"bucket": str(row[0]), "link_count": int(row[1] or 0)}
                for row in vc_dist_rows
            ]

        enroute = next((d for d in link_data if d["type"] == "ENROUTE"), {"avg_flow": 0, "max_flow": 0})
        r.update(
            {
                "ok": route_cnt > 0 and stop_cnt > 0 and ("pt_link_result" not in missing or "pt_summary_result" not in missing),
                "route_count": route_cnt,
                "stop_count": stop_cnt,
                "summary": summary,
                "iter": iter_summary,
                "link_data": link_data,
                "top_links": top_links,
                "route_flow_top": route_flow_top,
                "top_vc_links": top_vc_links,
                "vc_distribution": vc_distribution,
                "path_summary": path_summary,
                "top_paths": top_paths,
                "enroute_avg": enroute["avg_flow"],
                "max_section_flow": enroute["max_flow"],
            }
        )
        logs.append(
            f"pt: routes={route_cnt}, stops={stop_cnt}, enroute_avg={r['enroute_avg']}, "
            f"missing={','.join(missing) if missing else 'none'}"
        )
    except Exception as e:
        logs.append(f"pt failed: {e}")
    return r


def _diagnosis_scope_clause(scope: str) -> str:
    """按交通子系统在 SQL 层筛选，避免 LIMIT 后 motor 范围被公交行占满。"""
    if scope == "motor":
        return "(indicator_code NOT LIKE 'pt_%' AND indicator_code NOT LIKE 'slow_%')"
    if scope == "pt":
        return "indicator_code LIKE 'pt_%'"
    if scope == "slow":
        return "indicator_code LIKE 'slow_%'"
    return ""


def collect_diagnosis(
    conn,
    prefix: str,
    logs: list[str],
    limit: int = 12,
    *,
    indicator_codes: list[str] | None = None,
    spatial: SpatialScope | None = None,
    scope: str = "all",
) -> dict[str, Any]:
    spatial = spatial or SpatialScope()
    r: dict[str, Any] = {"ok": False, "rows": [], "requested_codes": indicator_codes or []}
    if not db.table_exists(conn, prefix, "diagnosis_indicator_result_rows"):
        logs.append("diagnosis_indicator_result_rows: table not found")
        return r
    tbl = db.table_name(prefix, "diagnosis_indicator_result_rows")
    try:
        if scope == "pt" and limit < 30:
            limit = 30
        if scope == "slow" and limit < 40:
            limit = 40
        where_parts: list[str] = []
        params: list[Any] = []
        if indicator_codes:
            where_parts.append("indicator_code = ANY(%s)")
            params.append(indicator_codes)
        scope_clause = _diagnosis_scope_clause(scope)
        if scope_clause:
            where_parts.append(scope_clause)
        if scope not in ("pt", "slow"):
            where_parts.append("indicator_code LIKE '%_macro_%'")
        where_sql = (" WHERE " + " AND ".join(where_parts)) if where_parts else ""
        _, rows = db.query(
            conn,
            f"""
            SELECT indicator_code, indicator_name, context_key, result_key, result_item,
                   result_value_num, result_value_text, result_unit
            FROM {tbl}
            {where_sql}
            ORDER BY indicator_code, context_key, result_key, result_item
            """,
            params or None,
        )
        dv = _diag_value_utils()
        raw = [
            {
                "indicator_code": row[0],
                "context_key": row[2] or "",
                "result_key": row[3] or "",
                "result_item": row[4],
                "result_value_num": row[5],
                "result_value_text": row[6],
                "result_unit": row[7],
            }
            for row in rows
        ]
        if scope == "pt":
            filtered = dv.filter_rows_for_report(raw, scope="all", type_filter="pt")
        elif scope == "slow":
            filtered = dv.filter_rows_for_report(raw, scope="all", type_filter="slow")
        else:
            filtered = dv.filter_rows_for_report(raw, scope="macro")
        if not spatial.is_global:
            filtered = [
                x
                for x in filtered
                if diagnosis_row_matches_spatial(
                    {
                        "code": x.get("indicator_code"),
                        "context": x.get("context_key"),
                        "key": x.get("result_key"),
                    },
                    spatial,
                )
            ]
        name_by_code = {row[0]: (row[1] or row[0]) for row in rows}
        if scope in ("pt", "slow"):
            r["rows"] = [
                {
                    "code": x.get("indicator_code"),
                    "name": name_by_code.get(x.get("indicator_code"), x.get("indicator_code")),
                    "context": x.get("context_key") or "GLOBAL",
                    "key": x.get("result_key") or "",
                    "item": x.get("result_item") or x.get("result_key") or "",
                    "num": _safe(x.get("result_value_num"), 4)
                    if x.get("result_value_num") is not None
                    else None,
                    "text": x.get("result_value_text") or "",
                    "unit": x.get("result_unit") or "",
                    "tier": dv.indicator_tier(x.get("indicator_code") or ""),
                }
                for x in filtered
            ]
        else:
            summary = dv.macro_summary_rows(dv.group_by_indicator(filtered))
            r["rows"] = [
                {
                    "code": s["code"],
                    "name": name_by_code.get(s["code"], s["code"]),
                    "context": s["ctx"],
                    "key": s["key"],
                    "item": s.get("item") or s["key"],
                    "num": _safe(s["num"], 4) if s["num"] is not None else None,
                    "text": s.get("text") or "",
                    "unit": s.get("unit") or "",
                    "tier": "macro",
                }
                for s in summary
            ]
        skipped = len(rows) - len(filtered)
        if skipped:
            logs.append(f"diagnosis: ignored {skipped} non-macro/auxiliary rows")
        r["rows"] = r["rows"][:limit]
        r["ok"] = bool(r["rows"])
        logs.append(
            f"diagnosis: {len(r['rows'])} rows"
            + (f" codes={indicator_codes}" if indicator_codes else "")
            + (f" [{spatial.label}]" if not spatial.is_global else "")
        )
    except Exception as e:
        logs.append(f"diagnosis failed: {e}")
    return r


def _motor_summary(conn, prefix: str, spatial: SpatialScope | None = None) -> dict[str, Any]:
    spatial = spatial or SpatialScope()
    tbl = db.table_name(prefix, "road_way")
    extra_where, extra_params = motor_link_filter_sql(spatial)
    where = "v_c IS NOT NULL"
    if extra_where:
        where = f"{where} AND ({extra_where})"
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT COUNT(*),
                   ROUND(AVG(v_c)::numeric, 4),
                   ROUND(AVG(speedlimit)::numeric, 1),
                   SUM(CASE WHEN v_c >= 0.8 THEN 1 ELSE 0 END)
            FROM {tbl} w
            WHERE {where}
            """,
            extra_params or None,
        )
        if not rows:
            return {"ok": False}
        cnt, avg_vc, avg_spd, congested = rows[0]
        cnt = int(cnt or 0)
        return {
            "ok": cnt > 0,
            "total_links": cnt,
            "avg_vc": float(avg_vc or 0),
            "avg_speed": float(avg_spd or 0),
            "congested_links": int(congested or 0),
            "congested_pct": round(int(congested or 0) / max(cnt, 1) * 100, 1),
        }
    except Exception:
        return {"ok": False}


def collect_payload(
    project_id: int,
    user_id: int,
    case_id: int,
    scope: str = "all",
    conn=None,
    *,
    prompt_blocks: dict[str, Any] | None = None,
    include_all_tables: bool = False,
    table_sample_rows: int = 0,
    diagnosis_limit: int = 12,
    **_: Any,
) -> dict[str, Any]:
    """采集完整 payload（不含数据库密码）。支持框选范围与指标筛选。"""
    scope = normalize_scope(scope)
    pb = prompt_blocks or {}
    spatial = parse_spatial_scope(pb)
    indicator_codes = resolve_indicator_codes(pb, scope)

    logs: list[str] = []
    prefix = db.build_prefix(project_id, user_id, case_id)
    own_conn = conn is None
    if own_conn:
        conn = db.connect()

    empty = {"ok": False}
    motor = slow = pt = diag = empty
    network_bundle: dict[str, Any] = {}
    comparison: dict[str, Any] = {"ok": False}
    tables: dict[str, Any] = {"ok": False, "skipped": not include_all_tables}
    try:
        network_bundle = collect_network_bundle(conn, project_id, user_id, case_id, logs)
        px = resolve_prefixes(conn, project_id, user_id, case_id)
        net_prefix = px["network"]

        if scope in ("motor", "all"):
            motor = collect_motor(conn, net_prefix, logs, spatial)
            if not motor.get("ok") and network_bundle.get("motor", {}).get("ok"):
                motor = dict(network_bundle["motor"])
                motor["ok"] = True
                motor["from_structure_fallback"] = True
            base_cid = _normalize_base_case_id(pb)
            fid = str(pb.get("faq_id") or "")
            if base_cid is None and (
                fid == "scheme_vs_base" or fid.startswith("scheme_")
            ):
                base_cid = _infer_base_case_id(conn, project_id, user_id, case_id, logs)
            if base_cid is not None:
                comparison = {"ok": False, "base_case_id": base_cid}
                if db.table_exists(conn, net_prefix, "road_way"):
                    base_prefix = db.build_prefix(project_id, user_id, base_cid)
                    base_net = (
                        base_prefix
                        if db.table_exists(conn, base_prefix, "road_way")
                        else net_prefix
                    )
                    base_s = _motor_summary(conn, base_net, spatial)
                    scheme_s = _motor_summary(conn, net_prefix, spatial)
                    if base_s.get("ok") and scheme_s.get("ok"):
                        comparison = {
                            "ok": True,
                            "avg_vc_delta": round(scheme_s["avg_vc"] - base_s["avg_vc"], 4),
                            "congested_pct_delta": round(
                                scheme_s["congested_pct"] - base_s["congested_pct"], 1
                            ),
                            "base_case_id": base_cid,
                            "base": base_s,
                            "scheme": scheme_s,
                        }
                        logs.append(f"base compare: dvc={comparison['avg_vc_delta']}")
                    else:
                        logs.append(
                            f"base compare skipped: base_ok={base_s.get('ok')} "
                            f"scheme_ok={scheme_s.get('ok')}"
                        )
        if scope in ("slow", "all"):
            slow = collect_slow(conn, prefix, logs)
            if not slow.get("ok") and network_bundle.get("slow", {}).get("ok"):
                slow = dict(network_bundle["slow"])
        if scope in ("pt", "all"):
            pt = collect_pt(conn, prefix, logs)
            if not pt.get("ok") and network_bundle.get("pt", {}).get("ok"):
                pt = dict(network_bundle["pt"])
        diag_scope = scope
        diag = collect_diagnosis(
            conn,
            prefix,
            logs,
            limit=diagnosis_limit,
            indicator_codes=indicator_codes,
            spatial=spatial,
            scope=diag_scope,
        )
        if diag.get("rows"):
            diag["rows"] = filter_diagnosis_rows(diag["rows"], diag_scope)
            diag["ok"] = bool(diag["rows"])
        if slow.get("ok") and diag.get("rows"):
            ap, apu = _pick_diag_global_num(diag["rows"], "slow_macro_area_percap")
            if ap is not None:
                slow["area_percap"] = ap
                slow["area_percap_unit"] = apu or "m²/人"
        tables = (
            collect_workspace_tables(
                conn,
                prefix,
                logs,
                scope,
                include_samples=table_sample_rows > 0,
                sample_rows=table_sample_rows,
            )
            if include_all_tables
            else {"ok": False, "skipped": True}
        )
    finally:
        if own_conn and conn:
            conn.close()

    data_gaps: list[str] = []
    if scope in ("motor", "all") and network_bundle.get("used_network_fallback"):
        data_gaps.append(
            f"方案前缀无 road_way，已回退工具前缀基础路网：{network_bundle['prefixes']['tool']}"
        )
    if scope in ("motor", "all") and not motor.get("ok"):
        data_gaps.append("机动车 road_way 数据不可用（方案与工具前缀均无路网表）")
    if scope in ("slow", "all") and not slow.get("ok"):
        data_gaps.append("慢行 slow_road_way / slow_greedy_link_flow_results 不可用")
    if scope in ("pt", "all") and not pt.get("ok"):
        missing_pt = pt.get("missing_tables") or []
        if missing_pt:
            data_gaps.append(
                "公交数据不完整，缺少表：" + ", ".join(str(x) for x in missing_pt[:8])
            )
        else:
            data_gaps.append("公交 pt_route / pt_link_result 不可用")
    if not diag.get("ok"):
        if indicator_codes:
            labels = [indicator_method_label(c) for c in indicator_codes[:6]]
            data_gaps.append(
                f"所选诊断指标无数据：{', '.join(labels)}（请先执行对应宏观/中微观诊断计算）"
            )
        else:
            data_gaps.append("诊断指标表为空或不存在，建议先执行机动车/慢行/公交宏观诊断")

    return {
        "meta": {
            "project_id": project_id,
            "user_id": user_id,
            "case_id": case_id,
            "prefix": prefix,
            "scope": scope,
            "title": f"AI智能诊断报告（{scope_label(scope)}）— 项目{project_id} 用户{user_id} 方案{case_id}",
            "scope_label": scope_label(scope),
            "spatial_scope": spatial.to_dict(),
            "indicator_codes": indicator_codes,
            "scheme_type": scope,
        },
        "motor": motor,
        "slow": slow,
        "pt": pt,
        "diagnosis": diag,
        "base_scheme_comparison": comparison,
        "network_bundle": network_bundle,
        "tables": tables,
        "data_gaps": data_gaps,
        "logs": logs,
        "kpi_summary_text": _build_kpi_summary_text(
            motor, slow, pt, diag, tables, scope, comparison
        ),
    }


def _build_kpi_summary_text(
    motor,
    slow,
    pt,
    diag,
    tables=None,
    scope: str = "all",
    comparison=None,
) -> str:
    lines = []
    if motor.get("ok") and scope in ("motor", "all"):
        lines.append(
            f"机动车：路段 {motor['total_links']} 条，平均 V/C={motor['avg_vc']}，"
            f"拥堵路段占比 {motor['congested_pct']}%"
        )
    if slow.get("ok") and scope in ("slow", "all"):
        slow_line = (
            f"慢行：路网 {slow['network_len_km']} km，平均流量 {slow['avg_flow']} 人次/h，"
            f"高流量路段占比 {slow['high_flow_pct']}%"
        )
        if slow.get("area_percap") is not None:
            slow_line += f"，人均慢行道路面 {slow['area_percap']} {slow.get('area_percap_unit', 'm²/人')}"
        lines.append(slow_line)
    if pt.get("ok") and scope in ("pt", "all"):
        summary = pt.get("summary") or {}
        total_demand = summary.get("total_demand")
        assigned_demand = summary.get("assigned_demand")
        infeasible_flow = summary.get("infeasible_flow") or (pt.get("iter") or {}).get("infeasible_flow")
        infeasible_ratio = None
        if total_demand and total_demand > 0 and infeasible_flow is not None:
            infeasible_ratio = round(float(infeasible_flow) / float(total_demand) * 100, 2)
        demand_bits = ""
        if total_demand is not None:
            demand_bits = f"，总需求 {total_demand} 人次"
            if assigned_demand is not None:
                demand_bits += f"，已分配 {assigned_demand} 人次"
            if infeasible_flow is not None:
                demand_bits += f"，不可达 {infeasible_flow} 人次"
                if infeasible_ratio is not None:
                    demand_bits += f"（{infeasible_ratio}%）"
        lines.append(
            f"公交：线路 {pt['route_count']} 条、站点 {pt['stop_count']} 个，"
            f"断面平均客流 {pt['enroute_avg']} 人次/班{demand_bits}"
        )
    if comparison and comparison.get("ok"):
        lines.append(
            f"相对基础方案：ΔV/C={comparison.get('avg_vc_delta')}，"
            f"Δ拥堵占比={comparison.get('congested_pct_delta')}%"
        )
    if diag.get("ok"):
        codes = diag.get("requested_codes") or []
        suffix = f"（筛选 {len(codes)} 项）" if codes else ""
        lines.append(f"诊断指标：已加载 {len(diag['rows'])} 条记录{suffix}")
    if tables and tables.get("ok"):
        reg = tables.get("registered") or {}
        n_exist = sum(1 for v in reg.values() if v.get("exists"))
        lines.append(
            f"数据表（server-workspace 登记）：已检查 {len(reg)} 张，存在 {n_exist} 张"
        )
        if tables.get("missing_suffixes"):
            lines.append(f"缺失表后缀：{', '.join(tables['missing_suffixes'][:8])}")
    return "\n".join(lines)
