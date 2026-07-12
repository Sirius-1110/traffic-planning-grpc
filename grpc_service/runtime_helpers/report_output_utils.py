"""报告 HTML 默认输出路径：固定 tna_* 旧命名（除非 param2.output_path 显式指定）。

project_name 仅用于报告页眉展示，不改变默认文件名。
"""
from __future__ import annotations

import json
import re
from datetime import datetime
from typing import Any


def parse_report_param2(raw: Any) -> dict:
    """解析 gRPC param2；失败或非 dict 时返回空 dict。支持双重 JSON 字符串。"""
    if isinstance(raw, dict):
        return raw
    if raw is None:
        return {}
    text = str(raw).strip()
    if not text:
        return {}
    for _ in range(2):
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            return {}
        if isinstance(parsed, dict):
            return parsed
        if isinstance(parsed, str):
            text = parsed.strip()
            continue
        return {}
    return {}


def sanitize_project_name(name: object) -> str:
    if name is None:
        return ""
    s = str(name).strip()
    if not s:
        return ""
    s = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "_", s)
    s = re.sub(r"\s+", "_", s)
    return s[:80]


def report_date_tag() -> str:
    return datetime.now().strftime("%Y%m%d")


def project_display_name(param2: dict, project_id: int) -> str:
    """报告页眉展示名：仅认 param2.project_name，否则「项目 {id}」。"""
    name = sanitize_project_name(param2.get("project_name"))
    if name:
        return name
    return f"项目 {project_id}"


def resolve_report_output_path(
    param2: dict,
    *,
    report_stem: str,
    fallback_path: str,
) -> str:
    """
    优先级：param2.output_path > fallback_path（tna_* 旧默认路径）。
    project_name 仅用于报告页眉展示（project_display_name），不参与文件名。
    report_stem 保留参数以兼容各 bridge 调用，不再用于默认路径。
    """
    explicit = str(param2.get("output_path") or "").strip()
    if explicit:
        return explicit
    return fallback_path
