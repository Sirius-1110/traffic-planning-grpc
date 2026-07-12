"""AI 智能诊断 — 框选范围：全局 / bbox / 多图层多 ID。"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

# 图层名 → 内部 ID 桶（与 GIS 图层 / PG 表后缀对齐）
LAYER_ALIASES: dict[str, str] = {
    "road_way": "link_ids",
    "road": "link_ids",
    "motor": "link_ids",
    "slow_road_way": "slow_link_ids",
    "slow": "slow_link_ids",
    "pt_route": "line_ids",
    "other_bus_route": "line_ids",
    "bus_route": "line_ids",
    "bus": "line_ids",
    "pt": "line_ids",
    "road_point": "node_ids",
    "node": "node_ids",
    "intersection": "node_ids",
}


@dataclass
class SpatialScope:
    """默认 mode=global 表示不框选。"""

    mode: str = "global"
    bbox: list[float] | None = None
    link_ids: list[int] = field(default_factory=list)
    slow_link_ids: list[int] = field(default_factory=list)
    line_ids: list[int] = field(default_factory=list)
    node_ids: list[int] = field(default_factory=list)
    object_ids: list[str] = field(default_factory=list)
    layers: dict[str, list[int]] = field(default_factory=dict)
    label: str = "全局范围（未框选）"

    @property
    def is_global(self) -> bool:
        return self.mode == "global"

    def to_dict(self) -> dict[str, Any]:
        return {
            "mode": self.mode,
            "bbox": self.bbox,
            "link_ids": self.link_ids,
            "slow_link_ids": self.slow_link_ids,
            "line_ids": self.line_ids,
            "node_ids": self.node_ids,
            "object_ids": self.object_ids,
            "layers": self.layers,
            "label": self.label,
        }


def _as_int_list(raw: Any) -> list[int]:
    if raw is None:
        return []
    if isinstance(raw, (int, float)):
        return [int(raw)]
    if isinstance(raw, str):
        raw = [x.strip() for x in raw.replace("，", ",").split(",") if x.strip()]
    out: list[int] = []
    for x in raw:
        try:
            out.append(int(x))
        except (TypeError, ValueError):
            continue
    return out


def _as_bbox(raw: Any) -> list[float] | None:
    if not raw:
        return None
    if isinstance(raw, str):
        try:
            import json

            raw = json.loads(raw)
        except json.JSONDecodeError:
            parts = [p.strip() for p in raw.replace("，", ",").split(",") if p.strip()]
            if len(parts) >= 4:
                return [float(x) for x in parts[:4]]
            return None
    if isinstance(raw, (list, tuple)) and len(raw) >= 4:
        return [float(raw[0]), float(raw[1]), float(raw[2]), float(raw[3])]
    return None


def _parse_layers(raw: Any) -> dict[str, list[int]]:
    """支持 layers 对象或 layers 数组。"""
    out: dict[str, list[int]] = {}
    if not raw:
        return out
    if isinstance(raw, dict):
        for layer, ids in raw.items():
            lst = _as_int_list(ids)
            if lst:
                out[str(layer)] = lst
        return out
    if isinstance(raw, list):
        for item in raw:
            if not isinstance(item, dict):
                continue
            layer = str(item.get("layer") or item.get("name") or item.get("table") or "")
            ids = _as_int_list(item.get("ids") or item.get("id_list") or item.get("object_ids"))
            if layer and ids:
                out[layer] = ids
    return out


def _apply_layers_to_scope(scope: SpatialScope, layers: dict[str, list[int]]) -> None:
    scope.layers = dict(layers)
    for layer, ids in layers.items():
        key = LAYER_ALIASES.get(layer, LAYER_ALIASES.get(layer.lower(), ""))
        if key == "link_ids":
            scope.link_ids = list(dict.fromkeys(scope.link_ids + ids))
        elif key == "slow_link_ids":
            scope.slow_link_ids = list(dict.fromkeys(scope.slow_link_ids + ids))
        elif key == "line_ids":
            scope.line_ids = list(dict.fromkeys(scope.line_ids + ids))
        elif key == "node_ids":
            scope.node_ids = list(dict.fromkeys(scope.node_ids + ids))


def parse_spatial_scope(prompt_blocks: dict[str, Any] | None) -> SpatialScope:
    pb = prompt_blocks or {}
    nested = pb.get("spatial_scope") or pb.get("selection_scope") or {}
    if isinstance(nested, str):
        try:
            import json

            nested = json.loads(nested)
        except json.JSONDecodeError:
            nested = {}

    bbox = _as_bbox(pb.get("bbox") or nested.get("bbox"))
    link_ids = _as_int_list(pb.get("link_ids") or nested.get("link_ids"))
    line_ids = _as_int_list(pb.get("line_ids") or nested.get("line_ids"))
    node_ids = _as_int_list(pb.get("node_ids") or nested.get("node_ids"))
    slow_link_ids = _as_int_list(pb.get("slow_link_ids") or nested.get("slow_link_ids"))
    layers = _parse_layers(pb.get("layers") or nested.get("layers"))

    object_ids = pb.get("object_ids") or nested.get("object_ids") or []
    if isinstance(object_ids, str):
        object_ids = [x.strip() for x in object_ids.replace("，", ",").split(",") if x.strip()]

    scope = SpatialScope(
        bbox=bbox,
        link_ids=link_ids,
        slow_link_ids=slow_link_ids,
        line_ids=line_ids,
        node_ids=node_ids,
        object_ids=list(object_ids) if isinstance(object_ids, list) else [],
    )
    if layers:
        _apply_layers_to_scope(scope, layers)

    if object_ids and not scope.link_ids and not scope.line_ids:
        for oid in object_ids:
            try:
                scope.link_ids.append(int(oid))
            except (TypeError, ValueError):
                pass

    if pb.get("selection_global") is True or str(pb.get("spatial_mode", "")).lower() in (
        "global",
        "全网",
        "全局",
    ):
        return SpatialScope(mode="global", label="全局范围（未框选）")

    has_filter = (
        bbox
        or scope.link_ids
        or scope.slow_link_ids
        or scope.line_ids
        or scope.node_ids
        or scope.layers
    )
    if has_filter:
        parts = []
        if bbox:
            parts.append(f"bbox={bbox}")
        if scope.layers:
            layer_parts = [
                f"{k}={v[:6]}{'…' if len(v) > 6 else ''}" for k, v in scope.layers.items()
            ]
            parts.append("layers={" + "; ".join(layer_parts) + "}")
        if scope.link_ids:
            parts.append(f"link_ids={scope.link_ids[:8]}{'…' if len(scope.link_ids) > 8 else ''}")
        if scope.slow_link_ids:
            parts.append(f"slow_link_ids={scope.slow_link_ids}")
        if scope.line_ids:
            parts.append(f"line_ids={scope.line_ids}")
        if scope.node_ids:
            parts.append(f"node_ids={scope.node_ids}")
        scope.mode = "filtered"
        scope.label = "局部框选：" + "；".join(parts)
        return scope

    return SpatialScope(mode="global", label="全局范围（未框选）")


def motor_link_filter_sql(spatial: SpatialScope, alias: str = "w") -> tuple[str, list[Any]]:
    if spatial.is_global:
        return "", []
    clauses: list[str] = []
    params: list[Any] = []
    ids = list(dict.fromkeys(spatial.link_ids + spatial.slow_link_ids))
    if ids:
        clauses.append(f"{alias}.link_id = ANY(%s)")
        params.append(ids)
    if spatial.bbox:
        clauses.append(
            f"ST_Intersects({alias}.geometry::geometry, "
            f"ST_MakeEnvelope(%s, %s, %s, %s, 4326))"
        )
        params.extend(spatial.bbox)
    if not clauses:
        return "", []
    return " AND ".join(clauses), params


def diagnosis_row_matches_spatial(row: dict[str, Any], spatial: SpatialScope) -> bool:
    if spatial.is_global:
        return True
    ctx = str(row.get("context") or row.get("context_key") or "")
    if ctx.upper() == "GLOBAL":
        return True
    all_ids: list[int] = (
        spatial.link_ids + spatial.slow_link_ids + spatial.line_ids + spatial.node_ids
    )
    ids = set(str(x) for x in all_ids)
    for oid in spatial.object_ids:
        ids.add(str(oid))
    if not ids:
        return True
    return any(i in ctx for i in ids)
