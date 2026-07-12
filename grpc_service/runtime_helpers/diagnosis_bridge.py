"""诊断指标桥接：被 C++ gRPC 服务通过 subprocess 调用。

调用约定与 trip_bridge.py 一致（基于 request-file/response-file/progress-file 的 KV 文本协议），
新增 --module {pt_meso|pt_macro|road_macro|road_micro|slow_macro|slow_meso_micro}：

* 一次调用对应统一服务的一个 diagnosis_<module> 方法；
* **宏观**（`road_macro` / `slow_macro` / `pt_macro`）：全方案完整路网与数据，**不接受** `link_ids` / `bbox` / `line_ids` 等局部范围筛选；
* **中微观**（`road_micro` / `slow_meso_micro` / `pt_meso`）：支持局部诊断，param2 可携带：
    - `indicator_codes`: list[str]，缺省=该 module 全部指标；
    - `target_link_id` / `link_ids` / `bbox`: road_micro 路段范围（至少一项；`link_ids`+`bbox` 取交集）；
    - `line_ids`: pt_meso 公交线路范围（局部点选；不传则方案内全部线路）；
    - `link_ids` / `bbox`: slow_meso_micro 微观路段范围（可选；仅过滤 `slow_micro_*`）；
    - `walk_type_codes` / `bike_type_codes` / `topn`: 慢行设施类型/算法参数（非空间范围）；
* 表名一律由 `project_id/user_id/case_id` + 固定后缀派生；param2 中的 *_table_name 字段被静默忽略；
* 输出统一写入 `{schema}.{prefix}_diagnosis_indicator_result_rows`，按 indicator_code 先删后插。
"""
import argparse
import importlib
import importlib.util
import json
import os
import sys


# 各 module 暴露的 fs_run_* 入口及对应 indicator_code、所需的 param2 额外字段。
MODULE_INDICATORS = {
    "pt_meso": [
        ("fs_run_total_routes", "pt_meso_total_routes"),
        ("fs_run_avg_mileage", "pt_meso_avg_mileage"),
        ("fs_run_avg_stations", "pt_meso_avg_stations"),
        ("fs_run_max_mileage", "pt_meso_max_mileage"),
        ("fs_run_max_stations", "pt_meso_max_stations"),
    ],
    "pt_macro": [
        ("fs_run_bus_density", "pt_macro_bus_density"),
        ("fs_run_pt_pop_coverage_500m", "pt_macro_pt_pop_coverage_500m"),
        ("fs_run_city_bus_area_coverage", "pt_macro_city_bus_area_coverage"),
        ("fs_run_rail_pop_coverage_800m", "pt_macro_rail_pop_coverage_800m"),
    ],
    "road_macro": [
        ("fs_run_road_density", "road_macro_density"),
        ("fs_run_tpi", "road_macro_tpi"),
        ("fs_run_congestion_ratio", "road_macro_congestion_ratio"),
        ("fs_run_average_speed", "road_macro_average_speed"),
        ("fs_run_tti", "road_macro_tti"),
        ("fs_run_dti", "road_macro_dti"),
        ("fs_run_speed_relation", "road_macro_speed_relation"),
        ("fs_run_capacity_distribution", "road_macro_capacity_distribution"),
        ("fs_run_co2_emission", "road_macro_co2"),
    ],
    "road_micro": [
        ("fs_run_segment_flow", "road_micro_flow"),
        ("fs_run_segment_speed_kmh", "road_micro_speed_kmh"),
        ("fs_run_segment_v_c", "road_micro_v_c"),
        ("fs_run_segment_tti", "road_micro_tti"),
        ("fs_run_segment_dti", "road_micro_dti"),
        ("fs_run_segment_source_tracing", "road_micro_source_tracing"),
    ],
    "slow_macro": [
        ("fs_run_slow_walk_density",         "slow_macro_walk_density"),
        ("fs_run_slow_bike_density",         "slow_macro_bike_density"),
        ("fs_run_slow_walk_continuity",      "slow_macro_walk_continuity"),
        ("fs_run_slow_bike_continuity",      "slow_macro_bike_continuity"),
        ("fs_run_slow_area_percap",          "slow_macro_area_percap"),
        ("fs_run_slow_macro_vc_distribution","slow_macro_vc_distribution"),
    ],
    "slow_meso_micro": [
        ("fs_run_slow_meso_walk_density",    "slow_meso_walk_density"),
        ("fs_run_slow_meso_bike_density",    "slow_meso_bike_density"),
        ("fs_run_slow_meso_walk_continuity", "slow_meso_walk_continuity"),
        ("fs_run_slow_meso_bike_continuity", "slow_meso_bike_continuity"),
        ("fs_run_slow_meso_area_percap",     "slow_meso_area_percap"),
        ("fs_run_slow_micro_vc",             "slow_micro_vc"),
        ("fs_run_slow_micro_topn_vc",        "slow_micro_topn_vc"),
    ],
}

MODULE_LABELS = {
    "pt_meso": "公交中微观诊断",
    "pt_macro": "公交宏观诊断",
    "road_macro": "机动车宏观诊断",
    "road_micro": "机动车中微观诊断",
    "slow_macro": "慢行宏观诊断",
    "slow_meso_micro": "慢行中微观诊断",
}


def _module_label(module_key: str) -> str:
    return MODULE_LABELS.get(module_key, f"诊断({module_key})")


def _humanize_diagnosis_error(raw: str, module_key: str, extra: dict | None = None) -> str:
    """将底层英文/数据库异常转为简短中文说明，写入 ResultData.message。"""
    extra = extra or {}
    text = (raw or "").strip()
    if not text:
        return "未知错误"

    low = text.lower()
    if "undefinedcolumn" in low and "geometry" in low:
        if extra.get("bbox"):
            return (
                "路网表缺少 geometry 几何列，无法按 bbox 框选路段；"
                "请补充 road_way.geometry，或改用 link_ids / target_link_id"
            )
        return "路网表缺少 geometry 几何列，空间筛选不可用"

    if module_key == "road_micro" and (
        "必须在 param2" in text
        or "link_ids/bbox 之一" in text
        or "must provide" in low
    ):
        return "未指定路段范围：param2 须包含 target_link_id、link_ids 或 bbox 至少一项"

    if "invalid" in low and "line_id" in low:
        return "line_ids 中的线路 ID 在方案公交线路表中不存在"

    if "relation" in low and "does not exist" in low:
        return "缺少所需数据库表，请先完成分配或检查方案输入数据"

    if ("empty" in low or "no data" in low or "rows=0" in low) and (
        "flow" in low or "greedy" in low or "分配" in text
    ):
        return "分配流量表为空或无有效记录，请先运行对应交通分配（如 base_motor_network）"

    if "connection" in low and ("refused" in low or "closed" in low):
        return "数据库连接失败，请检查 db.conf 与 PostgreSQL 服务"

    if "permission denied" in low or "authentication failed" in low:
        return "数据库认证失败，请检查 db.conf 中的账号密码"

    # 去掉冗长 SQL 片段，保留异常类型与首句
    for sep in ("\nLINE ", "\n"):
        if sep in text:
            text = text.split(sep, 1)[0].strip()
    if ":" in text:
        head, tail = text.split(":", 1)
        if head.endswith("Error") or head in ("UndefinedColumn", "ProgrammingError"):
            text = tail.strip() or text
    if len(text) > 100:
        return text[:100] + "…"
    return text


def _summarize_failure_reasons(failures: list[str], module_key: str, extra: dict) -> str:
    """从各指标失败信息中提取不重复的中文原因（用于 message）。"""
    seen: list[str] = []
    for item in failures:
        raw = item.split(":", 1)[1] if ":" in item else item
        hint = raw.strip() or _humanize_diagnosis_error(raw, module_key, extra)
        if hint and hint not in seen:
            seen.append(hint)
    return "；".join(seen[:3])


# ---------------- KV 协议读写（与 trip_bridge 完全一致） ----------------

def _read_request_file(path: str) -> dict:
    data = {}
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            data[key] = _unescape(value)
    return data


def _unescape(text: str) -> str:
    out, i = [], 0
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


def _write_response_file(path: str, code: int, message: str, payload: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"code={code}\n")
        f.write(f"message={_escape(message)}\n")
        for key, value in payload.items():
            f.write(f"{key}={_escape(str(value))}\n")


def _write_progress_file(path, percent: int, title: str) -> None:
    if not path:
        return
    safe_title = title.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    with open(path, "a", encoding="utf-8") as f:
        f.write(f"percent={percent}\ttitle={_escape(safe_title)}\n")


# ---------------- 解析数据库连接配置 ----------------

def _parse_param1(param1: str) -> dict:
    """解析 param1：libpq key=value 串、`;` 分隔串、或裸 DSN。

    返回 psycopg2.connect 风格的字典；若无可识别键则返回 {"connection_string": param1}。
    """
    if not param1 or not param1.strip():
        return {}
    kv = {}
    for part in param1.replace(";", "\n").splitlines():
        part = part.strip()
        if "=" in part and not part.startswith("#"):
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    if not kv:
        return {"connection_string": param1.strip()}
    if "port" in kv:
        try:
            kv["port"] = int(kv["port"])
        except ValueError:
            pass
    return kv


def _load_db_conf_fallback() -> dict:
    """param1 为空时回退到 db.conf：`TNA_DB_CONF` -> /opt/algorithms/db.conf。"""
    candidates = [
        os.environ.get("TNA_DB_CONF", ""),
        "/opt/algorithms/db.conf",
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            kv = {}
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
            if "port" in kv:
                try:
                    kv["port"] = int(kv["port"])
                except ValueError:
                    pass
            return kv
    raise FileNotFoundError(
        "未找到 db.conf：请通过 ParamsData.param1 提供 libpq 连接串或设置 TNA_DB_CONF/部署 /opt/algorithms/db.conf。"
    )


def _build_cfg(param1: str, project_id: int, user_id: int, case_id: int) -> tuple:
    db = _parse_param1(param1)
    if not db:
        db = _load_db_conf_fallback()

    schema = str(db.get("role", "")).strip() or os.environ.get("TNA_DB_SCHEMA", "").strip() or "user_project"
    prefix = f"project{project_id}_user{user_id}_case{case_id}_"

    db["role"] = schema
    db["scenario_prefix"] = prefix

    cfg = {"db_conn_str": db}
    return cfg, schema, prefix


# ---------------- 入口模块加载 ----------------

def _resolve_diagnosis_root(cli_repo_root) -> str:
    if cli_repo_root:
        return os.path.abspath(cli_repo_root)
    env_root = os.environ.get("TNA_DIAGNOSIS_ROOT", "").strip()
    if env_root:
        return os.path.abspath(env_root)
    # 仓库布局兜底：tna_unified_grpc_service 与 diagnosis-诊断指标 平级
    here = os.path.dirname(os.path.abspath(__file__))
    candidate = os.path.abspath(os.path.join(here, "..", "..", "..", "..", "..", "diagnosis-诊断指标"))
    return candidate


def _load_entry_module(diagnosis_root: str):
    if diagnosis_root not in sys.path:
        sys.path.insert(0, diagnosis_root)
    entry_path = os.path.join(diagnosis_root, "diagnostic_dll_entry.py")
    if not os.path.isfile(entry_path):
        raise FileNotFoundError(f"找不到 diagnostic_dll_entry.py：{entry_path}")
    spec = importlib.util.spec_from_file_location("diagnostic_dll_entry", entry_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"无法加载 diagnostic_dll_entry.py：{entry_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# ---------------- 单指标调用 ----------------

def _build_table_name(cfg: dict, bare_name: str) -> str:
    """按统一规范拼出 `{schema}.{prefix}{bare_name}`，与 BuildPostgreSQL/WritePG 生成的真实表名一致。"""
    db = cfg.get("db_conn_str", {}) if isinstance(cfg, dict) else {}
    schema = str(db.get("role", "user_project") or "user_project").strip()
    prefix = str(db.get("scenario_prefix", "") or "").strip()
    return f"{schema}.{prefix}{bare_name}"


def _cfg_with_slow_extras(cfg: dict, extra: dict) -> dict:
    cfg_with_types = dict(cfg)
    db_copy = dict(cfg_with_types.get("db_conn_str", {}))
    cfg_with_types["db_conn_str"] = db_copy
    if "walk_type_codes" in extra:
        cfg_with_types["walk_type_codes"] = [int(x) for x in extra["walk_type_codes"]]
    if "bike_type_codes" in extra:
        cfg_with_types["bike_type_codes"] = [int(x) for x in extra["bike_type_codes"]]
    if "walk_lane_width_m" in extra:
        cfg_with_types["walk_lane_width_m"] = float(extra["walk_lane_width_m"])
    if "bike_lane_width_m" in extra:
        cfg_with_types["bike_lane_width_m"] = float(extra["bike_lane_width_m"])
    if "topn" in extra:
        cfg_with_types["topn"] = int(extra["topn"])
    return cfg_with_types


def _call_indicator(entry_module, module_key: str, fs_name: str, cfg: dict, target_link_id, on_progress, extra: dict = None):
    """显式注入统一规范下的真实表名，绕开诊断库内部的 raw_name 推导。"""
    fn = getattr(entry_module, fs_name)
    out_table = _build_table_name(cfg, "diagnosis_indicator_result_rows")
    extra = extra or {}

    if module_key == "pt_meso":
        bus = _build_table_name(cfg, "other_bus_route")
        line_ids_param = extra.get("line_ids")
        return fn(on_progress, cfg, bus, out_table, line_ids=line_ids_param)

    if module_key == "pt_macro":
        taz = _build_table_name(cfg, "road_community")
        if fs_name == "fs_run_bus_density":
            bus = _build_table_name(cfg, "other_bus_route")
            return fn(on_progress, cfg, taz, bus, out_table)
        if fs_name == "fs_run_pt_pop_coverage_500m":
            pt_station = _build_table_name(cfg, "pt_station_table")
            return fn(on_progress, cfg, taz, pt_station, out_table)
        if fs_name == "fs_run_city_bus_area_coverage":
            city_stop = _build_table_name(cfg, "city_bus_stop_table")
            return fn(on_progress, cfg, taz, city_stop, out_table)
        if fs_name == "fs_run_rail_pop_coverage_800m":
            rail = _build_table_name(cfg, "rail_station_table")
            return fn(on_progress, cfg, taz, rail, out_table)
        raise ValueError(f"未知 pt_macro 入口: {fs_name}")

    if module_key == "road_macro":
        net = _build_table_name(cfg, "road_way")
        flow = _build_table_name(cfg, "greedy_link_flow_results")
        taz = _build_table_name(cfg, "road_community")
        return fn(on_progress, cfg, net, flow, taz, out_table)

    if module_key == "road_micro":
        # 兼容三种调用方式：
        # 1）仅 target_link_id（历史版本）；
        # 2）link_ids 列表（多路段筛选）；
        # 3）bbox（按空间范围筛选，可与 link_ids 组合）。
        extra = extra or {}
        link_ids_param = extra.get("link_ids")
        bbox_param = extra.get("bbox")

        if not link_ids_param and not bbox_param and target_link_id is None:
            raise ValueError(
                "未指定路段范围：param2 须包含 target_link_id、link_ids 或 bbox 至少一项"
            )

        net = _build_table_name(cfg, "road_way")
        flow = _build_table_name(cfg, "greedy_link_flow_results")
        path = _build_table_name(cfg, "greedy_path_results")

        # 旧模式：仅按 target_link_id 单路段诊断（不传 link_ids/bbox）。
        if not link_ids_param and not bbox_param:
            return fn(on_progress, cfg, target_link_id, net, flow, path, out_table)

        # 新模式：支持 link_ids / bbox。底层 fs_run_* 已扩展为接受关键字参数。
        return fn(
            on_progress,
            cfg,
            target_link_id,
            net,
            flow,
            path,
            out_table,
            link_ids=link_ids_param,
            bbox=bbox_param,
        )

    if module_key == "slow_macro":
        cfg_with_types = _cfg_with_slow_extras(cfg, extra)
        slow = _build_table_name(cfg, "slow_road_way")
        flow = _build_table_name(cfg, "slow_greedy_link_flow_results")
        taz = _build_table_name(cfg, "road_community")
        return fn(on_progress, cfg_with_types, slow, flow, taz, out_table)

    if module_key == "slow_meso_micro":
        cfg_with_types = _cfg_with_slow_extras(cfg, extra)
        slow = _build_table_name(cfg, "slow_road_way")
        flow = _build_table_name(cfg, "slow_greedy_link_flow_results")
        taz = _build_table_name(cfg, "road_community")
        link_ids_param = extra.get("link_ids")
        bbox_param = extra.get("bbox")
        return fn(
            on_progress,
            cfg_with_types,
            slow,
            flow,
            taz,
            out_table,
            link_ids=link_ids_param,
            bbox=bbox_param,
        )

    raise ValueError(f"未知 module: {module_key}")


# ---------------- 结果聚合（写到统一 KV 响应文件） ----------------

def _store_simple(payload: dict, key: str, value) -> None:
    if value is None or isinstance(value, (dict, list)):
        return
    payload[key] = str(value)


def _aggregate_indicator(payload: dict, indicator_code: str, result_str: str, logs: list) -> tuple[int, str]:
    """解析单个 fs_run_* 返回的 JSON。返回 (status: 0失败/1成功/2数据缺失跳过, message)。"""
    try:
        parsed = json.loads(result_str) if result_str else {}
    except Exception as exc:
        logs.append(f"[{indicator_code}] 解析返回 JSON 失败：{exc}")
        return 0, f"解析返回 JSON 失败：{exc}"

    status = int(parsed.get("status", 0) or 0)
    message = str(parsed.get("message", "") or "")
    data = parsed.get("data") if isinstance(parsed.get("data"), dict) else {}
    summary = parsed.get("summary") if isinstance(parsed.get("summary"), dict) else {}

    tables = data.get("tables") if isinstance(data.get("tables"), dict) else {}
    counts = data.get("counts") if isinstance(data.get("counts"), dict) else {}
    metrics = data.get("metrics") if isinstance(data.get("metrics"), dict) else {}
    for k, v in tables.items():
        _store_simple(payload, f"data.table.{indicator_code}_{k}", v)
    for k, v in counts.items():
        if isinstance(v, bool):
            payload[f"data.count.{indicator_code}_{k}"] = str(int(v))
        elif isinstance(v, int):
            payload[f"data.count.{indicator_code}_{k}"] = str(v)
        elif isinstance(v, float):
            payload[f"data.metric.{indicator_code}_{k}"] = str(v)
    for k, v in metrics.items():
        if isinstance(v, bool):
            payload[f"data.metric.{indicator_code}_{k}"] = str(int(v))
        elif isinstance(v, str):
            payload[f"data.metric.{indicator_code}_{k}"] = v
        elif isinstance(v, (int, float)):
            payload[f"data.metric.{indicator_code}_{k}"] = str(v)

    skipped = status == 2 or bool(counts.get("skipped")) or bool(metrics.get("skipped"))
    skip_reason = metrics.get("skip_reason") or message or ""
    missing_tables = metrics.get("missing_tables") or ""
    if skipped:
        payload[f"summary.attr.{indicator_code}_skipped"] = "1"
        payload[f"summary.attr.{indicator_code}_data_missing"] = "1"
        if skip_reason:
            payload[f"summary.attr.{indicator_code}_skip_reason"] = str(skip_reason)
        if missing_tables:
            payload[f"summary.attr.{indicator_code}_missing_tables"] = str(missing_tables)
        if status != 2:
            status = 2

    inner_logs = summary.get("logs") if isinstance(summary.get("logs"), list) else []
    for item in inner_logs:
        logs.append(f"[{indicator_code}] {item}")

    payload[f"summary.attr.{indicator_code}_status"] = str(status)
    if message:
        payload[f"summary.attr.{indicator_code}_message"] = message

    return status, message


def _run_pt_aon_bridge(module_key: str, args, req: dict) -> int:
    """Run the AON-aware pt diagnosis branch without touching road/slow logic."""
    bridge_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "diagnosis_bridge_pt_aon.py",
    )
    spec = importlib.util.spec_from_file_location("diagnosis_bridge_pt_aon", bridge_path)
    if spec is None or spec.loader is None:
        _write_response_file(
            args.response_file,
            -99,
            "诊断服务内部错误：无法加载公交 AON 诊断桥接模块",
            {"summary.stage": f"diagnosis_{module_key}"},
        )
        return 1

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    response = module.run(module_key, req)
    code = int(response.pop("code", -99))
    message = str(response.pop("message", "公交诊断执行完成"))
    _write_response_file(args.response_file, code, message, response)
    _write_progress_file(args.progress_file, 100, f"{module_key} done")
    return 0


# ---------------- 主入口 ----------------

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", required=True, choices=list(MODULE_INDICATORS.keys()))
    parser.add_argument("--request-file", required=True)
    parser.add_argument("--response-file", required=True)
    parser.add_argument("--progress-file", default=None)
    parser.add_argument("--repo-root", default=None)
    args = parser.parse_args()

    module_key = args.module
    indicator_specs = MODULE_INDICATORS[module_key]

    payload: dict = {}
    logs: list = []
    overall_code = 1
    overall_message = "Success"

    try:
        req = _read_request_file(args.request_file)
        project_id = int(req.get("project_id", "0") or 0)
        user_id = int(req.get("user_id", "0") or 0)
        case_id = int(req.get("case_id", "0") or 0)
        param1 = req.get("param1", "")
        param2 = req.get("param2", "")

        if project_id <= 0 or user_id <= 0:
            _write_response_file(
                args.response_file,
                -1,
                "入参错误：project_id 与 user_id 须为正整数",
                {},
            )
            return 0

        if module_key in ("pt_macro", "pt_meso"):
            return _run_pt_aon_bridge(module_key, args, req)

        diagnosis_root = _resolve_diagnosis_root(args.repo_root)
        entry_module = _load_entry_module(diagnosis_root)

        # 解析 param2
        extra: dict = {}
        if param2 and param2.strip():
            try:
                parsed = json.loads(param2)
                if isinstance(parsed, dict):
                    extra = parsed
            except Exception as exc:
                logs.append(f"param2 JSON 解析失败，忽略：{exc}")

        requested_codes = extra.get("indicator_codes") if isinstance(extra.get("indicator_codes"), list) else None
        target_link_id = extra.get("target_link_id")

        all_specs = list(indicator_specs)
        if requested_codes:
            wanted = set(str(c) for c in requested_codes)
            specs_to_run = [(fn, code) for fn, code in all_specs if code in wanted]
            unknown = sorted(wanted.difference(code for _, code in all_specs))
            if unknown:
                logs.append(f"忽略未知 indicator_codes: {unknown}")
        else:
            specs_to_run = all_specs

        if not specs_to_run:
            _write_response_file(
                args.response_file,
                -1,
                f"{_module_label(module_key)}入参错误：indicator_codes 中没有可识别的指标编码",
                {},
            )
            return 0

        cfg, schema, prefix = _build_cfg(param1, project_id, user_id, case_id)

        result_table_full = f'{schema}.{prefix}diagnosis_indicator_result_rows'
        payload["data.table.result_table"] = result_table_full
        payload["summary.stage"] = f"diagnosis_{module_key}"
        payload[f"summary.attr.module"] = module_key
        payload[f"summary.attr.schema"] = schema
        payload[f"summary.attr.scenario_prefix"] = prefix
        payload[f"summary.attr.indicator_codes"] = ",".join(code for _, code in specs_to_run)

        n_total = len(specs_to_run)
        n_computed = 0
        n_fail = 0
        n_skipped = 0
        failures: list = []
        skipped_codes: list = []
        missing_tables_all: list = []

        for idx, (fs_name, indicator_code) in enumerate(specs_to_run):
            base_pct = int(idx / n_total * 100)
            next_pct = int((idx + 1) / n_total * 100)

            def on_progress(step, percent, _base=base_pct, _next=next_pct, _code=indicator_code):
                try:
                    p = float(percent)
                except Exception:
                    p = 0.0
                p = max(0.0, min(100.0, p))
                mapped = int(_base + (_next - _base) * p / 100.0)
                _write_progress_file(args.progress_file, mapped, f"{_code} step {step}")

            try:
                _write_progress_file(args.progress_file, base_pct, f"{indicator_code} start")
                result_str = _call_indicator(entry_module, module_key, fs_name, cfg, target_link_id, on_progress, extra)
                ind_status, message = _aggregate_indicator(payload, indicator_code, result_str, logs)
                if ind_status == 1:
                    n_computed += 1
                elif ind_status == 2:
                    n_skipped += 1
                    skipped_codes.append(indicator_code)
                    if message:
                        payload[f"summary.attr.{indicator_code}_message"] = _humanize_diagnosis_error(
                            message, module_key, extra
                        )
                    mt = payload.get(f"summary.attr.{indicator_code}_missing_tables", "")
                    if mt:
                        for part in str(mt).split(","):
                            part = part.strip()
                            if part and part not in missing_tables_all:
                                missing_tables_all.append(part)
                else:
                    n_fail += 1
                    hint = _humanize_diagnosis_error(message, module_key, extra)
                    failures.append(f"{indicator_code}:{hint}")
                    payload[f"summary.attr.{indicator_code}_message"] = hint
                _write_progress_file(args.progress_file, next_pct, f"{indicator_code} done")
            except Exception as exc:
                n_fail += 1
                hint = _humanize_diagnosis_error(f"{exc.__class__.__name__}: {exc}", module_key, extra)
                failures.append(f"{indicator_code}:{hint}")
                logs.append(f"[{indicator_code}] 异常：{hint}")
                payload[f"summary.attr.{indicator_code}_status"] = "0"
                payload[f"summary.attr.{indicator_code}_message"] = hint

        payload["data.count.indicator_total"] = str(n_total)
        payload["data.count.indicator_computed"] = str(n_computed)
        payload["data.count.indicator_ok"] = str(n_computed + n_skipped)
        payload["data.count.indicator_failed"] = str(n_fail)
        payload["data.count.indicator_skipped"] = str(n_skipped)
        if failures:
            payload["summary.attr.failures"] = "; ".join(failures)
        if skipped_codes:
            payload["summary.attr.skipped_indicators"] = ",".join(skipped_codes)
            payload["summary.attr.data_missing"] = "1"
        if missing_tables_all:
            payload["summary.attr.missing_tables"] = ",".join(missing_tables_all)

        label = _module_label(module_key)
        reason = _summarize_failure_reasons(failures, module_key, extra)

        if n_computed == 0 and n_skipped == 0:
            overall_code = -99
            overall_message = f"{label}失败：全部 {n_total} 个指标未算出"
            if reason:
                overall_message += f"。原因：{reason}"
        elif n_fail > 0:
            overall_code = 1
            overall_message = (
                f"{label}部分成功：已算出 {n_computed} 项，跳过 {n_skipped} 项，失败 {n_fail} 项"
            )
            if reason:
                overall_message += f"。失败原因：{reason}"
        elif n_skipped > 0:
            overall_code = 2
            miss = ",".join(missing_tables_all[:3])
            overall_message = (
                f"{label}部分完成：已算出 {n_computed}/{n_total} 项，"
                f"{n_skipped} 项因输入数据缺失已跳过"
            )
            if miss:
                overall_message += f"（缺表：{miss}）"
            overall_message += "；可先运行交通分配或补全输入表后重试"
        else:
            overall_code = 1
            overall_message = f"{label}成功：{n_computed}/{n_total} 项指标已写入结果表"

        for i, item in enumerate(logs):
            payload[f"summary.log.{i}"] = item

        _write_response_file(args.response_file, overall_code, overall_message, payload)
        return 0
    except Exception as exc:
        for i, item in enumerate(logs):
            payload[f"summary.log.{i}"] = item
        payload[f"summary.log.{len(logs)}"] = f"diagnosis bridge fatal: {exc.__class__.__name__}: {exc}"
        fatal = _humanize_diagnosis_error(f"{exc.__class__.__name__}: {exc}", module_key, {})
        _write_response_file(args.response_file, -99, f"诊断服务内部错误：{fatal}", payload)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
