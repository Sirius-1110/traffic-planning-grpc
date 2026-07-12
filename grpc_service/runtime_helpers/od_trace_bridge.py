#!/usr/bin/env python3
"""
od_trace_bridge.py — 机动车 OD 溯源（方案库前缀 project{p}_user{u}_case{c}_）

param2:
  link_id          单路段溯源（与 min_v_c 二选一）
  min_v_c          批量：饱和度≥阈值（默认 0.8），不传 link_id 时启用
  top_n            每路段保留 OD 数，默认 30
  min_share_pct    最小占比过滤，默认 0
  max_segments     批量最多路段数，默认 200
  include_map_bundle  默认 true
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import traceback
from datetime import datetime
from typing import Any

try:
    from compare_scope_utils import kv_unescape
except ImportError:

    def kv_unescape(text: str) -> str:
        if not text:
            return text
        out: list[str] = []
        i = 0
        while i < len(text):
            if text[i] == "\\" and i + 1 < len(text):
                nxt = text[i + 1]
                out.append("\n" if nxt == "n" else "\r" if nxt == "r" else nxt)
                i += 2
                continue
            out.append(text[i])
            i += 1
        return "".join(out)

from od_trace_core import trace_saturated_links

CSV_COLUMNS = [
    "segment_link_id",
    "origin_zone",
    "dest_zone",
    "origin_id",
    "dest_id",
    "path_flow",
    "initial_od",
    "estimated_od",
    "share_pct",
    "n_paths",
    "path_travel_time_min",
    "path_free_flow_min",
]


def _read_kv(path: str) -> dict[str, str]:
    kv: dict[str, str] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = kv_unescape(v.strip())
    return kv


def _write_kv(path: str, d: dict[str, str]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        for k, v in d.items():
            f.write(f"{k}={v}\n")


def _fail(out_path: str, msg: str) -> None:
    _write_kv(out_path, {"code": "-1", "message": msg})
    sys.exit(0)


def _success(out_path: str, payload: dict[str, str]) -> None:
    _write_kv(out_path, payload)
    sys.exit(0)


def _load_db_conf(conf_path: str = "/opt/algorithms/db.conf") -> dict[str, str]:
    cfg: dict[str, str] = {}
    for p in [conf_path, "/home/giss/opt_algorithms/db.conf"]:
        try:
            with open(p, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if "=" in line and not line.startswith("#"):
                        k, v = line.split("=", 1)
                        cfg[k.strip()] = v.strip()
            if cfg:
                break
        except OSError:
            pass
    return cfg


def _connect(cfg: dict[str, str]):
    import psycopg2

    return psycopg2.connect(
        host=cfg.get("host", "localhost"),
        port=int(cfg.get("port", 5432)),
        dbname=cfg.get("dbname", "urban"),
        user=cfg.get("user", "urban"),
        password=cfg.get("password", ""),
    )


def _parse_param2(raw: str) -> dict[str, Any]:
    if not raw or not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise ValueError(f"param2 JSON 解析失败: {e}") from e


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--request-file")
    parser.add_argument("--response-file")
    args, _ = parser.parse_known_args()

    req_path = args.request_file or os.environ.get("TNA_INPUT_FILE", "")
    resp_path = args.response_file or os.environ.get("TNA_OUTPUT_FILE", "")
    if not req_path or not resp_path:
        print("[od_trace_bridge] missing request/response file", file=sys.stderr)
        sys.exit(1)

    kv = _read_kv(req_path)
    pid = int(kv.get("project_id", 0))
    uid = int(kv.get("user_id", 0))
    cid = int(kv.get("case_id", -1))
    if pid <= 0 or uid <= 0:
        _fail(resp_path, "project_id and user_id must be > 0")
    if cid < 0:
        _fail(resp_path, "case_id is required for scheme-prefix od_trace")

    try:
        p2 = _parse_param2(kv.get("param2", ""))
    except ValueError as e:
        _fail(resp_path, str(e))

    link_id_raw = p2.get("link_id")
    min_v_c = float(p2.get("min_v_c") or p2.get("saturation_threshold") or 0.8)
    top_n = int(p2.get("top_n") or 30)
    min_share = float(p2.get("min_share_pct") or 0.0)
    max_segments = int(p2.get("max_segments") or 200)
    include_map = p2.get("include_map_bundle", True)
    if isinstance(include_map, str):
        include_map = include_map.lower() not in ("0", "false", "no")

    link_id: int | None = int(link_id_raw) if link_id_raw is not None else None

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    if link_id is not None:
        base_stem = f"od_trace_p{pid}_u{uid}_c{cid}_link{link_id}_{ts}"
    else:
        base_stem = f"od_trace_p{pid}_u{uid}_c{cid}_vc{min_v_c}_{ts}"
    json_path = p2.get("output_path") or f"/tmp/{base_stem}.json"
    csv_path = json_path.replace(".json", ".csv")
    map_dir = p2.get("map_dir") or f"/tmp/{base_stem}_map"

    prefix = f"project{pid}_user{uid}_case{cid}_"
    db_conf = os.environ.get("TNA_DB_CONF", "/opt/algorithms/db.conf")
    try:
        conn = _connect(_load_db_conf(db_conf))
    except Exception as e:
        _fail(resp_path, f"数据库连接失败: {e}")

    html_path = ""
    map_data_path = ""
    try:
        result = trace_saturated_links(
            conn,
            prefix,
            link_id=link_id,
            min_v_c=min_v_c,
            top_n=top_n,
            min_share_pct=min_share,
            max_segments=max_segments,
        )
        ctx = result.pop("_ctx", {})
        export = {k: v for k, v in result.items() if not k.startswith("_")}

        if include_map:
            try:
                from od_trace_map_builder import build_map_bundle, write_map_bundle

                bundle = build_map_bundle(
                    conn,
                    prefix,
                    export,
                    pid=pid,
                    uid=uid,
                    cid=cid,
                    zone_labels=ctx.get("zone_labels"),
                )
                html_path, map_data_path = write_map_bundle(bundle, map_dir, stem="od_trace_map")
                export["map_bundle"] = {
                    "html_path": html_path,
                    "data_js_path": map_data_path,
                    "map_dir": map_dir,
                }
            except Exception as e:
                export.setdefault("warnings", []).append(f"map_bundle: {e}\n{traceback.format_exc()[-300:]}")
    except Exception as e:
        _fail(resp_path, f"OD 溯源失败: {e}\n{traceback.format_exc()[-400:]}")
    finally:
        try:
            conn.close()
        except Exception:
            pass

    try:
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(export, f, ensure_ascii=False, indent=2)
        with open(csv_path, "w", encoding="utf-8", newline="") as f:
            w = csv.DictWriter(f, fieldnames=CSV_COLUMNS, extrasaction="ignore")
            w.writeheader()
            for row in export.get("contributions") or []:
                w.writerow(row)
    except OSError as e:
        _fail(resp_path, f"写产物失败: {e}")

    meta = export.get("meta") or {}
    segments = export.get("segments") or []
    contribs = export.get("contributions") or []
    n_od = len(contribs)
    n_seg = len(segments)
    top1 = contribs[0]["share_pct"] if contribs else 0.0

    if link_id is not None:
        msg = f"OD 溯源完成：link {link_id}，{n_od} 条 OD 贡献"
    else:
        msg = f"饱和度≥{min_v_c} 批量溯源：{n_seg} 路段，{n_od} 条 OD 贡献"

    payload = {
        "code": "1",
        "message": msg,
        "summary.stage": "od_trace",
        "summary.attr.json_path": json_path,
        "summary.attr.csv_path": csv_path,
        "summary.attr.min_v_c": str(min_v_c),
        "summary.count.segments": str(n_seg),
        "summary.count.od_pairs": str(n_od),
        "summary.count.paths_total": str(n_od),
        "summary.metrics.saturated_link_total": str(meta.get("saturated_link_total") or 0),
        "summary.metrics.truncated": "1" if meta.get("truncated") else "0",
        "summary.metrics.top1_share_pct": str(top1),
    }
    if link_id is not None:
        payload["summary.attr.target_link_id"] = str(link_id)
        seg0 = segments[0] if segments else {}
        payload["summary.attr.target_volume"] = str(seg0.get("volume") or 0)
        payload["summary.metrics.path_flow_sum"] = str(seg0.get("path_flow_sum") or 0)
    if html_path:
        payload["summary.attr.html_path"] = html_path
        payload["summary.attr.map_data_js_path"] = map_data_path
        payload["summary.attr.map_dir"] = map_dir
    _success(resp_path, payload)


if __name__ == "__main__":
    main()
