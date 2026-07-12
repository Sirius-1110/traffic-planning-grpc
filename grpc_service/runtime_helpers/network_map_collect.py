"""路网 / OD 期望线采集（gRPC 报告与 AI 报告共用）。"""
from __future__ import annotations

import json
import os
from typing import Any


def _map_max_links() -> int:
    """报告地图最大路段数；0 表示不限制。默认 10000（原 1200 会导致大项目缺段）。"""
    raw = os.environ.get("TNA_MAP_MAX_LINKS", "10000").strip()
    try:
        n = int(raw)
    except ValueError:
        n = 10000
    return max(0, n)


def _table_name(prefix: str, suffix: str) -> str:
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


def _query(conn, sql: str, params=None):
    with conn.cursor() as cur:
        cur.execute(sql, params)
        cols = [d[0] for d in cur.description] if cur.description else []
        rows = cur.fetchall()
    return cols, rows


def build_scheme_prefix(project_id: int, user_id: int, case_id: int) -> str:
    return f"project{project_id}_user{user_id}_case{case_id}_"


class _DbShim:
    table_name = staticmethod(_table_name)
    table_exists = staticmethod(_table_exists)
    query = staticmethod(_query)

    @staticmethod
    def build_prefix(project_id: int, user_id: int, case_id: int) -> str:
        return build_scheme_prefix(project_id, user_id, case_id)


db = _DbShim()

_TYPE_MAP_MOTOR = {1: "快速路", 2: "主干路", 3: "次干路", 4: "支路", 5: "其他"}


def build_tool_prefix(project_id: int, user_id: int) -> str:
    return f"project{project_id}_user{user_id}_"


def resolve_prefixes(conn, project_id: int, user_id: int, case_id: int) -> dict[str, str]:
    """方案前缀优先；缺路网/OD 时回退工具前缀。"""
    scheme = db.build_prefix(project_id, user_id, case_id)
    tool = build_tool_prefix(project_id, user_id)
    network = scheme if db.table_exists(conn, scheme, "road_way") else tool
    od = scheme if db.table_exists(conn, scheme, "other_od") else tool
    return {"scheme": scheme, "tool": tool, "network": network, "od": od}


def _column_exists(conn, prefix: str, suffix: str, col: str) -> bool:
    tbl = f"{prefix}{suffix}"
    _, rows = db.query(
        conn,
        """
        SELECT 1 FROM information_schema.columns
        WHERE table_schema='user_project' AND table_name=%s AND column_name=%s LIMIT 1
        """,
        (tbl, col),
    )
    return bool(rows)


def _safe_float(v, dec: int = 2) -> float:
    try:
        return round(float(v), dec) if v is not None else 0.0
    except (TypeError, ValueError):
        return 0.0


def collect_motor_structure(
    conn, prefix: str, logs: list[str], *, source_label: str = ""
) -> dict[str, Any]:
    """机动车路网结构统计（无分配时也可出图）。"""
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "source_label": source_label or prefix,
        "has_flow": False,
        "type_data": [],
        "total_links": 0,
        "total_length_km": 0.0,
        "vc_dist": [0, 0, 0, 0],
        "avg_vc": 0.0,
        "congested_pct": 0.0,
        "avg_speed": 0.0,
        "top_sat_links": [],
    }
    if not db.table_exists(conn, prefix, "road_way"):
        return r
    has_vc = _column_exists(conn, prefix, "road_way", "v_c")
    has_vol = _column_exists(conn, prefix, "road_way", "volume")
    has_flow = has_vc and has_vol
    r["has_flow"] = has_flow
    tbl = db.table_name(prefix, "road_way")
    try:
        if has_flow:
            sql = f"""
                SELECT type, COUNT(*) AS cnt,
                       ROUND(SUM(COALESCE(length,0))::numeric, 0) AS total_len,
                       ROUND(AVG(v_c)::numeric, 4) AS avg_vc,
                       ROUND(AVG(speedlimit)::numeric, 1) AS avg_spd
                FROM {tbl} WHERE type IS NOT NULL GROUP BY type ORDER BY type
            """
        else:
            sql = f"""
                SELECT type, COUNT(*) AS cnt,
                       ROUND(SUM(COALESCE(length,0))::numeric, 0) AS total_len,
                       NULL::numeric, NULL::numeric
                FROM {tbl} WHERE type IS NOT NULL GROUP BY type ORDER BY type
            """
        _, rows = db.query(conn, sql)
        type_data = []
        total_cnt = total_len = 0.0
        w_vc = w_spd = 0.0
        for rt, cnt, length, avg_vc, avg_spd in rows:
            cnt_i = int(cnt or 0)
            label = _TYPE_MAP_MOTOR.get(int(rt), f"Type{rt}")
            type_data.append(
                {
                    "label": label,
                    "count": cnt_i,
                    "length_km": round(float(length or 0) / 1000, 1),
                    "avg_vc": _safe_float(avg_vc, 3),
                    "avg_spd": _safe_float(avg_spd, 1),
                }
            )
            total_cnt += cnt_i
            total_len += float(length or 0)
            if has_flow:
                w_vc += _safe_float(avg_vc, 3) * cnt_i
                w_spd += _safe_float(avg_spd, 1) * cnt_i

        r["type_data"] = type_data
        r["total_links"] = int(total_cnt)
        r["total_length_km"] = round(total_len / 1000, 1)

        if has_flow:
            _, vc_rows = db.query(
                conn,
                f"""
                SELECT
                  SUM(CASE WHEN v_c < 0.6 THEN 1 ELSE 0 END),
                  SUM(CASE WHEN v_c >= 0.6 AND v_c < 0.8 THEN 1 ELSE 0 END),
                  SUM(CASE WHEN v_c >= 0.8 AND v_c < 1.0 THEN 1 ELSE 0 END),
                  SUM(CASE WHEN v_c >= 1.0 THEN 1 ELSE 0 END),
                  COUNT(*) FILTER (WHERE v_c IS NOT NULL)
                FROM {tbl}
                """,
            )
            if vc_rows and vc_rows[0][-1]:
                a, b, c, d, valid = vc_rows[0]
                r["vc_dist"] = [int(a or 0), int(b or 0), int(c or 0), int(d or 0)]
                valid = int(valid or 0)
                congested = r["vc_dist"][2] + r["vc_dist"][3]
                r["congested_pct"] = round(congested / max(valid, 1) * 100, 1)
                r["avg_vc"] = round(w_vc / max(total_cnt, 1), 3)
                r["avg_speed"] = round(w_spd / max(total_cnt, 1), 1)
            has_name = _column_exists(conn, prefix, "road_way", "name")
            name_sel = (
                "COALESCE(NULLIF(TRIM(name), ''), '路段' || link_id::text)"
                if has_name
                else "'路段' || link_id::text"
            )
            _, top_rows = db.query(
                conn,
                f"""
                SELECT link_id, {name_sel}, ROUND(v_c::numeric,3),
                       ROUND(volume::numeric,0), ROUND(capacity::numeric,0)
                FROM {tbl} WHERE v_c IS NOT NULL AND capacity > 0
                ORDER BY v_c DESC NULLS LAST LIMIT 15
                """,
            )
            r["top_sat_links"] = [
                {
                    "link_id": int(x[0]),
                    "name": str(x[1] or f"路段{x[0]}"),
                    "v_c": _safe_float(x[2], 3),
                    "volume": _safe_float(x[3], 0),
                    "capacity": _safe_float(x[4], 0),
                }
                for x in top_rows
            ]
        r["ok"] = r["total_links"] > 0
        logs.append(
            f"network structure [{prefix}]: {r['total_links']} links, {r['total_length_km']} km"
            + (" (flow)" if has_flow else " (structure only)")
        )
    except Exception as e:
        logs.append(f"network structure failed [{prefix}]: {e}")
    return r


def collect_slow_structure(conn, prefix: str, logs: list[str]) -> dict[str, Any]:
    r = {"ok": False, "total_links": 0, "total_length_km": 0.0, "type_data": []}
    if not db.table_exists(conn, prefix, "slow_road_way"):
        return r
    tbl = db.table_name(prefix, "slow_road_way")
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT type, COUNT(*), ROUND(SUM(COALESCE(length,0))::numeric,0)
            FROM {tbl} GROUP BY type ORDER BY type
            """,
        )
        total = 0
        total_len = 0.0
        for rt, cnt, ln in rows:
            cnt_i = int(cnt or 0)
            total += cnt_i
            total_len += float(ln or 0)
            r["type_data"].append(
                {"label": f"慢行Type{rt}", "count": cnt_i, "length_km": round(float(ln or 0) / 1000, 1)}
            )
        r["total_links"] = total
        r["total_length_km"] = round(total_len / 1000, 1)
        r["ok"] = total > 0
        logs.append(f"slow structure: {total} links")
    except Exception as e:
        logs.append(f"slow structure failed: {e}")
    return r


def collect_pt_structure(conn, prefix: str, logs: list[str]) -> dict[str, Any]:
    r = {"ok": False, "route_count": 0, "stop_count": 0}
    if db.table_exists(conn, prefix, "pt_route"):
        try:
            _, rows = db.query(conn, f"SELECT COUNT(*) FROM {db.table_name(prefix, 'pt_route')}")
            r["route_count"] = int(rows[0][0] or 0)
            r["ok"] = r["route_count"] > 0
        except Exception as e:
            logs.append(f"pt route failed: {e}")
    if db.table_exists(conn, prefix, "pt_stop"):
        try:
            _, rows = db.query(conn, f"SELECT COUNT(*) FROM {db.table_name(prefix, 'pt_stop')}")
            r["stop_count"] = int(rows[0][0] or 0)
            r["ok"] = r["ok"] or r["stop_count"] > 0
        except Exception:
            pass
    if r["ok"]:
        logs.append(f"pt structure: routes={r['route_count']} stops={r['stop_count']}")
    return r


def _geojson_to_rings(geo: dict) -> list[list[list[float]]]:
    """GeoJSON Polygon/MultiPolygon → Leaflet 外环列表 [[lat,lng], ...]。"""
    gtype = geo.get("type")
    rings: list[list[list[float]]] = []
    if gtype == "Polygon":
        coords = geo.get("coordinates") or []
        if coords:
            rings.append([[float(p[1]), float(p[0])] for p in coords[0] if len(p) >= 2])
    elif gtype == "MultiPolygon":
        for poly in geo.get("coordinates") or []:
            if poly and poly[0]:
                rings.append([[float(p[1]), float(p[0])] for p in poly[0] if len(p) >= 2])
    return rings


def _bounds_from_line_coords(line_coords: list[list]) -> dict[str, Any]:
    lats: list[float] = []
    lons: list[float] = []
    for coords in line_coords:
        for c in coords or []:
            if len(c) >= 2:
                lons.append(float(c[0]))
                lats.append(float(c[1]))
    if not lats:
        return {"bounds": None, "center": [30.67, 104.06]}
    return {
        "bounds": [[min(lats), min(lons)], [max(lats), max(lons)]],
        "center": [sum(lats) / len(lats), sum(lons) / len(lons)],
    }


def _coords_valid(coords: list) -> bool:
    if not coords or len(coords) < 2:
        return False
    a, b = coords[0], coords[-1]
    if len(a) < 2 or len(b) < 2:
        return False
    return abs(a[0] - b[0]) > 1e-7 or abs(a[1] - b[1]) > 1e-7


def collect_od_summary(
    conn,
    prefix: str,
    logs: list[str],
    *,
    net_prefix: str | None = None,
    limit: int = 40,
) -> dict[str, Any]:
    """OD 期望线：优先用分配路网 type=10 质心连杆，其次 road_community 质心。

    other_od.od_centroid_line_geom 可能来自过期的 road_community_centeroid（与当前路网错位），
    仅在与路网范围相交时作为兜底。
    """
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "net_prefix": net_prefix or prefix,
        "pair_count": 0,
        "total_demand": 0.0,
        "top_pairs": [],
        "lines": [],
        "line_sources": {"connector": 0, "community": 0, "od_geom": 0},
    }
    if not db.table_exists(conn, prefix, "other_od"):
        return r
    od_tbl = db.table_name(prefix, "other_od")
    net_p = net_prefix or prefix
    rw_tbl = (
        db.table_name(net_p, "road_way")
        if db.table_exists(conn, net_p, "road_way")
        else ""
    )
    comm_tbl = (
        db.table_name(net_p, "road_community")
        if db.table_exists(conn, net_p, "road_community")
        else ""
    )
    try:
        _, cnt_rows = db.query(conn, f"SELECT COUNT(*), COALESCE(SUM(demand),0) FROM {od_tbl}")
        r["pair_count"] = int(cnt_rows[0][0] or 0)
        r["total_demand"] = _safe_float(cnt_rows[0][1], 1)
        has_od_geom = _column_exists(conn, prefix, "other_od", "od_centroid_line_geom")
        od_geom_col = "o.od_centroid_line_geom" if has_od_geom else "NULL::geometry"
        net_extent = (
            f"(SELECT ST_Extent(geometry) FROM {rw_tbl} "
            f"WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10)"
            if rw_tbl
            else "NULL::geometry"
        )
        connector_line = "NULL::geometry"
        community_line = "NULL::geometry"
        if rw_tbl:
            connector_line = """
              CASE WHEN z1.g IS NOT NULL AND z2.g IS NOT NULL THEN
                ST_MakeLine(ST_Centroid(z1.g), ST_Centroid(z2.g))
              END
            """
        if comm_tbl:
            community_line = """
              CASE WHEN rc1.g IS NOT NULL AND rc2.g IS NOT NULL THEN
                ST_MakeLine(ST_Centroid(rc1.g), ST_Centroid(rc2.g))
              END
            """
        validated_od_geom = f"""
          CASE
            WHEN {od_geom_col} IS NOT NULL
                 AND ST_Length({od_geom_col}::geography) > 50
                 AND {net_extent} IS NOT NULL
                 AND ST_Intersects({od_geom_col}, {net_extent})
            THEN {od_geom_col}
          END
        """ if has_od_geom and rw_tbl else "NULL::geometry"

        lateral_z1 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {rw_tbl} z
              WHERE z.type = 10 AND z.centroid_matched_node = o.f_id
                AND z.geometry IS NOT NULL
              ORDER BY z.link_id LIMIT 1
            ) z1 ON true"""
            if rw_tbl
            else ""
        )
        lateral_z2 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {rw_tbl} z
              WHERE z.type = 10 AND z.centroid_matched_node = o.t_id
                AND z.geometry IS NOT NULL
              ORDER BY z.link_id LIMIT 1
            ) z2 ON true"""
            if rw_tbl
            else ""
        )
        lateral_rc1 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {comm_tbl} rc
              WHERE rc.area_id = o.f_id AND rc.geometry IS NOT NULL
              ORDER BY rc.area_id LIMIT 1
            ) rc1 ON true"""
            if comm_tbl
            else ""
        )
        lateral_rc2 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {comm_tbl} rc
              WHERE rc.area_id = o.t_id AND rc.geometry IS NOT NULL
              ORDER BY rc.area_id LIMIT 1
            ) rc2 ON true"""
            if comm_tbl
            else ""
        )

        src_parts = ["WHEN o.f_id IS NOT DISTINCT FROM o.t_id THEN 'intra'"]
        if rw_tbl:
            src_parts.append("WHEN z1.g IS NOT NULL AND z2.g IS NOT NULL THEN 'connector'")
        if comm_tbl:
            src_parts.append("WHEN rc1.g IS NOT NULL AND rc2.g IS NOT NULL THEN 'community'")
        if has_od_geom and rw_tbl:
            src_parts.append(f"WHEN {validated_od_geom} IS NOT NULL THEN 'od_geom'")
        src_parts.append("ELSE 'none'")
        src_sql = "CASE " + " ".join(src_parts) + " END"

        _, rows = db.query(
            conn,
            f"""
            SELECT o.f_id, o.t_id, o.demand,
                   ST_AsGeoJSON(
                     CASE WHEN o.f_id IS DISTINCT FROM o.t_id THEN
                       COALESCE({connector_line}, {community_line}, {validated_od_geom})
                     END
                   ) AS gj,
                   {src_sql} AS src
            FROM {od_tbl} o
            {lateral_z1}
            {lateral_z2}
            {lateral_rc1}
            {lateral_rc2}
            ORDER BY o.demand DESC NULLS LAST
            LIMIT {int(limit)}
            """,
        )
        src_labels = {
            "connector": "质心连杆(type=10)",
            "community": "交通小区质心",
            "od_geom": "OD表期望线",
        }
        for f_id, t_id, demand, gj, src in rows:
            pair = {
                "f_id": int(f_id) if f_id is not None else 0,
                "t_id": int(t_id) if t_id is not None else 0,
                "demand": _safe_float(demand, 1),
            }
            r["top_pairs"].append(pair)
            if gj and src not in ("none", "intra"):
                try:
                    geo = json.loads(gj)
                    coords = geo.get("coordinates") or []
                    if _coords_valid(coords):
                        if src in r["line_sources"]:
                            r["line_sources"][src] += 1
                        r["lines"].append(
                            {
                                "f_id": pair["f_id"],
                                "t_id": pair["t_id"],
                                "demand": pair["demand"],
                                "label": f"O{pair['f_id']}→D{pair['t_id']}",
                                "source": src,
                                "source_label": src_labels.get(src, src),
                                "coordinates": coords,
                            }
                        )
                except json.JSONDecodeError:
                    pass
        if r["lines"]:
            od_map = _bounds_from_line_coords([ln["coordinates"] for ln in r["lines"]])
            r["map_bounds"] = od_map["bounds"]
            r["map_center"] = od_map["center"]
            demands = [ln["demand"] for ln in r["lines"]]
            r["demand_min"] = min(demands)
            r["demand_max"] = max(demands)
        r["ok"] = r["pair_count"] > 0
        logs.append(
            f"OD [{prefix}] net={net_p}: {r['pair_count']} pairs, lines {len(r['lines'])} "
            f"(connector={r['line_sources']['connector']}, "
            f"community={r['line_sources']['community']}, od_geom={r['line_sources']['od_geom']})"
        )
    except Exception as e:
        logs.append(f"OD failed [{prefix}]: {e}")
    return r


def collect_centroid_layer(conn, prefix: str, logs: list[str]) -> dict[str, Any]:
    """质心连杆 type=10 + 交通小区质心点（与分配路网同源）。"""
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "connectors": [],
        "zones": [],
        "zone_polygons": [],
    }
    if not db.table_exists(conn, prefix, "road_way"):
        return r
    rw = db.table_name(prefix, "road_way")
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT link_id, centroid_matched_node, ST_AsGeoJSON(geometry)
            FROM {rw}
            WHERE type = 10 AND geometry IS NOT NULL
            ORDER BY centroid_matched_node, link_id
            """,
        )
        for link_id, zone_id, gj in rows:
            if not gj:
                continue
            try:
                geo = json.loads(gj)
                coords = geo.get("coordinates") or []
                if geo.get("type") == "LineString":
                    segs = [coords]
                elif geo.get("type") == "MultiLineString":
                    segs = coords
                else:
                    continue
                for seg in segs:
                    if len(seg) < 2:
                        continue
                    r["connectors"].append(
                        {
                            "link_id": int(link_id),
                            "zone_id": int(zone_id) if zone_id is not None else None,
                            "latlng": [[p[1], p[0]] for p in seg if len(p) >= 2],
                        }
                    )
            except json.JSONDecodeError:
                pass
        if db.table_exists(conn, prefix, "road_community"):
            comm = db.table_name(prefix, "road_community")
            has_name = _column_exists(conn, prefix, "road_community", "name")
            name_sel = (
                "COALESCE(NULLIF(TRIM(name), ''), '小区' || area_id::text)"
                if has_name
                else "'小区' || area_id::text"
            )
            _, zrows = db.query(
                conn,
                f"""
                SELECT area_id, {name_sel},
                       ROUND(ST_Y(ST_Centroid(geometry))::numeric, 6),
                       ROUND(ST_X(ST_Centroid(geometry))::numeric, 6),
                       ST_AsGeoJSON(geometry)
                FROM {comm}
                WHERE geometry IS NOT NULL
                ORDER BY area_id
                """,
            )
            for area_id, zname, lat, lon, gj in zrows:
                entry: dict[str, Any] = {
                    "area_id": int(area_id),
                    "name": str(zname or f"小区{area_id}"),
                    "latlng": [float(lat), float(lon)],
                    "rings": [],
                }
                if gj:
                    try:
                        entry["rings"] = _geojson_to_rings(json.loads(gj))
                    except json.JSONDecodeError:
                        pass
                r["zones"].append(entry)
                if entry["rings"]:
                    r["zone_polygons"].append(
                        {"area_id": entry["area_id"], "name": entry["name"], "rings": entry["rings"]}
                    )
        r["ok"] = bool(r["connectors"] or r["zones"] or r["zone_polygons"])
        logs.append(
            f"centroid layer [{prefix}]: connectors={len(r['connectors'])}, "
            f"zones={len(r['zones'])}, polygons={len(r['zone_polygons'])}"
        )
    except Exception as e:
        logs.append(f"centroid layer failed [{prefix}]: {e}")
    return r


def _assignment_flow_status_slow(conn, prefix: str) -> dict[str, Any]:
    """慢行地图分配状态：优先检查 slow_road_way.volume / capacity_slow_adj。"""
    status: dict[str, Any] = {
        "flow_rows": 0,
        "road_vol_rows": 0,
        "has_flow": False,
        "note": "",
    }
    try:
        if db.table_exists(conn, prefix, "slow_road_way") and _column_exists(
            conn, prefix, "slow_road_way", "volume"
        ):
            _, rows = db.query(
                conn,
                f"SELECT COUNT(*) FROM {db.table_name(prefix, 'slow_road_way')} "
                "WHERE COALESCE(volume, 0) > 0 AND COALESCE(type, 0) <> 10",
            )
            status["road_vol_rows"] = int(rows[0][0] or 0)
            status["flow_rows"] = status["road_vol_rows"]
    except Exception as e:
        status["note"] = f"慢行分配状态检查失败：{e}"
    status["has_flow"] = status["road_vol_rows"] > 0
    if not status["has_flow"] and not status["note"]:
        status["note"] = "慢行分配流量为空；地图仅展示慢行路网拓扑，不按 V/C 着色。"
    return status


def collect_slow_map_links(
    conn, prefix: str, logs: list[str], *, max_links: int | None = None
) -> dict[str, Any]:
    """慢行报告专用：从 slow_road_way 绘制路网流量 / V/C。"""
    cap = _map_max_links() if max_links is None else max(0, int(max_links))
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "source_table": f"{prefix}slow_road_way",
        "center": [30.67, 104.06],
        "bounds": None,
        "links": [],
        "assignment": _assignment_flow_status_slow(conn, prefix),
    }
    if not db.table_exists(conn, prefix, "slow_road_way"):
        logs.append(f"slow map links [{prefix}]: slow_road_way not found")
        return r
    if not _column_exists(conn, prefix, "slow_road_way", "geometry"):
        logs.append(f"slow map links [{prefix}]: slow_road_way no geometry column")
        return r
    tbl = db.table_name(prefix, "slow_road_way")
    has_name = _column_exists(conn, prefix, "slow_road_way", "name")
    has_volume = _column_exists(conn, prefix, "slow_road_way", "volume")
    has_cap_adj = _column_exists(conn, prefix, "slow_road_way", "capacity_slow_adj")
    has_capacity = _column_exists(conn, prefix, "slow_road_way", "capacity")
    has_vc = _column_exists(conn, prefix, "slow_road_way", "v_c")
    name_sel = (
        "COALESCE(NULLIF(TRIM(name), ''), '慢行路段' || link_id::text)"
        if has_name
        else "'慢行路段' || link_id::text"
    )
    if has_volume and has_cap_adj:
        vc_sel = "CASE WHEN COALESCE(volume,0)>0 AND COALESCE(capacity_slow_adj,0)>0 THEN (volume/capacity_slow_adj)::float ELSE NULL END"
    elif has_volume and has_capacity:
        vc_sel = "CASE WHEN COALESCE(volume,0)>0 AND COALESCE(capacity,0)>0 THEN (volume/capacity)::float ELSE NULL END"
    elif has_vc:
        vc_sel = "CASE WHEN COALESCE(v_c,0)>0 THEN v_c::float ELSE NULL END"
    else:
        vc_sel = "NULL::float"
    vol_sel = (
        "COALESCE(volume, 0)"
        if _column_exists(conn, prefix, "slow_road_way", "volume")
        else "0"
    )
    try:
        _, cnt_row = db.query(
            conn,
            f"SELECT COUNT(*) FROM {tbl} WHERE geometry IS NOT NULL AND COALESCE(type,0) <> 10",
        )
        total = int(cnt_row[0][0] or 0)
        fetch_n = total if cap == 0 else (total if total <= cap else cap)
        limit_sql = "" if cap == 0 or fetch_n >= total else f"LIMIT {int(fetch_n)}"
        _, rows = db.query(
            conn,
            f"""
            SELECT link_id, {name_sel}, {vc_sel}, {vol_sel}, ST_AsGeoJSON(geometry)
            FROM {tbl}
            WHERE geometry IS NOT NULL AND COALESCE(type,0) <> 10
            ORDER BY ({vol_sel})::float DESC NULLS LAST,
                     CASE WHEN {vc_sel} IS NULL THEN 1 ELSE 0 END ASC,
                     ({vc_sel})::float DESC NULLS LAST,
                     link_id
            {limit_sql}
            """,
        )
        lats, lons = [], []
        for link_id, link_name, vc, volume, gj in rows:
            if not gj:
                continue
            try:
                geo = json.loads(gj)
            except json.JSONDecodeError:
                continue
            coords = geo.get("coordinates") or []
            if geo.get("type") == "MultiLineString":
                seg_list = coords
            elif geo.get("type") == "LineString":
                seg_list = [coords]
            else:
                continue
            for seg in seg_list:
                if not seg or len(seg) < 2:
                    continue
                latlng = [[p[1], p[0]] for p in seg if len(p) >= 2]
                for p in seg:
                    if len(p) >= 2:
                        lons.append(p[0])
                        lats.append(p[1])
                r["links"].append(
                    {
                        "link_id": int(link_id),
                        "name": str(link_name or f"慢行路段{link_id}"),
                        "v_c": _safe_float(vc, 3) if vc is not None else None,
                        "volume": _safe_float(volume, 0),
                        "latlng": latlng,
                        "dir": "forward",
                    }
                )
        if lats and lons:
            r["center"] = [sum(lats) / len(lats), sum(lons) / len(lons)]
            r["bounds"] = [[min(lats), min(lons)], [max(lats), max(lons)]]
        vc_links = sum(1 for lk in r["links"] if lk.get("v_c") is not None)
        r["assignment"]["vc_link_count"] = vc_links
        r["assignment"]["has_vc_coloring"] = bool(vc_links)
        r["ok"] = bool(r["links"])
        truncated = total > len(r["links"])
        r["assignment"]["total_geom"] = total
        r["assignment"]["truncated"] = truncated
        if truncated:
            r["assignment"]["truncate_note"] = (
                f"路网共 {total} 条可绘制路段，地图展示 {len(r['links'])} 条"
                f"（上限 {cap if cap else '无'}，已优先保留有流量/高 V/C 路段）。"
            )
        logs.append(
            f"slow map links [{prefix}slow_road_way]: {len(r['links'])}/{total} segments, "
            f"has_flow={r['assignment'].get('has_flow')}, vc_links={vc_links}"
            + (f", truncated cap={cap}" if truncated else "")
        )
    except Exception as e:
        logs.append(f"slow map links failed [{prefix}slow_road_way]: {e}")
    return r


def collect_slow_od_summary(
    conn, prefix: str, logs: list[str], *, limit: int = 40
) -> dict[str, Any]:
    """慢行报告专用：从 slow_other_od 绘制 OD 期望线，优先使用 slow_road_way type=10。"""
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "source_table": f"{prefix}slow_other_od",
        "net_prefix": prefix,
        "pair_count": 0,
        "total_demand": 0.0,
        "top_pairs": [],
        "lines": [],
        "line_sources": {"connector": 0, "community": 0, "od_geom": 0},
    }
    if not db.table_exists(conn, prefix, "slow_other_od"):
        logs.append(f"slow OD [{prefix}]: slow_other_od not found")
        return r
    od_tbl = db.table_name(prefix, "slow_other_od")
    rw_tbl = db.table_name(prefix, "slow_road_way") if db.table_exists(conn, prefix, "slow_road_way") else ""
    comm_tbl = (
        db.table_name(prefix, "slow_road_community")
        if db.table_exists(conn, prefix, "slow_road_community")
        else ""
    )
    try:
        _, cnt_rows = db.query(conn, f"SELECT COUNT(*), COALESCE(SUM(demand),0) FROM {od_tbl}")
        r["pair_count"] = int(cnt_rows[0][0] or 0)
        r["total_demand"] = _safe_float(cnt_rows[0][1], 1)
        connector_line = "NULL::geometry"
        community_line = "NULL::geometry"
        if rw_tbl:
            connector_line = """
              CASE WHEN z1.g IS NOT NULL AND z2.g IS NOT NULL THEN
                ST_MakeLine(ST_Centroid(z1.g), ST_Centroid(z2.g))
              END
            """
        if comm_tbl:
            community_line = """
              CASE WHEN rc1.g IS NOT NULL AND rc2.g IS NOT NULL THEN
                ST_MakeLine(ST_Centroid(rc1.g), ST_Centroid(rc2.g))
              END
            """
        lateral_z1 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {rw_tbl} z
              WHERE z.type = 10 AND z.centroid_matched_node = o.f_id
                AND z.geometry IS NOT NULL
              ORDER BY z.link_id LIMIT 1
            ) z1 ON true"""
            if rw_tbl
            else ""
        )
        lateral_z2 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {rw_tbl} z
              WHERE z.type = 10 AND z.centroid_matched_node = o.t_id
                AND z.geometry IS NOT NULL
              ORDER BY z.link_id LIMIT 1
            ) z2 ON true"""
            if rw_tbl
            else ""
        )
        lateral_rc1 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {comm_tbl} rc
              WHERE rc.area_id = o.f_id AND rc.geometry IS NOT NULL
              ORDER BY rc.area_id LIMIT 1
            ) rc1 ON true"""
            if comm_tbl
            else ""
        )
        lateral_rc2 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {comm_tbl} rc
              WHERE rc.area_id = o.t_id AND rc.geometry IS NOT NULL
              ORDER BY rc.area_id LIMIT 1
            ) rc2 ON true"""
            if comm_tbl
            else ""
        )
        src_parts = ["WHEN o.f_id IS NOT DISTINCT FROM o.t_id THEN 'intra'"]
        if rw_tbl:
            src_parts.append("WHEN z1.g IS NOT NULL AND z2.g IS NOT NULL THEN 'connector'")
        if comm_tbl:
            src_parts.append("WHEN rc1.g IS NOT NULL AND rc2.g IS NOT NULL THEN 'community'")
        src_parts.append("ELSE 'none'")
        src_sql = "CASE " + " ".join(src_parts) + " END"
        _, rows = db.query(
            conn,
            f"""
            SELECT o.f_id, o.t_id, o.demand,
                   ST_AsGeoJSON(
                     CASE WHEN o.f_id IS DISTINCT FROM o.t_id THEN
                       COALESCE({connector_line}, {community_line})
                     END
                   ) AS gj,
                   {src_sql} AS src
            FROM {od_tbl} o
            {lateral_z1}
            {lateral_z2}
            {lateral_rc1}
            {lateral_rc2}
            ORDER BY o.demand DESC NULLS LAST
            LIMIT {int(limit)}
            """,
        )
        src_labels = {
            "connector": "慢行质心连杆(type=10)",
            "community": "慢行交通小区质心",
        }
        for f_id, t_id, demand, gj, src in rows:
            pair = {
                "f_id": int(f_id) if f_id is not None else 0,
                "t_id": int(t_id) if t_id is not None else 0,
                "demand": _safe_float(demand, 1),
            }
            r["top_pairs"].append(pair)
            if gj and src not in ("none", "intra"):
                try:
                    geo = json.loads(gj)
                    coords = geo.get("coordinates") or []
                    if _coords_valid(coords):
                        if src in r["line_sources"]:
                            r["line_sources"][src] += 1
                        r["lines"].append(
                            {
                                "f_id": pair["f_id"],
                                "t_id": pair["t_id"],
                                "demand": pair["demand"],
                                "label": f"O{pair['f_id']}→D{pair['t_id']}",
                                "source": src,
                                "source_label": src_labels.get(src, src),
                                "coordinates": coords,
                            }
                        )
                except json.JSONDecodeError:
                    pass
        if r["lines"]:
            od_map = _bounds_from_line_coords([ln["coordinates"] for ln in r["lines"]])
            r["map_bounds"] = od_map["bounds"]
            r["map_center"] = od_map["center"]
            demands = [ln["demand"] for ln in r["lines"]]
            r["demand_min"] = min(demands)
            r["demand_max"] = max(demands)
        r["ok"] = r["pair_count"] > 0
        logs.append(
            f"slow OD [{prefix}slow_other_od] net={prefix}slow_road_way: "
            f"{r['pair_count']} pairs, lines {len(r['lines'])} "
            f"(connector={r['line_sources']['connector']}, "
            f"community={r['line_sources']['community']}, od_geom={r['line_sources']['od_geom']})"
        )
    except Exception as e:
        logs.append(f"slow OD failed [{prefix}slow_other_od]: {e}")
    return r


def collect_slow_centroid_layer(conn, prefix: str, logs: list[str]) -> dict[str, Any]:
    """慢行报告专用：从 slow_road_way type=10 + slow_road_community 绘制小区和质心连杆。"""
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "source_table": f"{prefix}slow_road_way",
        "connectors": [],
        "zones": [],
        "zone_polygons": [],
    }
    if not db.table_exists(conn, prefix, "slow_road_way"):
        return r
    rw = db.table_name(prefix, "slow_road_way")
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT link_id, centroid_matched_node, ST_AsGeoJSON(geometry)
            FROM {rw}
            WHERE type = 10 AND geometry IS NOT NULL
            ORDER BY centroid_matched_node, link_id
            """,
        )
        for link_id, zone_id, gj in rows:
            if not gj:
                continue
            try:
                geo = json.loads(gj)
                coords = geo.get("coordinates") or []
                segs = [coords] if geo.get("type") == "LineString" else coords
                for seg in segs:
                    if len(seg) < 2:
                        continue
                    r["connectors"].append(
                        {
                            "link_id": int(link_id),
                            "zone_id": int(zone_id) if zone_id is not None else None,
                            "latlng": [[p[1], p[0]] for p in seg if len(p) >= 2],
                        }
                    )
            except json.JSONDecodeError:
                pass
        if db.table_exists(conn, prefix, "slow_road_community"):
            comm = db.table_name(prefix, "slow_road_community")
            has_name = _column_exists(conn, prefix, "slow_road_community", "name")
            name_sel = (
                "COALESCE(NULLIF(TRIM(name), ''), '慢行小区' || area_id::text)"
                if has_name
                else "'慢行小区' || area_id::text"
            )
            _, zrows = db.query(
                conn,
                f"""
                SELECT area_id, {name_sel},
                       ROUND(ST_Y(ST_Centroid(geometry))::numeric, 6),
                       ROUND(ST_X(ST_Centroid(geometry))::numeric, 6),
                       ST_AsGeoJSON(geometry)
                FROM {comm}
                WHERE geometry IS NOT NULL
                ORDER BY area_id
                """,
            )
            for area_id, zname, lat, lon, gj in zrows:
                entry: dict[str, Any] = {
                    "area_id": int(area_id),
                    "name": str(zname or f"慢行小区{area_id}"),
                    "latlng": [float(lat), float(lon)],
                    "rings": [],
                }
                if gj:
                    try:
                        entry["rings"] = _geojson_to_rings(json.loads(gj))
                    except json.JSONDecodeError:
                        pass
                r["zones"].append(entry)
                if entry["rings"]:
                    r["zone_polygons"].append(
                        {"area_id": entry["area_id"], "name": entry["name"], "rings": entry["rings"]}
                    )
        r["ok"] = bool(r["connectors"] or r["zones"] or r["zone_polygons"])
        logs.append(
            f"slow centroid layer [{prefix}slow_road_way]: connectors={len(r['connectors'])}, "
            f"zones={len(r['zones'])}, polygons={len(r['zone_polygons'])}"
        )
    except Exception as e:
        logs.append(f"slow centroid layer failed [{prefix}slow_road_way]: {e}")
    return r


def _assignment_flow_status(conn, prefix: str) -> dict[str, Any]:
    """判断方案是否已有有效分配流量（无流量则地图仅绘拓扑，不着色 V/C）。"""
    status: dict[str, Any] = {
        "flow_rows": 0,
        "road_vol_rows": 0,
        "has_flow": False,
        "note": "",
    }
    try:
        if db.table_exists(conn, prefix, "greedy_link_flow_results"):
            _, rows = db.query(
                conn,
                f"SELECT COUNT(*) FROM {db.table_name(prefix, 'greedy_link_flow_results')} "
                "WHERE COALESCE(flow, 0) > 0",
            )
            status["flow_rows"] = int(rows[0][0] or 0)
        if db.table_exists(conn, prefix, "road_way") and _column_exists(
            conn, prefix, "road_way", "volume"
        ):
            _, rows = db.query(
                conn,
                f"SELECT COUNT(*) FROM {db.table_name(prefix, 'road_way')} "
                "WHERE COALESCE(volume, 0) > 0",
            )
            status["road_vol_rows"] = int(rows[0][0] or 0)
    except Exception:
        pass
    status["has_flow"] = status["flow_rows"] > 0 or status["road_vol_rows"] > 0
    if not status["has_flow"]:
        status["note"] = (
            "交通分配未成功或流量为空（greedy_link_flow_results 无有效 flow，"
            "road_way.volume 均为 0）。地图仅展示路网拓扑，不按 V/C 着色；"
            "请检查 OD 可达性、质心连杆与路网连通性后重新执行 base_motor_network / review_road。"
        )
    return status


def collect_map_links(
    conn, prefix: str, logs: list[str], *, max_links: int | None = None, with_vc: bool = True
) -> dict[str, Any]:
    cap = _map_max_links() if max_links is None else max(0, int(max_links))
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "center": [30.67, 104.06],
        "bounds": None,
        "links": [],
        "assignment": _assignment_flow_status(conn, prefix),
    }
    has_flow = bool(r["assignment"].get("has_flow"))
    if not db.table_exists(conn, prefix, "road_way"):
        return r
    if not _column_exists(conn, prefix, "road_way", "geometry"):
        logs.append("road_way: no geometry column")
        return r
    road_tbl = db.table_name(prefix, "road_way")
    has_vc_col = with_vc and _column_exists(conn, prefix, "road_way", "v_c")
    has_name = _column_exists(conn, prefix, "road_way", "name")
    has_init = _column_exists(conn, prefix, "road_way", "init_node")
    has_term = _column_exists(conn, prefix, "road_way", "term_node")
    has_osmid = _column_exists(conn, prefix, "road_way", "link_osmid")
    node_sel = (
        "r.init_node, r.term_node"
        if has_init and has_term
        else "NULL::bigint, NULL::bigint"
    )
    osmid_sel = "r.link_osmid" if has_osmid else "NULL::bigint"
    has_greedy = (
        has_flow
        and db.table_exists(conn, prefix, "greedy_link_flow_results")
        and _column_exists(conn, prefix, "greedy_link_flow_results", "flow")
        and _column_exists(conn, prefix, "greedy_link_flow_results", "capacity")
    )
    name_sel = (
        "COALESCE(NULLIF(TRIM(r.name), ''), '路段' || r.link_id::text)"
        if has_name
        else "'路段' || r.link_id::text"
    )
    if has_greedy:
        greedy_tbl = db.table_name(prefix, "greedy_link_flow_results")
        vc_sel = (
            "CASE WHEN COALESCE(g.flow, 0) > 0 AND COALESCE(g.capacity, 0) > 0 "
            "THEN (g.flow / g.capacity)::float ELSE NULL END"
        )
        vol_sel = "COALESCE(g.flow, r.volume, 0)"
        from_sql = f"{road_tbl} r LEFT JOIN {greedy_tbl} g ON g.link_id = r.link_id"
        geom_sel = "ST_AsGeoJSON(r.geometry)"
        where_sql = "r.geometry IS NOT NULL AND COALESCE(r.type, 0) <> 10"
        id_sel = "r.link_id"
    elif has_flow and has_vc_col:
        vc_sel = "CASE WHEN COALESCE(r.volume, 0) > 0 THEN r.v_c ELSE NULL END"
        vol_sel = "COALESCE(r.volume, 0)"
        from_sql = f"{road_tbl} r"
        geom_sel = "ST_AsGeoJSON(r.geometry)"
        where_sql = "r.geometry IS NOT NULL AND COALESCE(r.type, 0) <> 10"
        id_sel = "r.link_id"
    else:
        vc_sel = "NULL::float"
        vol_sel = "0"
        from_sql = f"{road_tbl} r"
        geom_sel = "ST_AsGeoJSON(r.geometry)"
        where_sql = "r.geometry IS NOT NULL AND COALESCE(r.type, 0) <> 10"
        id_sel = "r.link_id"
    try:
        _, cnt_row = db.query(
            conn,
            f"SELECT COUNT(*) FROM {road_tbl} WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10",
        )
        total = int(cnt_row[0][0] or 0)
        fetch_n = total if cap == 0 else (total if total <= cap else cap)
        limit_sql = "" if cap == 0 or fetch_n >= total else f"LIMIT {int(fetch_n)}"
        _, rows = db.query(
            conn,
            f"""
            SELECT {id_sel}, {name_sel}, {vc_sel}, {vol_sel}, {geom_sel}, {node_sel}, {osmid_sel}
            FROM {from_sql}
            WHERE {where_sql}
            ORDER BY ({vol_sel})::float DESC NULLS LAST,
                     CASE WHEN {vc_sel} IS NULL THEN 1 ELSE 0 END ASC,
                     ({vc_sel})::float DESC NULLS LAST,
                     {id_sel}
            {limit_sql}
            """,
        )
        lats, lons = [], []
        for link_id, link_name, vc, volume, gj, init_node, term_node, link_osmid in rows:
            if not gj:
                continue
            try:
                geo = json.loads(gj)
            except json.JSONDecodeError:
                continue
            lines = geo.get("coordinates") or []
            if geo.get("type") == "MultiLineString":
                seg_list = lines
            elif geo.get("type") == "LineString":
                seg_list = [lines]
            else:
                continue
            for seg in seg_list:
                if not seg or len(seg) < 2:
                    continue
                latlng = [[p[1], p[0]] for p in seg if len(p) >= 2]
                for p in seg:
                    if len(p) >= 2:
                        lons.append(p[0])
                        lats.append(p[1])
                ini = int(init_node) if init_node is not None else 0
                ter = int(term_node) if term_node is not None else 0
                r["links"].append(
                    {
                        "link_id": int(link_id),
                        "name": str(link_name or f"路段{link_id}"),
                        "v_c": _safe_float(vc, 3) if vc is not None else None,
                        "volume": _safe_float(volume, 0),
                        "latlng": latlng,
                        "init_node": ini if ini else None,
                        "term_node": ter if ter else None,
                        "link_osmid": int(link_osmid) if link_osmid is not None else None,
                        "dir": "forward" if ini <= ter else "reverse",
                    }
                )
        if lats and lons:
            r["center"] = [sum(lats) / len(lats), sum(lons) / len(lons)]
            r["bounds"] = [
                [min(lats), min(lons)],
                [max(lats), max(lons)],
            ]
        vc_links = sum(1 for lk in r["links"] if lk.get("v_c") is not None)
        r["assignment"]["vc_link_count"] = vc_links
        r["assignment"]["has_vc_coloring"] = has_flow and vc_links > 0
        truncated = total > len(r["links"])
        r["assignment"]["total_geom"] = total
        r["assignment"]["truncated"] = truncated
        if truncated:
            r["assignment"]["truncate_note"] = (
                f"路网共 {total} 条可绘制路段，地图展示 {len(r['links'])} 条"
                f"（上限 {cap if cap else '无'}，已优先保留有流量/高 V/C 路段）。"
            )
        r["ok"] = bool(r["links"])
        logs.append(
            f"map links [{prefix}]: {len(r['links'])}/{total} segments, "
            f"has_flow={has_flow}, vc_links={vc_links}"
            + (f", truncated cap={cap}" if truncated else "")
        )
    except Exception as e:
        logs.append(f"map links failed [{prefix}]: {e}")
    return r


def count_road_geom_links(conn, prefix: str) -> int:
    """可绘制几何的机动车路段数（不含 type=10）。"""
    if not db.table_exists(conn, prefix, "road_way"):
        return 0
    if not _column_exists(conn, prefix, "road_way", "geometry"):
        return 0
    tbl = db.table_name(prefix, "road_way")
    try:
        _, rows = db.query(
            conn,
            f"SELECT COUNT(*) FROM {tbl} WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10",
        )
        return int(rows[0][0] or 0)
    except Exception:
        return 0


def resolve_map_case_id(
    conn, project_id: int, user_id: int, *candidates: int | None
) -> tuple[int, str]:
    """在候选方案中选取首个有 road_way 几何的方案用于地图展示。"""
    preferred: list[int] = []
    for c in candidates:
        if c is None:
            continue
        try:
            cid = int(c)
        except (TypeError, ValueError):
            continue
        if cid >= 0 and cid not in preferred:
            preferred.append(cid)
    if not preferred:
        return 0, ""
    for i, cid in enumerate(preferred):
        prefix = build_scheme_prefix(project_id, user_id, cid)
        if count_road_geom_links(conn, prefix) > 0:
            if i == 0:
                return cid, ""
            return cid, f"方案 case{preferred[0]} 无路网几何，地图示意采用 case{cid}"
    return preferred[0], f"方案 case{preferred[0]} 无路网几何，无法绘制空间分布图"


def fetch_scheme_assignment_status(conn, prefix: str) -> dict[str, Any]:
    """分配/流量状态，供报告地图与图表可用性判断。"""
    return _assignment_flow_status(conn, prefix)


def collect_map_bundle_for_cases(
    conn,
    project_id: int,
    user_id: int,
    case_candidates: list[int | None],
    logs: list[str] | None = None,
    type_filter: str = "all",
) -> tuple[dict[str, Any], dict[str, Any]]:
    """按候选 case 顺序采集地图；返回 (bundle, meta{map_case_id, map_note, pipeline})。"""
    logs = logs if logs is not None else []
    map_cid, map_note = resolve_map_case_id(conn, project_id, user_id, *case_candidates)
    prefix = build_scheme_prefix(project_id, user_id, map_cid)
    bundle = collect_map_bundle_scheme(
        conn, project_id, user_id, map_cid, logs, type_filter=type_filter
    )
    pipeline = (
        _assignment_flow_status_slow(conn, prefix)
        if (type_filter or "").lower() == "slow"
        else fetch_scheme_assignment_status(conn, prefix)
    )
    if map_note:
        logs.append(map_note)
    meta = {"map_case_id": map_cid, "map_note": map_note, "pipeline": pipeline}
    return bundle, meta


def collect_network_bundle(
    conn, project_id: int, user_id: int, case_id: int, logs: list[str]
) -> dict[str, Any]:
    px = resolve_prefixes(conn, project_id, user_id, case_id)
    scheme, tool, net_p, od_p = px["scheme"], px["tool"], px["network"], px["od"]
    used_fallback = net_p != scheme
    motor = collect_motor_structure(
        conn,
        net_p,
        logs,
        source_label=f"{'工具前缀(基础路网)' if used_fallback else '方案前缀'} {net_p}",
    )
    slow_p = scheme if db.table_exists(conn, scheme, "slow_road_way") else tool
    slow = collect_slow_structure(conn, slow_p, logs)
    pt = collect_pt_structure(conn, scheme if db.table_exists(conn, scheme, "pt_route") else tool, logs)
    od = collect_od_summary(conn, od_p, logs, net_prefix=net_p)
    mmap = collect_map_links(conn, net_p, logs)
    centroid = collect_centroid_layer(conn, net_p, logs)
    return {
        "prefixes": px,
        "used_network_fallback": used_fallback,
        "motor": motor,
        "slow": slow,
        "pt": pt,
        "od": od,
        "map": mmap,
        "centroid": centroid,
    }


def collect_map_bundle_scheme(
    conn,
    project_id: int,
    user_id: int,
    case_id: int,
    logs: list[str] | None = None,
    type_filter: str = "all",
) -> dict[str, Any]:
    """方案类报告：路网流量图 + OD 期望线（不含慢/公交结构统计）。"""
    logs = logs if logs is not None else []
    px = resolve_prefixes(conn, project_id, user_id, case_id)
    net_p, od_p = px["network"], px["od"]
    if (type_filter or "").lower() == "slow":
        scheme = px["scheme"]
        return {
            "prefixes": {
                **px,
                "network": scheme,
                "od": scheme,
                "map_source": f"{scheme}slow_road_way",
                "od_source": f"{scheme}slow_other_od",
            },
            "map": collect_slow_map_links(conn, scheme, logs),
            "od": collect_slow_od_summary(conn, scheme, logs),
            "centroid": collect_slow_centroid_layer(conn, scheme, logs),
        }
    return {
        "prefixes": px,
        "map": collect_map_links(conn, net_p, logs),
        "od": collect_od_summary(conn, od_p, logs, net_prefix=net_p),
        "centroid": collect_centroid_layer(conn, net_p, logs),
    }


def collect_map_bundle_tool(
    conn, project_id: int, user_id: int, logs: list[str] | None = None
) -> dict[str, Any]:
    """工具类报告：仅 project{p}_user{u}_ 前缀。"""
    logs = logs if logs is not None else []
    tool = build_tool_prefix(project_id, user_id)
    return {
        "prefixes": {"tool": tool, "network": tool, "od": tool},
        "map": collect_map_links(conn, tool, logs),
        "od": collect_od_summary(conn, tool, logs, net_prefix=tool),
        "centroid": collect_centroid_layer(conn, tool, logs),
    }
