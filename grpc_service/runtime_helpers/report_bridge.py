#!/usr/bin/env python3
"""report_bridge.py

gRPC 桥接脚本，供 C++ gRPC server 通过 run_request_response_command 调用。
支持三类任务（通过 --task 参数区分）：
  1  report_export       承载力分析报告（图表 + Markdown/HTML + ZIP）
  2  flow_export         TESSNG 仿真流量 Flow.csv
  3  sim_network_export  仿真路网 OSM 文件拷贝

调用方式：
  python report_bridge.py --task 1 \\
      --request-file /tmp/tna_req_xxx --response-file /tmp/tna_resp_xxx \\
      [--progress-file /tmp/tna_prog_xxx] [--report-root /opt/algorithms/report_app]

参数映射（来自请求 KV 文件）：
  project_id  → prefix = "project{project_id}_user{user_id}"
  user_id     → 同上
  case_id     → scenario_name = "case{case_id}"（含 case_id=0 → case0）
  param1      → business_type（默认 "carrying_capacity"）

数据库配置：
  优先读取环境变量 TNA_REPORT_PG_JSON（JSON 格式 pgadmin_conf.json 结构），其次解析
  TNA_DB_CONF 指向的 libpq key=value 格式 db.conf。
  两者都会作为 TNA_REPORT_PG_JSON 环境变量传给 run_tasks.py 子进程。
"""

import argparse
import json
import os
import re
import subprocess
import sys


# ---------------------------------------------------------------------------
# KV 文件 读/写（与 trip_bridge.py 保持一致）
# ---------------------------------------------------------------------------

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


def _write_response_file(path: str, code: int, message: str, payload: dict[str, str]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"code={code}\n")
        f.write(f"message={_escape(message)}\n")
        for key, value in payload.items():
            f.write(f"{key}={_escape(str(value))}\n")


def _write_progress_file(path: str | None, percent: int, title: str) -> None:
    if not path:
        return
    safe = title.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"percent={percent}\ttitle={_escape(safe)}\n")


# ---------------------------------------------------------------------------
# 数据库配置解析
# ---------------------------------------------------------------------------

def _parse_libpq_conf(path: str) -> dict:
    cfg: dict = {}
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for m in re.finditer(r"(\w+)\s*=\s*(\S+)", text):
            cfg[m.group(1)] = m.group(2)
    except Exception:
        pass
    return {
        "host": cfg.get("host", "localhost"),
        "port": int(cfg.get("port", 5432)),
        "dbname": cfg.get("dbname", "urban"),
        "user": cfg.get("user", ""),
        "password": cfg.get("password", ""),
        "connection_string": "",
    }


def _resolve_pg_json() -> str:
    """返回数据库配置 JSON 字符串（优先 TNA_REPORT_PG_JSON > 解析 db.conf）。"""
    env_json = os.environ.get("TNA_REPORT_PG_JSON", "").strip()
    if env_json:
        return env_json

    db_conf_path = os.environ.get("TNA_DB_CONF", "").strip()
    if not db_conf_path:
        db_conf_path = "/opt/algorithms/db.conf"

    cfg = _parse_libpq_conf(db_conf_path)
    return json.dumps(cfg)


# ---------------------------------------------------------------------------
# report_app 根目录
# ---------------------------------------------------------------------------

def _resolve_report_root(cli_root: str | None) -> str:
    if cli_root:
        return os.path.abspath(cli_root)
    env_root = os.environ.get("TNA_REPORT_ROOT", "").strip()
    if env_root:
        return os.path.abspath(env_root)
    return os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "report_app")
    )


# ---------------------------------------------------------------------------
# 预测输出目录（与 run_tasks.py 中 build_task_dir_name + ensure_output_dir 规则一致）
# ---------------------------------------------------------------------------

def _predict_output_dir(report_root: str, prefix: str, scenario_name: str, task_type: int) -> str:
    if scenario_name:
        dir_name = f"task{task_type}_{scenario_name}_results"
    else:
        dir_name = f"task{task_type}_default_result"
    return os.path.join(report_root, prefix, dir_name)


# ---------------------------------------------------------------------------
# 主逻辑
# ---------------------------------------------------------------------------

def _run(task: int, project_id: int, user_id: int, case_id: int,
         business_type: str, report_root: str,
         progress_path: str | None) -> tuple[int, str, dict[str, str]]:

    run_tasks_py = os.path.join(report_root, "run_tasks.py")
    if not os.path.isfile(run_tasks_py):
        return -1, f"run_tasks.py not found at {run_tasks_py}", {}

    prefix = f"project{project_id}_user{user_id}"
    scenario_name = f"case{case_id}"
    schema = "user_project"

    _write_progress_file(progress_path, 5, "正在启动子进程...")

    # 构建子进程环境（继承当前环境，注入 DB 配置）
    env = os.environ.copy()
    env["TNA_REPORT_PG_JSON"] = _resolve_pg_json()

    # run_tasks.py 使用位置参数：task_type schema prefix scenario_name business_type
    cmd = [sys.executable, run_tasks_py,
           str(task), schema, prefix, scenario_name, business_type]

    try:
        result = subprocess.run(
            cmd,
            cwd=report_root,
            env=env,
            capture_output=True,
            text=True,
            timeout=600,
        )
    except subprocess.TimeoutExpired:
        return -99, "run_tasks.py timed out after 600s", {}
    except Exception as exc:
        return -99, f"subprocess error: {exc}", {}

    stdout = result.stdout.strip()
    stderr = result.stderr.strip()
    combined_log = "\n".join(filter(None, [stdout, stderr]))

    if result.returncode != 0:
        return -99, f"run_tasks.py exit={result.returncode}\n{combined_log}", {}

    _write_progress_file(progress_path, 90, "子进程完成，整理结果...")

    out_dir = _predict_output_dir(report_root, prefix, scenario_name, task)
    payload: dict[str, str] = {
        "data.table.output_dir": out_dir if os.path.isdir(out_dir) else "",
        "summary.stage": ["report_export", "flow_export", "sim_network_export"][task - 1],
        "summary.log": combined_log[:2000],
    }

    if task == 1:
        zip_path = out_dir + ".zip" if os.path.isdir(out_dir) else ""
        payload["data.table.zip_path"] = zip_path if os.path.exists(zip_path) else ""

    elif task == 2:
        if os.path.isdir(out_dir):
            csv_files = [f for f in os.listdir(out_dir) if f.endswith("_flow.csv")]
            if csv_files:
                flow_csv = os.path.join(out_dir, csv_files[0])
                payload["data.table.flow_csv_path"] = flow_csv
                with open(flow_csv, encoding="utf-8", errors="replace") as fh:
                    row_count = max(0, sum(1 for _ in fh) - 1)
                payload["data.count.flow_rows"] = str(row_count)

    elif task == 3:
        if os.path.isdir(out_dir):
            dir_name = os.path.basename(out_dir)
            osm_path = os.path.join(out_dir, f"{dir_name}.osm")
            payload["data.table.osm_path"] = osm_path if os.path.exists(osm_path) else ""

    return 1, "Success", payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--task", type=int, required=True,
                        help="1=report_export  2=flow_export  3=sim_network_export")
    parser.add_argument("--request-file", required=True)
    parser.add_argument("--response-file", required=True)
    parser.add_argument("--progress-file", default=None)
    parser.add_argument("--report-root", default=None,
                        help="Directory containing run_tasks.py")
    args = parser.parse_args()

    req = _read_request_file(args.request_file)
    project_id = int(req.get("project_id", "0") or 0)
    user_id    = int(req.get("user_id",    "0") or 0)
    case_id    = int(req.get("case_id",    "0") or 0)
    param1     = req.get("param1", "").strip()
    business_type = param1 if param1 else "carrying_capacity"

    if project_id <= 0 or user_id <= 0:
        _write_response_file(args.response_file, -1,
                             "project_id and user_id are required positive integers.", {})
        return 1

    report_root = _resolve_report_root(args.report_root)

    code, message, payload = _run(
        task=args.task,
        project_id=project_id,
        user_id=user_id,
        case_id=case_id,
        business_type=business_type,
        report_root=report_root,
        progress_path=args.progress_file,
    )
    _write_response_file(args.response_file, code, message, payload)
    return 0 if code == 1 else 1


if __name__ == "__main__":
    raise SystemExit(main())
