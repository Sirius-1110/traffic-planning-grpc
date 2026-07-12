"""解析 param2 顶层 GIS 图层修改记录（gis_edit_log / ids）。

gis_edit_log 为数组，每项对应一次修改（可多条）：
  - 文案：{"demo": "路段图层编辑要素ID：707 修改： (fft: 从 'a' 改成 'b')"}
  - 结构化：{"target_id":"707","changes":[{"field":"fft","old_value":"a","new_value":"b"}]}
"""
from __future__ import annotations

import re
from typing import Any

from .param_parse import parse_param2

_ID_RE = re.compile(r"(?:要素ID|对象ID|link_id|路段ID|ID)[：:\s]*(\d+)", re.I)
_FIELD_CHG_RE = re.compile(
    r"([a-zA-Z_][\w]*)\s*[:：]\s*从\s*['\"]?([^'\"）)]+)['\"]?\s*改成\s*['\"]?([^'\"）)]+)['\"]?",
    re.I,
)

_TEXT_KEYS = ("demo", "text", "message", "content", "desc", "note")


def _parse_ids_raw(raw: Any) -> list[str]:
    if raw is None:
        return []
    if isinstance(raw, list):
        return [str(x).strip() for x in raw if str(x).strip()]
    text = str(raw).strip()
    if not text:
        return []
    parts = re.split(r"[,，\s]+", text)
    return [p.strip() for p in parts if p.strip()]


def _extract_text(item: dict[str, Any]) -> str:
    for key in _TEXT_KEYS:
        val = item.get(key)
        if val and str(val).strip():
            return str(val).strip()
    return ""


def _format_changes(changes: list[dict[str, Any]]) -> str:
    parts: list[str] = []
    for ch in changes:
        if not isinstance(ch, dict):
            continue
        field = str(ch.get("field") or ch.get("name") or ch.get("attr") or "").strip()
        old_v = ch.get("old_value", ch.get("from", ch.get("old", "")))
        new_v = ch.get("new_value", ch.get("to", ch.get("new", "")))
        if field:
            parts.append(f"{field}: {old_v} → {new_v}")
    return "; ".join(parts)


def edit_log_text_to_gis_op(text: str, index: int = 1) -> dict[str, Any]:
    t = (text or "").strip()
    m = _ID_RE.search(t)
    target_id = m.group(1) if m else ""
    action = "修改属性"
    if "新增" in t or "添加" in t:
        action = "新增"
    elif "删除" in t:
        action = "删除"
    elif "编辑" in t or "修改" in t:
        action = "编辑属性"

    changes: list[dict[str, str]] = []
    for fm in _FIELD_CHG_RE.finditer(t):
        changes.append({
            "field": fm.group(1),
            "old_value": fm.group(2).strip(),
            "new_value": fm.group(3).strip(),
        })
    params = _format_changes(changes) if changes else t[:200]

    return {
        "op_id": f"GIS-{index:02d}",
        "target_id": target_id,
        "action": action,
        "params": params,
        "note": t[:500],
        "changes": changes or None,
    }


def structured_item_to_gis_op(item: dict[str, Any], index: int) -> dict[str, Any]:
    target_id = str(
        item.get("target_id") or item.get("link_id") or item.get("element_id") or ""
    ).strip()
    action = str(item.get("action") or "编辑属性").strip()
    layer = str(item.get("layer") or item.get("layer_name") or "").strip()

    changes: list[dict[str, Any]] = []
    raw_changes = item.get("changes")
    if isinstance(raw_changes, list):
        changes = [dict(c) for c in raw_changes if isinstance(c, dict)]
    elif item.get("field"):
        changes = [{
            "field": item.get("field"),
            "old_value": item.get("old_value", item.get("from", item.get("old", ""))),
            "new_value": item.get("new_value", item.get("to", item.get("new", ""))),
        }]

    params = _format_changes(changes)
    demo = _extract_text(item)
    if not target_id and demo:
        m = _ID_RE.search(demo)
        target_id = m.group(1) if m else ""
    if not params and demo:
        op = edit_log_text_to_gis_op(demo, index)
        if target_id:
            op["target_id"] = target_id
        if layer:
            op["layer"] = layer
        if changes:
            op["changes"] = changes
            op["params"] = _format_changes(changes) or op.get("params", "")
        return op

    note = demo or (f"{layer} 要素 {target_id} {action}" if layer else f"要素 {target_id} {action}")
    row: dict[str, Any] = {
        "op_id": f"GIS-{index:02d}",
        "target_id": target_id,
        "action": action,
        "params": params,
        "note": note[:500],
        "changes": changes or None,
    }
    if layer:
        row["layer"] = layer
    return row


def gis_edit_item_to_op(item: Any, index: int) -> dict[str, Any] | None:
    if isinstance(item, str) and item.strip():
        return edit_log_text_to_gis_op(item.strip(), index)
    if not isinstance(item, dict):
        return None

    text = _extract_text(item)
    has_structured = bool(
        item.get("changes")
        or item.get("field")
        or (
            (item.get("target_id") or item.get("link_id") or item.get("element_id"))
            and not text
        )
    )
    if has_structured:
        return structured_item_to_gis_op(item, index)
    if text:
        op = edit_log_text_to_gis_op(text, index)
        tid = item.get("target_id") or item.get("link_id")
        if tid:
            op["target_id"] = str(tid)
        if item.get("layer") or item.get("layer_name"):
            op["layer"] = str(item.get("layer") or item.get("layer_name"))
        return op
    if item.get("target_id") or item.get("action") or item.get("note"):
        row = dict(item)
        row.setdefault("op_id", f"GIS-{index:02d}")
        return row
    return None


def _build_gis_context(
    prompt_blocks: dict[str, Any] | None,
    param2: dict[str, Any] | None,
) -> dict[str, Any]:
    pb = dict(prompt_blocks or {})
    p2 = dict(param2 or {})
    ctx = dict(pb)
    for key in ("gis_edit_log", "ids", "gis_operations"):
        if key in p2 and p2[key] is not None:
            ctx[key] = p2[key]
    if not ctx.get("gis_edit_log"):
        legacy = p2.get("demos") or pb.get("demos")
        if legacy:
            ctx["gis_edit_log"] = legacy
    if "ids" not in ctx and p2.get("ids") is None and pb.get("ids") is not None:
        ctx["ids"] = pb["ids"]
    return ctx


def parse_gis_operations_from_context(
    prompt_blocks: dict[str, Any] | None = None,
    param2: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    ctx = _build_gis_context(prompt_blocks, param2)
    out: list[dict[str, Any]] = []

    structured = ctx.get("gis_operations")
    if isinstance(structured, list):
        for i, item in enumerate(structured, 1):
            if isinstance(item, dict) and (item.get("target_id") or item.get("note") or item.get("action")):
                row = dict(item)
                row.setdefault("op_id", f"GIS-{i:02d}")
                out.append(row)

    edit_log = ctx.get("gis_edit_log")
    if isinstance(edit_log, list):
        for item in edit_log:
            op = gis_edit_item_to_op(item, len(out) + 1)
            if op:
                out.append(op)

    focus_ids = _parse_ids_raw(ctx.get("ids"))
    if focus_ids and out:
        id_set = set(focus_ids)
        for row in out:
            tid = str(row.get("target_id") or "")
            if tid and tid in id_set:
                row["focus"] = True
    elif focus_ids and not out:
        for i, lid in enumerate(focus_ids[:50], 1):
            out.append({
                "op_id": f"GIS-{i:02d}",
                "target_id": lid,
                "action": "关注路段",
                "params": "",
                "note": "param2.ids 列表中的路段/要素",
            })

    return out


def parse_ids_raw(raw: Any) -> list[str]:
    return _parse_ids_raw(raw)


def collect_gis_edit_log(
    prompt_blocks: dict[str, Any] | None = None,
    param2: dict[str, Any] | None = None,
) -> dict[str, Any]:
    ctx = _build_gis_context(prompt_blocks, param2)
    ops = parse_gis_operations_from_context(prompt_blocks, param2)
    focus_ids = parse_ids_raw(ctx.get("ids"))
    return {
        "operations": ops,
        "focus_ids": focus_ids,
        "text": format_gis_edit_log_text(ops, focus_ids=focus_ids),
        "count": len(ops),
        "entries": ctx.get("gis_edit_log") if isinstance(ctx.get("gis_edit_log"), list) else [],
    }


def merge_gis_operations(
    client_ops: list[dict[str, Any]] | None,
    llm_ops: list[dict[str, Any]] | None,
) -> list[dict[str, Any]]:
    client = [dict(op) for op in (client_ops or []) if isinstance(op, dict)]
    llm = [dict(op) for op in (llm_ops or []) if isinstance(op, dict)]
    seen = {(str(o.get("target_id") or ""), str(o.get("action") or "")) for o in client}
    merged = list(client)
    for op in llm:
        key = (str(op.get("target_id") or ""), str(op.get("action") or ""))
        if key in seen and key != ("", ""):
            continue
        merged.append(op)
        seen.add(key)
    return merged


def normalize_request_param2(data: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(data, dict):
        return data
    data["param2"] = parse_param2(data.get("param2"))
    return data


def format_gis_edit_log_text(ops: list[dict[str, Any]], *, focus_ids: list[str] | None = None) -> str:
    if not ops and not focus_ids:
        return "（无 GIS 编辑记录）"
    lines: list[str] = [f"共 {len(ops)} 条修改记录"]
    if focus_ids:
        lines.append(f"关注要素/路段 ID：{', '.join(focus_ids)}")
    for op in ops:
        if not isinstance(op, dict):
            continue
        changes = op.get("changes")
        if isinstance(changes, list) and changes:
            ch_text = _format_changes(changes)
        else:
            ch_text = str(op.get("params") or "")
        lines.append(
            f"- [{op.get('op_id', '')}] 对象 {op.get('target_id', '')} | "
            f"{op.get('action', '')} | {ch_text} | {op.get('note', '')}"
        )
    return "\n".join(lines)
