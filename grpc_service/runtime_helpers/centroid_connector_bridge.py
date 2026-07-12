#!/usr/bin/env python3
"""质心连杆 gRPC bridge（与 trip_bridge 同 request/response 文件协议）。"""
from __future__ import annotations

import argparse
import json
import os
import sys

_HELPERS = os.path.dirname(os.path.abspath(__file__))
if _HELPERS not in sys.path:
    sys.path.insert(0, _HELPERS)

from centroid_connector_service import run_build_centroid_connectors  # noqa: E402


def _read_request_file(path: str) -> dict[str, str]:
    data: dict[str, str] = {}
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            data[key] = _unescape(value)
    return data


def _unescape(text: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "n":
                out.append("\n")
            elif nxt == "r":
                out.append("\r")
            elif nxt == "\\":
                out.append("\\")
            else:
                out.append(nxt)
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n")


def _write_response_file(path: str, code: int, message: str, payload: dict[str, str]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"code={code}\n")
        f.write(f"message={_escape(message)}\n")
        for key, value in payload.items():
            f.write(f"{key}={_escape(str(value))}\n")


def _write_progress_file(path: str | None, percent: int, title: str) -> None:
    if not path:
        return
    safe_title = title.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"percent={percent}\ttitle={_escape(safe_title)}\n")


def _payload_from_result(result: dict) -> dict[str, str]:
    payload: dict[str, str] = {}
    data = result.get("data") if isinstance(result.get("data"), dict) else {}
    summary = result.get("summary") if isinstance(result.get("summary"), dict) else {}
    tables = data.get("tables") if isinstance(data.get("tables"), dict) else {}
    counts = data.get("counts") if isinstance(data.get("counts"), dict) else {}
    for k, v in tables.items():
        payload[f"data.table.{k}"] = str(v)
    for k, v in counts.items():
        payload[f"data.count.{k}"] = str(v)
    if summary.get("stage"):
        payload["summary.stage"] = str(summary["stage"])
    attrs = summary.get("attributes") if isinstance(summary.get("attributes"), dict) else {}
    for k, v in attrs.items():
        payload[f"summary.attr.{k}"] = str(v)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--request-file", required=True)
    parser.add_argument("--response-file", required=True)
    parser.add_argument("--progress-file", default=None)
    args = parser.parse_args()

    req = _read_request_file(args.request_file)
    project_id = int(req.get("project_id", "0") or 0)
    user_id = int(req.get("user_id", "0") or 0)
    case_id = int(req.get("case_id", "0") or 0)
    param1 = req.get("param1", "")
    param2 = req.get("param2", "")

    def on_progress(percent: int, title: str) -> None:
        _write_progress_file(args.progress_file, percent, title)

    try:
        _write_progress_file(args.progress_file, 0, "正在构建质心连杆...")
        code, message, result = run_build_centroid_connectors(
            project_id, user_id, case_id, param1, param2, on_progress=on_progress
        )
        payload = _payload_from_result(result)
        _write_response_file(args.response_file, code, message, payload)
        return 0 if code == 1 else 1
    except Exception as e:
        _write_response_file(args.response_file, -99, f"centroid_connector_bridge: {e}", {})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
