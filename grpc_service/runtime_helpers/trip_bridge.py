import argparse
import importlib
import json
import os
import sys
from typing import Callable


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
            f.write(f"{key}={_escape(value)}\n")


def _write_progress_file(path: str | None, percent: int, title: str) -> None:
    if not path:
        return
    safe_title = title.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"percent={percent}\ttitle={_escape(safe_title)}\n")


def _resolve_trip_root(cli_repo_root: str | None) -> str:
    if cli_repo_root:
        return os.path.abspath(cli_repo_root)
    env_root = os.environ.get("TNA_TRIP_ROOT", "").strip()
    if env_root:
        return os.path.abspath(env_root)
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "TripGeneration"))


def _load_handler(method: str):
    mapping = {
        "trip_generation": "grpc_server.handlers.trip_generation_handler",
        "trip_distribution": "grpc_server.handlers.trip_distribution_handler",
        "trip_model_all": "grpc_server.handlers.trip_model_all_handler",
    }
    module_name = mapping.get(method)
    if not module_name:
        raise ValueError(f"unsupported trip method: {method}")
    return importlib.import_module(module_name)


def _store_string_map(payload: dict[str, str], prefix: str, data: dict) -> None:
    for key, value in data.items():
        if isinstance(value, (dict, list)) or value is None:
            continue
        payload[f"{prefix}{key}"] = str(value)


def _store_int_map(payload: dict[str, str], prefix: str, data: dict) -> None:
    for key, value in data.items():
        if isinstance(value, bool):
            payload[f"{prefix}{key}"] = str(int(value))
        elif isinstance(value, int):
            payload[f"{prefix}{key}"] = str(value)


def _store_float_map(payload: dict[str, str], prefix: str, data: dict) -> None:
    for key, value in data.items():
        if isinstance(value, bool):
            continue
        if isinstance(value, float):
            payload[f"{prefix}{key}"] = str(value)


def _copy_single_result(payload: dict[str, str], result: dict) -> None:
    data = result.get("data") if isinstance(result.get("data"), dict) else {}
    summary = result.get("summary") if isinstance(result.get("summary"), dict) else {}

    tables = data.get("tables") if isinstance(data.get("tables"), dict) else {}
    counts = data.get("counts") if isinstance(data.get("counts"), dict) else {}
    metrics = data.get("metrics") if isinstance(data.get("metrics"), dict) else {}
    _store_string_map(payload, "data.table.", tables)
    _store_int_map(payload, "data.count.", counts)
    _store_float_map(payload, "data.metric.", metrics)

    if "stage" in summary and not isinstance(summary["stage"], (dict, list)):
        payload["summary.stage"] = str(summary["stage"])

    logs = summary.get("logs") if isinstance(summary.get("logs"), list) else []
    for idx, item in enumerate(logs):
        payload[f"summary.log.{idx}"] = str(item)

    for key, value in summary.items():
        if key in {"logs", "stage"}:
            continue
        if isinstance(value, bool):
            payload[f"data.count.{key}"] = str(int(value))
        elif isinstance(value, int):
            payload[f"data.count.{key}"] = str(value)
        elif isinstance(value, float):
            payload[f"data.metric.{key}"] = str(value)
        elif value is not None and not isinstance(value, (dict, list)):
            payload[f"summary.attr.{key}"] = str(value)


def _extract_payload(method: str, result: dict) -> dict[str, str]:
    payload: dict[str, str] = {}

    if method == "trip_model_all":
        payload["summary.stage"] = "trip_model_all"
        logs: list[str] = []
        for prefix, child in (("generation", result.get("generation")), ("distribution", result.get("distribution"))):
            if not isinstance(child, dict):
                continue
            child_data = child.get("data") if isinstance(child.get("data"), dict) else {}
            child_summary = child.get("summary") if isinstance(child.get("summary"), dict) else {}
            tables = child_data.get("tables") if isinstance(child_data.get("tables"), dict) else {}
            counts = child_data.get("counts") if isinstance(child_data.get("counts"), dict) else {}
            metrics = child_data.get("metrics") if isinstance(child_data.get("metrics"), dict) else {}
            _store_string_map(payload, f"data.table.{prefix}_", tables)
            _store_int_map(payload, f"data.count.{prefix}_", counts)
            _store_float_map(payload, f"data.metric.{prefix}_", metrics)
            for key, value in child_summary.items():
                if key == "logs":
                    continue
                if key == "stage":
                    continue
                elif isinstance(value, bool):
                    payload[f"data.count.{prefix}_{key}"] = str(int(value))
                elif isinstance(value, int):
                    payload[f"data.count.{prefix}_{key}"] = str(value)
                elif isinstance(value, float):
                    payload[f"data.metric.{prefix}_{key}"] = str(value)
                elif value is not None and not isinstance(value, (dict, list)):
                    payload[f"summary.attr.{prefix}_{key}"] = str(value)
            child_logs = child_summary.get("logs") if isinstance(child_summary.get("logs"), list) else []
            logs.extend([f"[{prefix}] {item}" for item in child_logs])
        for idx, item in enumerate(logs):
            payload[f"summary.log.{idx}"] = item
    else:
        _copy_single_result(payload, result)

    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--method", required=True)
    parser.add_argument("--request-file", required=True)
    parser.add_argument("--response-file", required=True)
    parser.add_argument("--progress-file", default=None)
    parser.add_argument("--repo-root", default=None)
    args = parser.parse_args()

    trip_root = _resolve_trip_root(args.repo_root)
    if trip_root not in sys.path:
        sys.path.insert(0, trip_root)

    req = _read_request_file(args.request_file)
    project_id = int(req.get("project_id", "0") or 0)
    user_id = int(req.get("user_id", "0") or 0)
    case_id = int(req.get("case_id", "0") or 0)
    param1 = req.get("param1", "")
    param2 = req.get("param2", "")

    def on_progress(iteration: int, percent: float) -> None:
        pct = int(max(0.0, min(100.0, float(percent))))
        _write_progress_file(args.progress_file, pct, f"step {iteration}")

    try:
        handler = _load_handler(args.method)
        code, message, data = handler.run(
            project_id=project_id,
            user_id=user_id,
            case_id=case_id,
            param1=param1,
            param2=param2,
            on_progress=on_progress,
        )
        payload: dict[str, str] = {}
        try:
            parsed = json.loads(data) if data else {}
            if isinstance(parsed, dict):
                payload = _extract_payload(args.method, parsed)
        except Exception:
            payload = {}
        _write_response_file(args.response_file, int(code), str(message), payload)
        return 0
    except Exception as exc:
        _write_response_file(args.response_file, -99, f"trip bridge error: {exc.__class__.__name__}: {exc}", {})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
