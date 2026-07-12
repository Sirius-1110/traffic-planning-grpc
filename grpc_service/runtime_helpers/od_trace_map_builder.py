"""Build TRACE_MAP_DATA + HTML for urban motor od_trace (Leaflet bundle)."""
from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Any

from od_trace_core import build_path_geometry, fft_to_minutes, link_travel_minutes

_TYPE_LABEL = {1: "快速路", 2: "主干路", 3: "次干路", 4: "支路", 5: "其他"}


def geojson_line_to_latlng(geojson_text: str | None) -> list[list[float]]:
    if not geojson_text:
        return []
    try:
        geo = json.loads(geojson_text) if isinstance(geojson_text, str) else geojson_text
    except (json.JSONDecodeError, TypeError):
        return []
    gtype = geo.get("type")
    coords: list = []
    if gtype == "LineString":
        coords = geo.get("coordinates") or []
    elif gtype == "MultiLineString":
        for part in geo.get("coordinates") or []:
            coords.extend(part or [])
    return [[float(p[1]), float(p[0])] for p in coords if isinstance(p, (list, tuple)) and len(p) >= 2]


def geojson_polygon_to_latlng_rings(geojson_text: str | None) -> list[list[list[float]]]:
    if not geojson_text:
        return []
    try:
        geo = json.loads(geojson_text) if isinstance(geojson_text, str) else geojson_text
    except (json.JSONDecodeError, TypeError):
        return []
    gtype = geo.get("type")
    rings: list[list[list[float]]] = []
    if gtype == "Polygon":
        for ring in geo.get("coordinates") or []:
            rings.append([[float(p[1]), float(p[0])] for p in ring if len(p) >= 2])
    elif gtype == "MultiPolygon":
        for poly in geo.get("coordinates") or []:
            if poly:
                rings.append([[float(p[1]), float(p[0])] for p in poly[0] if len(p) >= 2])
    return rings


def _tbl(prefix: str, name: str) -> str:
    return f'user_project."{prefix}{name}"'


def _vc_color(v_c: float | None) -> str:
    v = float(v_c or 0)
    if v >= 1.2:
        return "#b71c1c"
    if v >= 1.0:
        return "#e65100"
    if v >= 0.8:
        return "#f9a825"
    if v >= 0.6:
        return "#7cb342"
    return "#78909c"


def _fetch_link_geometries(cur, way_tbl: str, link_ids: list[int]) -> dict[int, dict[str, Any]]:
    if not link_ids:
        return {}
    cur.execute(
        f"""
        SELECT link_id, init_node, term_node, COALESCE(name,'') AS name, type, volume, capacity, v_c, fft, length,
               COALESCE(route_ref,'') AS route_ref, COALESCE(direction,'') AS direction,
               ST_AsGeoJSON(geometry) AS geo
        FROM {way_tbl}
        WHERE link_id = ANY(%s) AND geometry IS NOT NULL
        """,
        (link_ids,),
    )
    out: dict[int, dict[str, Any]] = {}
    for lid, init_node, term_node, name, typ, vol, cap, vc, fft, length, route_ref, direction, geo in cur.fetchall():
        line = geojson_line_to_latlng(geo)
        if len(line) < 2:
            continue
        vc_f = float(vc) if vc is not None else (float(vol or 0) / float(cap) if cap else 0)
        out[int(lid)] = {
            "link_id": int(lid),
            "init_node": int(init_node),
            "term_node": int(term_node),
            "name": name or "",
            "type": int(typ or 0),
            "type_label": _TYPE_LABEL.get(int(typ or 0), "其他"),
            "volume": float(vol or 0),
            "capacity": float(cap or 0),
            "v_c": vc_f,
            "fft_min": fft_to_minutes(float(fft) if fft is not None else None),
            "travel_min": link_travel_minutes(float(fft) if fft is not None else None, vc_f),
            "length": float(length or 0),
            "route_ref": route_ref,
            "direction": direction,
            "line_coords": line,
            "color": _vc_color(vc_f),
        }
    return out


def _path_coords_from_links(
    link_ids: list[int],
    geom_map: dict[int, dict[str, Any]],
    start_node: int | None = None,
) -> dict[str, Any]:
    return build_path_geometry(link_ids, geom_map, start_node)


def collect_zones(cur, comm_tbl: str, zone_labels: dict[int, dict[str, Any]]) -> list[dict[str, Any]]:
    cur.execute(
        f"""
        SELECT area_id, ST_Y(ST_Centroid(geometry)) AS lat, ST_X(ST_Centroid(geometry)) AS lng,
               ST_AsGeoJSON(geometry) AS geo
        FROM {comm_tbl}
        WHERE geometry IS NOT NULL
        ORDER BY area_id
        """
    )
    zones: list[dict[str, Any]] = []
    for area_id, lat, lng, geo in cur.fetchall():
        aid = int(area_id)
        zl = zone_labels.get(aid, {})
        if lat is None or lng is None:
            continue
        zones.append(
            {
                "zone_id": zl.get("zone_idx", aid),
                "pg_id": aid,
                "label": zl.get("label", f"Z?({aid})"),
                "name": zl.get("name", f"交通小区{aid}"),
                "latlng": [float(lat), float(lng)],
                "polygons": geojson_polygon_to_latlng_rings(geo),
            }
        )
    return zones


def collect_connectors(
    cur,
    way_tbl: str,
    comm_tbl: str,
    zone_labels: dict[int, dict[str, Any]],
    *,
    one_per_zone: bool = True,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    cur.execute(
        f"""
        SELECT link_id, init_node, term_node, ST_AsGeoJSON(geometry) AS geo
        FROM {way_tbl}
        WHERE type = 10 AND geometry IS NOT NULL
        ORDER BY link_id
        """
    )
    rows = cur.fetchall()
    if not rows:
        return [], {"total": 0, "displayed": 0, "zones": 0}
    virtual_nodes: set[int] = set()
    for _, a, b, _ in rows:
        virtual_nodes.add(max(int(a), int(b)))
    min_virtual = min(virtual_nodes)
    cur.execute(f"SELECT area_id FROM {comm_tbl} ORDER BY area_id")
    area_ids = [int(r[0]) for r in cur.fetchall()]
    vnode_for_zone = {aid: min_virtual + i for i, aid in enumerate(area_ids)}

    by_zone: dict[int, list[dict[str, Any]]] = {}
    for lid, a, b, geo in rows:
        a, b = int(a), int(b)
        hi, lo = (a, b) if a > b else (b, a)
        zone_id = next((z for z, v in vnode_for_zone.items() if v == hi), None)
        zl = zone_labels.get(zone_id or -1, {})
        coords = geojson_line_to_latlng(geo)
        if len(coords) < 2:
            continue
        item = {
            "link_id": int(lid),
            "zone_id": zl.get("zone_idx", zone_id),
            "zone_pg": zone_id,
            "zone_label": zl.get("label", ""),
            "virtual_node": hi,
            "net_node": lo,
            "coords": coords,
        }
        key = int(zone_id or hi)
        by_zone.setdefault(key, []).append(item)

    out: list[dict[str, Any]] = []
    for _zid, items in sorted(by_zone.items(), key=lambda x: (x[1][0].get("zone_id") or 0)):
        if one_per_zone:
            # 同一小区多次 build_centroid_connectors 会累积多条；展示最新 link_id
            out.append(max(items, key=lambda x: x["link_id"]))
        else:
            out.extend(items)

    stats = {
        "total": len(rows),
        "displayed": len(out),
        "zones": len(by_zone),
    }
    return out, stats


def collect_full_network(cur, way_tbl: str, *, max_links: int = 12000) -> list[dict[str, Any]]:
    cur.execute(
        f"""
        SELECT link_id, COALESCE(name,'') AS name, type, volume, capacity, v_c, fft,
               ST_AsGeoJSON(geometry) AS geo
        FROM {way_tbl}
        WHERE type <> 10 AND geometry IS NOT NULL
        ORDER BY COALESCE(v_c, CASE WHEN capacity>0 THEN volume/capacity ELSE 0 END) DESC
        LIMIT %s
        """,
        (max_links,),
    )
    links: list[dict[str, Any]] = []
    for lid, name, typ, vol, cap, vc, fft, geo in cur.fetchall():
        line = geojson_line_to_latlng(geo)
        if len(line) < 2:
            continue
        vc_f = float(vc) if vc is not None else (float(vol or 0) / float(cap) if cap else 0)
        links.append(
            {
                "link_id": int(lid),
                "name": name or "",
                "type_label": _TYPE_LABEL.get(int(typ or 0), "其他"),
                "volume": float(vol or 0),
                "capacity": float(cap or 0),
                "v_c": vc_f,
                "fft_min": fft_to_minutes(float(fft) if fft is not None else None),
                "travel_min": link_travel_minutes(float(fft) if fft is not None else None, vc_f),
                "line_coords": line,
                "color": _vc_color(vc_f),
                "saturated": vc_f >= 0.8,
            }
        )
    return links


def _segment_to_map(seg: dict[str, Any], geom_map: dict[int, dict[str, Any]]) -> dict[str, Any]:
    lid = int(seg["link_id"])
    g = geom_map.get(lid, {})
    line = g.get("line_coords") or []
    map_ods: list[dict[str, Any]] = []
    for row in seg.get("contributions") or []:
        lids = [int(x) for x in (row.get("path_link_ids") or [])]
        pg = _path_coords_from_links(lids, geom_map, row.get("origin_net_node"))
        path_coords = pg.get("path_coords") or []
        path_segments = pg.get("path_segments") or []
        path_links = [geom_map[x] for x in lids if x in geom_map]
        map_ods.append(
            {
                "origin": row.get("origin_zone"),
                "dest": row.get("dest_zone"),
                "origin_id": row.get("origin_id"),
                "dest_id": row.get("dest_id"),
                "path_flow": row.get("path_flow"),
                "initial_od": row.get("initial_od"),
                "estimated_od": row.get("estimated_od"),
                "share_pct": row.get("share_pct"),
                "n_paths": row.get("n_paths", 1),
                "path_travel_time_min": row.get("path_travel_time_min"),
                "path_free_flow_min": row.get("path_free_flow_min"),
                "paths": [
                    {
                        "path_id": 1,
                        "path_flow": row.get("path_flow"),
                        "path_travel_time_min": row.get("path_travel_time_min"),
                        "path_free_flow_min": row.get("path_free_flow_min"),
                        "path_coords": path_coords,
                        "path_segments": path_segments,
                        "path_links": path_links,
                        "path_links_detail": row.get("path_links_detail") or path_links,
                    }
                ],
            }
        )
    return {
        "link_id": lid,
        "label": seg.get("label") or g.get("name") or f"link {lid}",
        "line_coords": line,
        "assigned": seg.get("volume"),
        "capacity": seg.get("capacity"),
        "v_c": seg.get("v_c"),
        "path_flow_sum": seg.get("path_flow_sum"),
        "paths_pass": seg.get("paths_pass", bool(map_ods)),
        "n_paths_through": len(map_ods),
        "all_path_ods": map_ods,
        "top_ods": map_ods,
        "related_zone_labels": sorted({o["origin"] for o in map_ods} | {o["dest"] for o in map_ods}),
    }


def build_map_bundle(
    conn,
    prefix: str,
    trace_result: dict[str, Any],
    *,
    pid: int,
    uid: int,
    cid: int,
    zone_labels: dict[int, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    meta = trace_result.get("meta") or {}
    way_tbl = _tbl(prefix, "road_way")
    comm_tbl = _tbl(prefix, "road_community")
    cur = conn.cursor()

    all_link_ids: set[int] = set()
    for seg in trace_result.get("segments") or []:
        all_link_ids.add(int(seg["link_id"]))
        for row in seg.get("contributions") or []:
            all_link_ids.update(int(x) for x in (row.get("path_link_ids") or []))

    geom_map = _fetch_link_geometries(cur, way_tbl, sorted(all_link_ids))
    zones = collect_zones(cur, comm_tbl, zone_labels or {})
    connectors, conn_stats = collect_connectors(cur, way_tbl, comm_tbl, zone_labels or {})
    network_links = collect_full_network(cur, way_tbl)

    segments = [_segment_to_map(seg, geom_map) for seg in (trace_result.get("segments") or [])]
    saturated_count = sum(1 for lk in network_links if lk.get("saturated"))

    return {
        "meta": {
            "generated_at": datetime.now().isoformat(timespec="seconds"),
            "run": {
                "folder": f"project{pid}_user{uid}_case{cid}",
                "label": f"p{pid} u{uid} case{cid} · 饱和度≥{meta.get('min_v_c', 0.8)} 溯源",
            },
            "summary": {
                "flow_unit": meta.get("flow_unit", "pcu/h"),
                "flow_unit_note": "分配流量=road_way.volume；OD=other_od.demand；行程=BPR(fft×60min, v/c capped 2.0)",
                "method": meta.get("method", "aon_shortest_path"),
                "min_v_c": meta.get("min_v_c", 0.8),
                "zone_count": len(zones),
                "centroid_connector_count": conn_stats.get("displayed", len(connectors)),
                "centroid_connector_total_in_db": conn_stats.get("total", len(connectors)),
                "centroid_connector_zones": conn_stats.get("zones", len(zones)),
                "network_link_count": len(network_links),
                "saturated_link_count": saturated_count,
                "segment_count": len(segments),
                "saturated_link_total": meta.get("saturated_link_total"),
                "truncated": meta.get("truncated"),
            },
        },
        "zones": zones,
        "centroid_connectors": connectors,
        "network_links": network_links,
        "segments": segments,
    }


def write_map_bundle(
    bundle: dict[str, Any],
    out_dir: str | Path,
    *,
    stem: str = "od_trace_map",
) -> tuple[str, str]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    html_path = out / f"{stem}.html"
    data_path = out / f"{stem}_data.js"

    template = (Path(__file__).resolve().parent / "od_trace_map.html").read_text(encoding="utf-8")
    data_js = "window.TRACE_MAP_DATA = " + json.dumps(bundle, ensure_ascii=False, separators=(",", ":")) + ";\n"
    data_path.write_text(data_js, encoding="utf-8")

    html = template.replace("__DATA_JS__", data_path.name)
    html_path.write_text(html, encoding="utf-8")
    return str(html_path), str(data_path)
