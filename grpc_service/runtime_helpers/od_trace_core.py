"""Shared OD trace graph + batch saturated-link tracing."""
from __future__ import annotations

import heapq
from collections import defaultdict
from typing import Any

SCHEMA = "user_project"


def tbl(prefix: str, name: str) -> str:
    return f'{SCHEMA}."{prefix}{name}"'


def fft_to_minutes(fft: float | None) -> float | None:
    """TNA road_way.fft 单位为小时。"""
    if fft is None or fft <= 0:
        return None
    return float(fft) * 60.0


def link_travel_minutes(fft: float | None, v_c: float | None, *, vc_cap: float = 2.0) -> float | None:
    """BPR 近似：t = t0 * (1 + 0.15 * (v/c)^4)；v/c 显示上限避免极端过载爆炸。"""
    t0 = fft_to_minutes(fft)
    if t0 is None:
        return None
    if v_c is None or v_c <= 0:
        return t0
    vc_eff = min(float(v_c), vc_cap)
    return t0 * (1.0 + 0.15 * (vc_eff ** 4))


def link_weight(length: float | None, fft: float | None, capacity: float | None) -> float:
    if fft is not None and fft > 0:
        return float(fft)
    if length is not None and length > 0:
        return float(length) / 1000.0
    if capacity is not None and capacity > 0:
        return 1.0 / float(capacity)
    return 1.0


def dijkstra(
    adj: dict[int, list[tuple[int, float, int]]],
    start: int,
    end: int,
) -> tuple[float, list[int]] | None:
    if start == end:
        return 0.0, []
    dist: dict[int, float] = {start: 0.0}
    prev: dict[int, tuple[int, int]] = {}
    heap: list[tuple[float, int]] = [(0.0, start)]
    while heap:
        d, u = heapq.heappop(heap)
        if d > dist.get(u, float("inf")):
            continue
        if u == end:
            break
        for v, w, lid in adj.get(u, []):
            nd = d + w
            if nd < dist.get(v, float("inf")):
                dist[v] = nd
                prev[v] = (u, lid)
                heapq.heappush(heap, (nd, v))
    if end not in dist:
        return None
    link_ids: list[int] = []
    cur = end
    while cur != start:
        if cur not in prev:
            return None
        pu, lid = prev[cur]
        link_ids.append(lid)
        cur = pu
    link_ids.reverse()
    return dist[end], link_ids


def zone_labels(cur, comm_tbl: str) -> dict[int, dict[str, Any]]:
    cur.execute(f"SELECT area_id FROM {comm_tbl} ORDER BY area_id")
    labels: dict[int, dict[str, Any]] = {}
    for i, (area_id,) in enumerate(cur.fetchall(), start=1):
        aid = int(area_id)
        labels[aid] = {
            "zone_idx": i,
            "name": f"交通小区{i}",
            "label": f"Z{i}({aid})",
        }
    return labels


def zone_network_nodes(cur, way_tbl: str, comm_tbl: str) -> dict[int, int]:
    cur.execute(f"SELECT init_node, term_node FROM {way_tbl} WHERE type = 10")
    virtual_nodes: set[int] = set()
    connectors: list[tuple[int, int]] = []
    for a, b in cur.fetchall():
        a, b = int(a), int(b)
        connectors.append((a, b))
        virtual_nodes.add(max(a, b))
    if not virtual_nodes:
        return {}
    min_virtual = min(virtual_nodes)
    cur.execute(f"SELECT area_id FROM {comm_tbl} ORDER BY area_id")
    area_ids = [int(r[0]) for r in cur.fetchall()]
    vnode_for_zone = {aid: min_virtual + i for i, aid in enumerate(area_ids)}
    zone_net: dict[int, int] = {}
    for a, b in connectors:
        hi, lo = (a, b) if a > b else (b, a)
        for zid, vnode in vnode_for_zone.items():
            if hi == vnode:
                zone_net[zid] = lo
                break
    return zone_net


def build_graph(cur, way_tbl: str) -> tuple[dict[int, list[tuple[int, float, int]]], dict[int, dict[str, Any]]]:
    cur.execute(
        f"""
        SELECT link_id, init_node, term_node, length, fft, capacity, volume, v_c,
               COALESCE(name, '') AS name, type,
               COALESCE(route_ref,'') AS route_ref, COALESCE(direction,'') AS direction
        FROM {way_tbl}
        WHERE init_node IS NOT NULL AND term_node IS NOT NULL
        """
    )
    adj: dict[int, list[tuple[int, float, int]]] = defaultdict(list)
    meta: dict[int, dict[str, Any]] = {}
    for row in cur.fetchall():
        lid, a, b, length, fft, cap, vol, vc, name, typ, route_ref, direction = row
        lid, a, b = int(lid), int(a), int(b)
        cap_f = float(cap or 0)
        vol_f = float(vol or 0)
        vc_f = float(vc) if vc is not None else (vol_f / cap_f if cap_f > 0 else 0.0)
        w = link_weight(length, fft, cap)
        adj[a].append((b, w, lid))
        meta[lid] = {
            "init_node": a,
            "term_node": b,
            "length": float(length or 0),
            "fft": float(fft) if fft is not None else None,
            "fft_min": fft_to_minutes(fft),
            "capacity": cap_f,
            "volume": vol_f,
            "v_c": vc_f,
            "name": name,
            "type": int(typ or 0),
            "route_ref": route_ref,
            "direction": direction,
            "weight_hours": w,
            "travel_min": link_travel_minutes(fft, vc_f),
        }
    return adj, meta


def saturated_link_ids(link_meta: dict[int, dict[str, Any]], min_v_c: float, *, exclude_type10: bool = True) -> list[int]:
    out: list[tuple[float, int]] = []
    for lid, m in link_meta.items():
        if exclude_type10 and int(m.get("type") or 0) == 10:
            continue
        vc = float(m.get("v_c") or 0)
        if vc >= min_v_c:
            out.append((vc, lid))
    out.sort(reverse=True)
    return [lid for _, lid in out]


def path_link_details(link_ids: list[int], link_meta: dict[int, dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for lid in link_ids:
        m = link_meta.get(lid, {})
        rows.append(
            {
                "link_id": lid,
                "name": m.get("name") or "",
                "volume": m.get("volume"),
                "capacity": m.get("capacity"),
                "v_c": m.get("v_c"),
                "fft_min": m.get("fft_min"),
                "travel_min": m.get("travel_min"),
                "length": m.get("length"),
            }
        )
    return rows


def coord_dist_km(a: list[float], b: list[float]) -> float:
    return ((float(a[0]) - float(b[0])) ** 2 + (float(a[1]) - float(b[1])) ** 2) ** 0.5 * 111.0


def orient_link_line(
    line: list[list[float]],
    init_node: int | None,
    term_node: int | None,
    enter_node: int | None,
    prev_pt: list[float] | None = None,
) -> list[list[float]]:
    """按路径进入节点 + 与上一段衔接距离决定几何方向。"""
    if len(line) < 2:
        return line
    if prev_pt is not None:
        d0 = coord_dist_km(prev_pt, line[0])
        d1 = coord_dist_km(prev_pt, line[-1])
        return line if d0 <= d1 else list(reversed(line))
    if enter_node is None or init_node is None or term_node is None:
        return line
    if enter_node == init_node:
        return line
    if enter_node == term_node:
        return list(reversed(line))
    return line


def build_path_geometry(
    link_ids: list[int],
    geom_map: dict[int, dict[str, Any]],
    start_node: int | None,
    *,
    split_gap_km: float = 0.12,
) -> dict[str, Any]:
    """
    按节点链拼接路径几何；gap 过大或 type=10 连杆单独成段，避免 Leaflet 画飞线。
    """
    segments: list[dict[str, Any]] = []
    prev_pt: list[float] | None = None
    enter_node = start_node

    for lid in link_ids:
        g = geom_map.get(lid)
        if not g:
            continue
        raw = g.get("line_coords") or []
        if len(raw) < 2:
            continue
        init_n = g.get("init_node")
        term_n = g.get("term_node")
        is_connector = int(g.get("type") or 0) == 10

        if enter_node is not None and init_n is not None and term_n is not None:
            line = orient_link_line(raw, int(init_n), int(term_n), int(enter_node), prev_pt)
            enter_node = int(term_n) if int(enter_node) == int(init_n) else int(init_n)
        elif prev_pt is not None:
            line = orient_link_line(raw, None, None, None, prev_pt)
        else:
            line = raw
            if init_n is not None and term_n is not None:
                enter_node = int(term_n)

        gap_km = coord_dist_km(prev_pt, line[0]) if prev_pt is not None else 0.0
        need_new = prev_pt is None or gap_km > split_gap_km
        if segments and not need_new:
            last = segments[-1]
            if last.get("is_connector") != is_connector and gap_km > 0.02:
                need_new = True

        if need_new:
            segments.append({"coords": list(line), "is_connector": is_connector, "link_ids": [lid]})
        else:
            last = segments[-1]
            last["coords"].extend(line[1:])
            last["link_ids"].append(lid)
            last["is_connector"] = last.get("is_connector") or is_connector

        prev_pt = line[-1]

    flat: list[list[float]] = []
    for seg in segments:
        if not flat:
            flat.extend(seg["coords"])
        else:
            if coord_dist_km(flat[-1], seg["coords"][0]) <= split_gap_km:
                flat.extend(seg["coords"][1:])
            else:
                flat.extend(seg["coords"])

    return {
        "path_segments": segments,
        "path_coords": flat,
    }


def summarize_path(link_ids: list[int], link_meta: dict[int, dict[str, Any]]) -> dict[str, Any]:
    fft_min_sum = 0.0
    travel_min_sum = 0.0
    has_fft = False
    has_travel = False
    for lid in link_ids:
        m = link_meta.get(lid, {})
        if m.get("fft_min") is not None:
            fft_min_sum += float(m["fft_min"])
            has_fft = True
        if m.get("travel_min") is not None:
            travel_min_sum += float(m["travel_min"])
            has_travel = True
    return {
        "path_free_flow_min": round(fft_min_sum, 2) if has_fft else None,
        "path_travel_time_min": round(travel_min_sum, 2) if has_travel else round(fft_min_sum, 2) if has_fft else None,
        "path_link_count": len(link_ids),
    }


def trace_saturated_links(
    conn,
    prefix: str,
    *,
    link_id: int | None = None,
    min_v_c: float = 0.8,
    top_n: int = 20,
    min_share_pct: float = 0.0,
    max_segments: int = 200,
) -> dict[str, Any]:
    way_tbl = tbl(prefix, "road_way")
    comm_tbl = tbl(prefix, "road_community")
    od_tbl = tbl(prefix, "other_od")
    cur = conn.cursor()

    adj, link_meta = build_graph(cur, way_tbl)
    zone_labels_map = zone_labels(cur, comm_tbl)
    zone_nodes = zone_network_nodes(cur, way_tbl, comm_tbl)

    if link_id is not None:
        target_ids = [int(link_id)]
        if int(link_id) not in link_meta:
            raise ValueError(f"link_id={link_id} 不存在")
    else:
        target_ids = saturated_link_ids(link_meta, min_v_c)
        if max_segments > 0:
            target_ids = target_ids[:max_segments]

    target_set = set(target_ids)
    link_contribs: dict[int, list[dict[str, Any]]] = {lid: [] for lid in target_ids}
    path_cache: dict[tuple[int, int], tuple[float, list[int]] | None] = {}

    cur.execute(
        f"SELECT f_id, t_id, COALESCE(demand, 0) AS vol FROM {od_tbl} WHERE COALESCE(demand, 0) > 0"
    )
    for f_id, t_id, vol in cur.fetchall():
        f_id, t_id = int(f_id), int(t_id)
        demand = float(vol)
        o_node = zone_nodes.get(f_id)
        d_node = zone_nodes.get(t_id)
        if o_node is None or d_node is None:
            continue
        key = (o_node, d_node)
        if key not in path_cache:
            path_cache[key] = dijkstra(adj, o_node, d_node)
        path = path_cache[key]
        if not path:
            continue
        _, lids = path
        hits = target_set.intersection(lids)
        if not hits:
            continue
        oz = zone_labels_map.get(f_id, {"label": f"Z?({f_id})", "name": ""})
        dz = zone_labels_map.get(t_id, {"label": f"Z?({t_id})", "name": ""})
        pstats = summarize_path(lids, link_meta)
        row = {
            "origin_zone": oz["label"],
            "dest_zone": dz["label"],
            "origin_id": f_id,
            "dest_id": t_id,
            "origin_net_node": o_node,
            "dest_net_node": d_node,
            "path_flow": demand,
            "initial_od": demand,
            "estimated_od": demand,
            "share_pct": 0.0,
            "n_paths": 1,
            "path_travel_time_min": pstats["path_travel_time_min"],
            "path_free_flow_min": pstats["path_free_flow_min"],
            "path_link_ids": lids,
            "path_links_detail": path_link_details(lids, link_meta),
        }
        for lid in hits:
            link_contribs[lid].append(dict(row))

    segments: list[dict[str, Any]] = []
    all_contributions: list[dict[str, Any]] = []
    for lid in target_ids:
        m = link_meta[lid]
        contribs = link_contribs.get(lid) or []
        total_flow = sum(r["path_flow"] for r in contribs)
        if total_flow > 0:
            for r in contribs:
                r["share_pct"] = round(100.0 * r["path_flow"] / total_flow, 1)
        contribs.sort(key=lambda r: (-r["path_flow"], r["origin_id"], r["dest_id"]))
        if min_share_pct > 0:
            contribs = [r for r in contribs if r["share_pct"] >= min_share_pct]
        if top_n > 0:
            contribs = contribs[:top_n]
        seg = {
            "link_id": lid,
            "label": (m.get("name") or f"link {lid}") + (f" · {m.get('direction')}" if m.get("direction") else ""),
            "volume": m.get("volume"),
            "capacity": m.get("capacity"),
            "v_c": m.get("v_c"),
            "path_flow_sum": total_flow,
            "paths_pass": bool(contribs),
            "n_paths_through": len(contribs),
            "contributions": contribs,
        }
        segments.append(seg)
        for r in contribs:
            rc = dict(r)
            rc["segment_link_id"] = lid
            all_contributions.append(rc)

    total_sat = len(saturated_link_ids(link_meta, min_v_c))
    return {
        "meta": {
            "link_id": link_id,
            "min_v_c": min_v_c,
            "flow_unit": "pcu/h",
            "method": "aon_shortest_path",
            "table_prefix": prefix,
            "segment_count": len(segments),
            "saturated_link_total": total_sat,
            "truncated": total_sat > len(segments) and link_id is None,
        },
        "segments": segments,
        "contributions": all_contributions,
        "_ctx": {
            "zone_labels": zone_labels_map,
            "zone_nodes": zone_nodes,
            "link_meta": link_meta,
            "target_ids": target_ids,
        },
    }
