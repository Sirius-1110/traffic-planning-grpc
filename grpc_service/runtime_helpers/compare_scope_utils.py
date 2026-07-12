"""方案对比：bbox scope → 各 case 独立解析机动车 link_id。"""
from __future__ import annotations

import json
from typing import Any


def kv_unescape(text: str) -> str:
    """与 local_bridge.cpp unescape_value 一致。"""
    if not text:
        return text
    out: list[str] = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "n":
                out.append("\n")
            elif nxt == "r":
                out.append("\r")
            else:
                out.append(nxt)
            i += 2
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def normalize_bbox_param(bbox: Any) -> dict[str, Any] | None:
    """
    支持：
      - [south, west, north, east]（WGS84，与 OSM 地图一致）
      - GeoJSON Polygon / Feature
    返回 envelope 或 geojson 模式描述。
    """
    if bbox is None:
        return None
    if isinstance(bbox, str):
        text = bbox.strip()
        if not text:
            return None
        try:
            bbox = json.loads(text)
        except json.JSONDecodeError:
            parts = [p.strip() for p in text.replace(";", ",").split(",") if p.strip()]
            if len(parts) == 4:
                bbox = [float(x) for x in parts]
            else:
                return None
    if isinstance(bbox, list) and len(bbox) == 4:
        try:
            south, west, north, east = [float(x) for x in bbox]
            return {
                "mode": "envelope",
                "west": west,
                "south": south,
                "east": east,
                "north": north,
            }
        except (TypeError, ValueError):
            return None
    if isinstance(bbox, dict):
        if bbox.get("type") == "Feature":
            return normalize_bbox_param(bbox.get("geometry"))
        if bbox.get("type") == "Polygon" and bbox.get("coordinates"):
            return {"mode": "geojson", "geojson": bbox}
    return None


def extract_bbox_from_param2(p2: dict) -> Any:
    """param2 顶层 bbox 或 compare_scope.bbox。"""
    if not p2:
        return None
    if p2.get("bbox") is not None:
        return p2.get("bbox")
    scope = p2.get("compare_scope")
    if isinstance(scope, dict) and scope.get("bbox") is not None:
        return scope.get("bbox")
    return None


def _table_geometry_srid(conn, road_way_table: str) -> int:
    with conn.cursor() as cur:
        cur.execute(
            f"""
            SELECT ST_SRID(geometry)
            FROM {road_way_table}
            WHERE geometry IS NOT NULL
            LIMIT 1
            """
        )
        row = cur.fetchone()
    return int(row[0] or 0) if row else 0


def resolve_motor_link_ids_by_bbox(
    conn,
    road_way_table: str,
    bbox_norm: dict[str, Any],
    *,
    exclude_type10: bool = True,
    logs: list[str] | None = None,
) -> list[int]:
    logs = logs or []
    t10 = " AND COALESCE(w.type, 0) <> 10" if exclude_type10 else ""
    srid = _table_geometry_srid(conn, road_way_table)
    if srid <= 0:
        srid = 4326
    with conn.cursor() as cur:
        if bbox_norm["mode"] == "envelope":
            w, s, e, n = (
                bbox_norm["west"],
                bbox_norm["south"],
                bbox_norm["east"],
                bbox_norm["north"],
            )
            if srid == 4326:
                geom_expr = "ST_MakeEnvelope(%s, %s, %s, %s, 4326)"
                geom_args: tuple[Any, ...] = (w, s, e, n)
            else:
                geom_expr = (
                    "ST_Transform(ST_MakeEnvelope(%s, %s, %s, %s, 4326), %s)"
                )
                geom_args = (w, s, e, n, srid)
            cur.execute(
                f"""
                SELECT w.link_id
                FROM {road_way_table} w
                WHERE w.geometry IS NOT NULL
                  {t10}
                  AND ST_Intersects(w.geometry, {geom_expr})
                ORDER BY w.link_id
                """,
                geom_args,
            )
        else:
            geojson = json.dumps(bbox_norm["geojson"], ensure_ascii=False)
            if srid == 4326:
                geom_expr = "ST_SetSRID(ST_GeomFromGeoJSON(%s), 4326)"
                geom_args = (geojson,)
            else:
                geom_expr = (
                    "ST_Transform(ST_SetSRID(ST_GeomFromGeoJSON(%s), 4326), %s)"
                )
                geom_args = (geojson, srid)
            cur.execute(
                f"""
                SELECT w.link_id
                FROM {road_way_table} w
                WHERE w.geometry IS NOT NULL
                  {t10}
                  AND ST_Intersects(w.geometry, {geom_expr})
                ORDER BY w.link_id
                """,
                geom_args,
            )
        rows = cur.fetchall()
    link_ids = [int(r[0]) for r in rows]
    logs.append(
        f"bbox scope: {road_way_table} srid={srid} matched link_ids={len(link_ids)}"
    )
    return link_ids


def resolve_link_ids_by_case(
    conn,
    prefix: str,
    road_way_table_fn,
    bbox_norm: dict[str, Any],
    *,
    exclude_type10: bool = True,
    logs: list[str] | None = None,
) -> list[int]:
    tbl = road_way_table_fn(prefix, "road_way")
    return resolve_motor_link_ids_by_bbox(
        conn, tbl, bbox_norm, exclude_type10=exclude_type10, logs=logs
    )
