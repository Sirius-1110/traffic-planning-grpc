"""路网 / OD 期望线 / 基础统计采集（供 AI 报告 HTML 地图与图表）。"""
from __future__ import annotations

import json
from typing import Any

from . import db

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
    """OD 期望线：优先用 road_community 交通小区质心，其次用 type=10 质心连杆。

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
        road_center = (
            f"(SELECT ST_Centroid(ST_SetSRID(ST_Extent(geometry)::geometry, 4326)) FROM {rw_tbl} "
            f"WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10)"
            if rw_tbl
            else "NULL::geometry"
        )
        def _road_side_point(alias: str) -> str:
            return f"""
              CASE
                WHEN GeometryType({alias}.g) = 'LINESTRING' THEN
                  CASE
                    WHEN ST_Distance(ST_StartPoint({alias}.g)::geography, {road_center}::geography)
                       <= ST_Distance(ST_EndPoint({alias}.g)::geography, {road_center}::geography)
                    THEN ST_StartPoint({alias}.g)
                    ELSE ST_EndPoint({alias}.g)
                  END
                ELSE ST_Centroid({alias}.g)
              END
            """
        if rw_tbl:
            connector_line = f"""
              CASE WHEN z1.g IS NOT NULL AND z2.g IS NOT NULL THEN
                ST_MakeLine({_road_side_point('z1')}, {_road_side_point('z2')})
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
                AND (CASE WHEN '{rw_tbl}' <> '' THEN ST_Intersects(rc.geometry, (
                  SELECT ST_SetSRID(ST_Extent(geometry)::geometry, 4326)
                  FROM {rw_tbl}
                  WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10
                )) ELSE true END)
              ORDER BY rc.area_id LIMIT 1
            ) rc1 ON true"""
            if comm_tbl
            else ""
        )
        lateral_rc2 = (
            f"""LEFT JOIN LATERAL (
              SELECT geometry AS g FROM {comm_tbl} rc
              WHERE rc.area_id = o.t_id AND rc.geometry IS NOT NULL
                AND (CASE WHEN '{rw_tbl}' <> '' THEN ST_Intersects(rc.geometry, (
                  SELECT ST_SetSRID(ST_Extent(geometry)::geometry, 4326)
                  FROM {rw_tbl}
                  WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10
                )) ELSE true END)
              ORDER BY rc.area_id LIMIT 1
            ) rc2 ON true"""
            if comm_tbl
            else ""
        )

        src_parts = ["WHEN o.f_id IS NOT DISTINCT FROM o.t_id THEN 'intra'"]
        if comm_tbl:
            src_parts.append("WHEN rc1.g IS NOT NULL AND rc2.g IS NOT NULL THEN 'community'")
        if rw_tbl:
            src_parts.append("WHEN z1.g IS NOT NULL AND z2.g IS NOT NULL THEN 'connector'")
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
                       COALESCE({community_line}, {connector_line}, {validated_od_geom})
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


def collect_centroid_layer(conn, prefix: str, logs: list[str], *, road_suffix: str = "road_way", community_suffix: str = "road_community") -> dict[str, Any]:
    """质心连杆 type=10 + 交通小区质心点（与分配路网同源）。"""
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "connectors": [],
        "zones": [],
        "zone_polygons": [],
    }
    if not db.table_exists(conn, prefix, road_suffix):
        return r
    rw = db.table_name(prefix, road_suffix)
    try:
        _, rows = db.query(
            conn,
            f"""
            SELECT link_id, centroid_matched_node, ST_AsGeoJSON(geometry)
            FROM {rw}
            WHERE type = 10 AND geometry IS NOT NULL
              AND ST_Length(geometry::geography) <= 10000
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
        if db.table_exists(conn, prefix, community_suffix):
            comm = db.table_name(prefix, community_suffix)
            has_name = _column_exists(conn, prefix, community_suffix, "name")
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
                  AND ST_Intersects(geometry, (
                    SELECT ST_SetSRID(ST_Extent(geometry)::geometry, 4326)
                    FROM {rw}
                    WHERE geometry IS NOT NULL AND COALESCE(type, 0) <> 10
                  ))
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


def collect_map_links(
    conn,
    prefix: str,
    logs: list[str],
    *,
    table_suffix: str = "road_way",
    max_links: int = 1200,
    with_vc: bool = True,
    map_label: str = "机动车",
) -> dict[str, Any]:
    """采集地图路段。

    table_suffix="road_way" 用于机动车；table_suffix="slow_road_way" 用于慢行。
    慢行若存在 capacity_slow_adj，则 V/C 优先按 volume / capacity_slow_adj 计算，避免混用机动车 road_way.v_c。
    """
    r: dict[str, Any] = {
        "ok": False,
        "source_prefix": prefix,
        "source_table": f"{prefix}{table_suffix}",
        "map_mode": "slow" if table_suffix == "slow_road_way" else "motor",
        "center": [30.67, 104.06],
        "bounds": None,
        "links": [],
        "assignment": {
            "has_flow": False,
            "has_vc_coloring": False,
            "flow_rows": 0,
            "vc_rows": 0,
            "note": f"{map_label}交通分配未成功或流量为空，地图仅展示路网拓扑，不按 V/C 着色。",
        },
    }
    if not db.table_exists(conn, prefix, table_suffix):
        r["reason"] = f"缺少 {table_suffix} 表。"
        return r
    if not _column_exists(conn, prefix, table_suffix, "geometry"):
        logs.append(f"{table_suffix}: no geometry column")
        r["reason"] = f"{table_suffix} 缺少 geometry 字段。"
        return r
    tbl = db.table_name(prefix, table_suffix)
    has_type = _column_exists(conn, prefix, table_suffix, "type")
    non_centroid_where = "COALESCE(type, 0) <> 10" if has_type else "true"
    has_vc = with_vc and _column_exists(conn, prefix, table_suffix, "v_c")
    has_vol = _column_exists(conn, prefix, table_suffix, "volume")
    has_name = _column_exists(conn, prefix, table_suffix, "name")
    has_cap = _column_exists(conn, prefix, table_suffix, "capacity")
    has_cap_slow_adj = _column_exists(conn, prefix, table_suffix, "capacity_slow_adj")
    if table_suffix == "slow_road_way" and has_vol and has_cap_slow_adj:
        vc_sel = "CASE WHEN capacity_slow_adj > 0 THEN volume / capacity_slow_adj ELSE NULL END"
        vc_source = "volume/capacity_slow_adj"
    elif has_vc:
        vc_sel = "v_c"
        vc_source = "v_c"
    elif has_vol and has_cap:
        vc_sel = "CASE WHEN capacity > 0 THEN volume / capacity ELSE NULL END"
        vc_source = "volume/capacity"
    else:
        vc_sel = "NULL::float"
        vc_source = "none"
    name_sel = (
        "COALESCE(NULLIF(TRIM(name), ''), '路段' || link_id::text)"
        if has_name
        else "'路段' || link_id::text"
    )
    try:
        flow_rows = 0
        vc_rows = 0
        if has_vol or vc_source != "none":
            flow_expr = "COALESCE(volume, 0)" if has_vol else "0"
            _, flow_meta = db.query(
                conn,
                f"""
                SELECT
                  COUNT(*) FILTER (WHERE {flow_expr} <> 0),
                  COUNT(*) FILTER (WHERE ({vc_sel}) IS NOT NULL)
                FROM {tbl}
                WHERE geometry IS NOT NULL AND {non_centroid_where}
                """,
            )
            flow_rows = int(flow_meta[0][0] or 0)
            vc_rows = int(flow_meta[0][1] or 0)
            r["assignment"] = {
                "has_flow": flow_rows > 0 or vc_rows > 0,
                "has_vc_coloring": vc_rows > 0,
                "flow_rows": flow_rows,
                "vc_rows": vc_rows,
                "vc_source": vc_source,
                "note": f"已读取{map_label}分配流量与 V/C，地图按路段 V/C 分级着色。" if vc_rows > 0 else f"已有{map_label}分配记录但未解析到有效 V/C，地图以拓扑线展示。",
            }
        _, cnt_row = db.query(
            conn,
            f"SELECT COUNT(*) FROM {tbl} WHERE geometry IS NOT NULL AND {non_centroid_where}",
        )
        total = int(cnt_row[0][0] or 0)
        fetch_n = min(total, int(max_links)) if total else int(max_links)
        _, rows = db.query(
            conn,
            f"""
            SELECT link_id, {name_sel}, {vc_sel} AS map_vc, ST_AsGeoJSON(geometry)
            FROM {tbl}
            WHERE geometry IS NOT NULL AND {non_centroid_where}
            ORDER BY CASE WHEN ({vc_sel}) IS NOT NULL THEN ({vc_sel}) ELSE 0 END DESC NULLS LAST
            LIMIT {int(fetch_n)}
            """,
        )
        lats, lons = [], []
        for link_id, link_name, vc, gj in rows:
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
                r["links"].append(
                    {
                        "link_id": int(link_id),
                        "name": str(link_name or f"路段{link_id}"),
                        "v_c": _safe_float(vc, 3) if vc is not None else None,
                        "latlng": latlng,
                    }
                )
        if lats and lons:
            r["center"] = [sum(lats) / len(lats), sum(lons) / len(lons)]
            r["bounds"] = [[min(lats), min(lons)], [max(lats), max(lons)]]
        r["ok"] = bool(r["links"])
        logs.append(
            f"map links [{prefix}{table_suffix}]: {len(r['links'])}/{total} segments, flow_rows={flow_rows}, vc_rows={vc_rows}, vc_source={vc_source}"
        )
    except Exception as e:
        logs.append(f"map links failed [{prefix}{table_suffix}]: {e}")
    return r

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
    mmap = collect_map_links(conn, net_p, logs, table_suffix="road_way", map_label="机动车")
    centroid = collect_centroid_layer(conn, net_p, logs, road_suffix="road_way", community_suffix="road_community")
    slow_map = collect_map_links(conn, slow_p, logs, table_suffix="slow_road_way", map_label="慢行")
    slow_centroid = collect_centroid_layer(conn, slow_p, logs, road_suffix="slow_road_way", community_suffix="slow_road_community")
    return {
        "prefixes": px,
        "used_network_fallback": used_fallback,
        "motor": motor,
        "slow": slow,
        "pt": pt,
        "od": od,
        "map": mmap,
        "centroid": centroid,
        "slow_map": slow_map,
        "slow_centroid": slow_centroid,
    }
