"""解析 MCP / HTTP 入参 JSON 字符串。"""
from __future__ import annotations

import json
from typing import Any


def parse_param2(raw: Any) -> dict[str, Any]:
    """gRPC param2 同义：对象或 JSON 字符串。"""
    if raw is None:
        return {}
    if isinstance(raw, dict):
        return raw
    text = str(raw).strip()
    if not text:
        return {}
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def parse_indicator_codes_json(raw: str | None) -> list[str] | None:
    """
    支持单选与多选：
    - "road_macro_tpi"
    - ["road_macro_tpi","road_micro_v_c"]
    - "road_macro_tpi,road_micro_v_c"
    """
    if raw is None or not str(raw).strip():
        return None
    text = str(raw).strip()
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        parts = [x.strip() for x in text.replace("，", ",").split(",") if x.strip()]
        return parts or None
    if isinstance(parsed, str):
        s = parsed.strip()
        return [s] if s else None
    if isinstance(parsed, list):
        codes = [str(x).strip() for x in parsed if str(x).strip()]
        return codes or None
    return None


def merge_spatial_into_prompt_blocks(pb: dict[str, Any], sf: dict[str, Any]) -> None:
    """将 spatial_filter_json 合并进 prompt_blocks。"""
    if not isinstance(sf, dict):
        return
    if isinstance(sf.get("spatial_scope"), dict):
        pb["spatial_scope"] = sf["spatial_scope"]
    if sf.get("layers") is not None:
        pb["layers"] = sf["layers"]
    for k in (
        "bbox",
        "link_ids",
        "line_ids",
        "node_ids",
        "object_ids",
        "selection_global",
        "spatial_mode",
    ):
        if k in sf:
            pb[k] = sf[k]
