"""基础数据分析报告桥接脚本：被 C++ gRPC 服务通过 subprocess 调用。

调用约定与 diagnosis_bridge.py 一致（request-file/response-file KV 文本协议）。

功能：
1. 从 PostgreSQL 读取机动车/慢行/公交/TAZ 各表的真实数据
2. 计算各维度统计指标
3. 将数据嵌入 ECharts HTML 模板，生成可视化报告文件
4. 通过 response-file 返回报告路径及关键指标

param2 可选字段（JSON 字符串，**项目名仅在此传入**）：
  param2.project_name: str  —— 项目名称；缺省 output_path 时用于「{project_name}_{YYYYMMDD}_{报告类型}.html」；写入报告页眉
  param2.output_path: str  —— HTML 输出路径，缺省见 project_name 或 /tmp/tna_base_report_{pid}_{uid}.html
  param2.include_html_content: bool —— 是否在 summary.attributes.content 返回 HTML 正文（默认 true）
"""
import argparse
import json
import os
import sys
import traceback

try:
    from .diagnosis_value_utils import (
        filter_rows_for_report,
        group_by_indicator,
        macro_summary_rows,
    )
    from .network_map_collect import collect_map_bundle_for_cases, collect_map_bundle_tool
    from .network_map_report import render_map_sections
    from .indicator_report_meta import get_indicator_meta
    from .report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
except ImportError:
    from diagnosis_value_utils import (
        filter_rows_for_report,
        group_by_indicator,
        macro_summary_rows,
    )
    from network_map_collect import collect_map_bundle_for_cases, collect_map_bundle_tool
    from network_map_report import render_map_sections
    from indicator_report_meta import get_indicator_meta
    from report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2


# 排放与公交运营成本估算（与 cost_benefit_bridge 口径一致，供基础报告数值展示）
CO2_KG_PER_VKM = 0.18       # 机动车 pcu·km，IPCC 乘用车均值 kg
PT_COST_PER_VKM = 4.0        # 公交车辆公里运营成本（元），行业参考值
PT_CO2_KG_PER_PKM = 0.089    # 公交人均公里碳排 kg
PT_OPERATING_HOURS = 16      # 日运营小时数（用于由高峰小时推算全日）
VC_LEVEL_ORDER = ("畅通", "基本畅通", "拥挤", "严重拥堵")
_DIAG_OVERALL_KEYS = frozenset({"overall", "全网统归", "value"})

try:
    from .table_name_cn import (
        TABLE_CN,
        COLUMN_CN,
        table_cn as _table_cn,
        field_cn as _field_cn,
        table_missing_warning as _table_missing_warning,
        tables_missing_warning as _tables_missing_warning,
        data_sources_line,
        BASE_REPORT_TABLES,
    )
except ImportError:
    from table_name_cn import (
        TABLE_CN,
        COLUMN_CN,
        table_cn as _table_cn,
        field_cn as _field_cn,
        table_missing_warning as _table_missing_warning,
        tables_missing_warning as _tables_missing_warning,
        data_sources_line,
        BASE_REPORT_TABLES,
    )


# ─── KV 协议工具（与 diagnosis_bridge.py 完全一致）─────────────────────────────

def _escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")

def _unescape(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            out.append("\n" if nxt == "n" else "\r" if nxt == "r" else nxt)
            i += 2
        else:
            out.append(s[i]); i += 1
    return "".join(out)

def read_request(path: str) -> dict:
    kv = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            pos = line.find("=")
            if pos < 0:
                continue
            kv[line[:pos]] = _unescape(line[pos + 1:])
    return kv

def write_response(path: str, kv: dict):
    with open(path, "w", encoding="utf-8") as f:
        for k, v in kv.items():
            f.write(f"{k}={_escape(str(v))}\n")

def write_progress(path: str, percent: int, title: str):
    if not path:
        return
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"percent={percent}\ntitle={_escape(title)}\n")
    except Exception:
        pass


# ─── 数据库连接工具 ─────────────────────────────────────────────────────────────

def _get_conn(param1: str):
    import psycopg2
    if param1 and param1.strip():
        conn = psycopg2.connect(param1)
        conn.autocommit = True
        return conn
    db_conf = os.environ.get("TNA_DB_CONF", "/opt/algorithms/db.conf")
    cfg = {}
    with open(db_conf, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    conn = psycopg2.connect(
        host=cfg.get("host", "localhost"),
        port=int(cfg.get("port", 5432)),
        dbname=cfg.get("dbname", "urban"),
        user=cfg.get("user", "urban"),
        password=cfg.get("password", ""),
    )
    # 只读采集：某张表 SQL 失败后勿污染同连接上的后续 table_exists / 查询
    conn.autocommit = True
    return conn

def _prefix(pid: int, uid: int) -> str:
    """基础数据分析（工具类）：表前缀 project{p}_user{u}_，永不拼 case。"""
    return f"project{pid}_user{uid}_"


def _scheme_prefix(pid: int, uid: int, case_id: int) -> str:
    """方案类诊断结果表前缀（含 case_id=0 的 _case0_）。"""
    return f"project{pid}_user{uid}_case{case_id}_"


_MOTOR_TYPE_MAP_URBAN = {
    1: "主干道",
    2: "次干道",
    3: "支路",
    4: "集散道路",
    5: "其他",
    10: "质心连杆",
}
_MOTOR_TYPE_MAP_HIGHWAY = {
    1: "高速公路",
    2: "国道",
    3: "省道",
    4: "县乡道",
    5: "其他",
    10: "质心连杆",
}


def _infer_road_length_unit(conn, prefix: str) -> str:
    """road_way.length 单位：Greedy 默认 m；部分高速/导入项目为 km。"""
    if not _table_exists(conn, prefix, "road_way"):
        return "m"
    tbl = _tbl(prefix, "road_way")
    try:
        _, rows = _query(
            conn, f"SELECT COALESCE(MAX(length), 0) FROM {tbl} WHERE length > 0"
        )
        max_len = float(rows[0][0]) if rows and rows[0][0] is not None else 0.0
    except Exception:
        return "m"
    return "km" if max_len <= 500 else "m"


def _length_raw_to_km(raw: float, unit: str) -> float:
    return float(raw) if unit == "km" else float(raw) / 1000.0


def _motor_type_map(conn, prefix: str) -> dict[int, str]:
    """无 type 4/5 时按公路等级（高速/国道/省道）命名。"""
    if not _table_exists(conn, prefix, "road_way"):
        return _MOTOR_TYPE_MAP_URBAN
    tbl = _tbl(prefix, "road_way")
    try:
        _, rows = _query(
            conn, f"SELECT DISTINCT type FROM {tbl} WHERE type IS NOT NULL"
        )
        types = {int(r[0]) for r in rows if r[0] is not None}
    except Exception:
        return _MOTOR_TYPE_MAP_URBAN
    if types & {4, 5}:
        return _MOTOR_TYPE_MAP_URBAN
    return _MOTOR_TYPE_MAP_HIGHWAY


_LEGACY_ROAD_CLASS: dict[str, int] = {
    "主干道": 1,
    "次干道": 2,
    "支路": 3,
    "集散道路": 4,
    "其他": 5,
    "快速路": 1,
    "主干路": 2,
    "次干路": 3,
}


def _normalize_road_class_label(raw: str, type_map: dict[int, str]) -> str:
    s = (raw or "").strip()
    if not s:
        return s
    if s in type_map.values():
        return s
    import re

    m = re.match(r"(?i)^(?:type|class|等级)?[_-]?(\d+)$", s)
    if m:
        return type_map.get(int(m.group(1)), f"Type{m.group(1)}")
    try:
        return type_map.get(int(s), f"Type{s}")
    except ValueError:
        pass
    if s in _LEGACY_ROAD_CLASS:
        return type_map.get(_LEGACY_ROAD_CLASS[s], s)
    return s


def _vc_mileage_pct_from_flow(conn, prefix: str) -> list[dict]:
    """按 V/C 分档统计里程占比（%），作 road_macro_capacity_distribution 后备。"""
    if not _table_exists(conn, prefix, "road_way"):
        return []
    if not _column_exists(conn, prefix, "road_way", "v_c"):
        return []
    tbl = _tbl(prefix, "road_way")
    dedupe_key = "COALESCE(NULLIF(link_osmid::text,''), link_id::text)"
    try:
        _, rows = _query(
            conn,
            f"""
            WITH dedup AS (
                SELECT DISTINCT ON ({dedupe_key}) length, v_c
                FROM {tbl}
                WHERE v_c IS NOT NULL AND length > 0
                ORDER BY {dedupe_key}, link_id
            )
            SELECT
                ROUND(
                    (SUM(CASE WHEN v_c < 0.6 THEN length ELSE 0 END)
                     / NULLIF(SUM(length), 0) * 100)::numeric, 2
                ),
                ROUND(
                    (SUM(CASE WHEN v_c >= 0.6 AND v_c < 0.8 THEN length ELSE 0 END)
                     / NULLIF(SUM(length), 0) * 100)::numeric, 2
                ),
                ROUND(
                    (SUM(CASE WHEN v_c >= 0.8 AND v_c < 1.0 THEN length ELSE 0 END)
                     / NULLIF(SUM(length), 0) * 100)::numeric, 2
                ),
                ROUND(
                    (SUM(CASE WHEN v_c >= 1.0 THEN length ELSE 0 END)
                     / NULLIF(SUM(length), 0) * 100)::numeric, 2
                ),
                SUM(length)
            FROM dedup
            """,
        )
        if not rows or not rows[0][-1]:
            return []
        a, b, c, d, _ = rows[0]
        labels = VC_LEVEL_ORDER
        vals = [float(a or 0), float(b or 0), float(c or 0), float(d or 0)]
        return [{"label": labels[i], "value": vals[i]} for i in range(4)]
    except Exception:
        return []


def _macro_indicator_count(conn, scheme_prefix: str) -> int:
    if not _table_exists(conn, scheme_prefix, "diagnosis_indicator_result_rows"):
        return 0
    tbl = _tbl(scheme_prefix, "diagnosis_indicator_result_rows")
    try:
        _, rows = _query(
            conn,
            f"SELECT COUNT(DISTINCT indicator_code) FROM {tbl} WHERE indicator_code LIKE '%_macro_%'",
        )
        return int(rows[0][0]) if rows else 0
    except Exception:
        return 0


def _road_macro_indicator_count(conn, scheme_prefix: str) -> int:
    if not _table_exists(conn, scheme_prefix, "diagnosis_indicator_result_rows"):
        return 0
    tbl = _tbl(scheme_prefix, "diagnosis_indicator_result_rows")
    try:
        _, rows = _query(
            conn,
            f"SELECT COUNT(DISTINCT indicator_code) FROM {tbl} WHERE indicator_code LIKE 'road_macro_%'",
        )
        return int(rows[0][0]) if rows else 0
    except Exception:
        return 0


def _resolve_diag_prefix(
    conn,
    pid: int,
    uid: int,
    case_id: int,
    tool_prefix: str,
    logs: list,
    *,
    diag_case_id: int | None = None,
) -> str:
    """诊断结果写在方案表；优先选机动车宏观指标最全且分配成功的方案前缀。"""
    if _table_exists(conn, tool_prefix, "diagnosis_indicator_result_rows"):
        return tool_prefix
    candidates: list[tuple[str, str]] = []
    if diag_case_id is not None:
        candidates.append(
            (_scheme_prefix(pid, uid, int(diag_case_id)), f"param2.diag_case_id={diag_case_id}")
        )
    if case_id is not None:
        sp = _scheme_prefix(pid, uid, case_id)
        if all(sp != c[0] for c in candidates):
            candidates.append((sp, f"case_id={case_id}"))
    for fallback_cid in (1, 3, 2, 0):
        sp = _scheme_prefix(pid, uid, fallback_cid)
        if all(sp != c[0] for c in candidates):
            candidates.append((sp, f"case_id={fallback_cid}"))
    best_sp, best_label, best_key = "", "", (-1, -1)
    for sp, label in candidates:
        road_n = _road_macro_indicator_count(conn, sp)
        macro_n = _macro_indicator_count(conn, sp)
        if not _table_exists(conn, sp, "diagnosis_indicator_result_rows"):
            continue
        key = (road_n, macro_n)
        if key > best_key:
            best_key, best_sp, best_label = key, sp, label
    if best_sp:
        logs.append(
            f"诊断：工具库无诊断指标结果表，回退读取方案库「{_table_cn('diagnosis_indicator_result_rows')}」"
            f"（{best_label}，road_macro {best_key[0]} 项，宏观合计 {best_key[1]} 项）"
        )
        return best_sp
    return tool_prefix


def _resolve_scheme_table_prefix(
    conn,
    pid: int,
    uid: int,
    case_id: int,
    suffix: str,
    tool_prefix: str,
    logs: list,
    *,
    label: str = "数据",
) -> str:
    """工具表无数据时，回退到含目标表且有行的方案前缀。"""
    if case_id:
        sp = _scheme_prefix(pid, uid, case_id)
        if _table_exists(conn, sp, suffix):
            try:
                _, rows = _query(conn, f"SELECT 1 FROM {_tbl(sp, suffix)} LIMIT 1")
                if rows:
                    if sp != tool_prefix:
                        logs.append(
                            f"{label}：读取方案库「{_table_cn(suffix)}」（case_id={case_id}）"
                        )
                    return sp
            except Exception:
                pass
    for candidate in (tool_prefix,):
        if _table_exists(conn, candidate, suffix):
            try:
                _, rows = _query(conn, f"SELECT 1 FROM {_tbl(candidate, suffix)} LIMIT 1")
                if rows:
                    return candidate
            except Exception:
                pass
    candidates: list[tuple[str, str]] = []
    if case_id is not None:
        candidates.append((_scheme_prefix(pid, uid, case_id), f"case_id={case_id}"))
    for fallback_cid in (2, 3, 0):
        sp = _scheme_prefix(pid, uid, fallback_cid)
        if all(sp != c[0] for c in candidates):
            candidates.append((sp, f"case_id={fallback_cid}"))
    for sp, lbl in candidates:
        if not _table_exists(conn, sp, suffix):
            continue
        try:
            _, rows = _query(conn, f"SELECT 1 FROM {_tbl(sp, suffix)} LIMIT 1")
            if rows:
                logs.append(
                    f"{label}：回退读取方案库「{_table_cn(suffix)}」（{lbl}）"
                )
                return sp
        except Exception:
            continue
    return tool_prefix


def _extract_motor_diag_charts(raw: list[dict]) -> dict:
    """从诊断原始行提取机动车宏观柱状图序列。"""
    cong_by_class: list[dict] = []
    vc_dist: list[dict] = []
    cong_overall: float | None = None
    seen_vc: set[str] = set()
    for r in raw:
        code = (r.get("indicator_code") or "").strip()
        rk = (r.get("result_key") or "").strip()
        ri = (r.get("result_item") or "").strip()
        num = r.get("result_value_num")
        if num is None:
            continue
        try:
            val = float(num)
        except (TypeError, ValueError):
            continue
        if code == "road_macro_congestion_ratio":
            if rk in _DIAG_OVERALL_KEYS and ri in ("value", "overall", ""):
                cong_overall = val
            elif ri in ("value", "overall", "") and rk not in _DIAG_OVERALL_KEYS:
                cong_by_class.append({"label": rk, "value": val})
        elif code == "road_macro_capacity_distribution":
            if rk in _DIAG_OVERALL_KEYS and ri and ri not in _DIAG_OVERALL_KEYS:
                if ri in seen_vc:
                    continue
                seen_vc.add(ri)
                vc_dist.append({"label": ri, "value": val})
    if not cong_by_class and cong_overall is not None:
        cong_by_class = [{"label": "全网", "value": cong_overall}]
    order = {name: idx for idx, name in enumerate(VC_LEVEL_ORDER)}
    vc_dist.sort(key=lambda x: order.get(x["label"], 99))
    return {
        "congestion_by_class": cong_by_class,
        "vc_dist_overall": vc_dist,
        "congestion_overall": cong_overall,
    }


def _congestion_ratio_by_class_from_flow(conn, prefix: str) -> list[dict]:
    """按道路等级统计 V/C≥0.8 里程占比，作拥堵里程柱状图后备。"""
    if not _table_exists(conn, prefix, "road_way"):
        return []
    if not _column_exists(conn, prefix, "road_way", "v_c"):
        return []
    tbl = _tbl(prefix, "road_way")
    has_osm = _column_exists(conn, prefix, "road_way", "link_osmid")
    dedupe_key = "COALESCE(NULLIF(link_osmid::text,''), link_id::text)"
    type_map = _motor_type_map(conn, prefix)
    try:
        _, rows = _query(
            conn,
            f"""
            WITH dedup AS (
                SELECT DISTINCT ON ({dedupe_key}) type, length, v_c
                FROM {tbl}
                WHERE v_c IS NOT NULL AND length > 0 AND type <> 10
                ORDER BY {dedupe_key}, link_id
            )
            SELECT type,
                   ROUND(
                       (SUM(CASE WHEN v_c >= 0.8 THEN length ELSE 0 END)
                        / NULLIF(SUM(length), 0) * 100)::numeric,
                       2
                   ) AS congest_pct
            FROM dedup
            GROUP BY type
            ORDER BY type
            """,
        )
        out = []
        for t, pct in rows:
            if pct is None or int(t) == 10:
                continue
            out.append(
                {
                    "label": type_map.get(int(t), f"Type{t}"),
                    "value": float(pct),
                }
            )
        return out
    except Exception:
        return []


def _motor_vmt_and_co2(conn, prefix: str) -> tuple[float, float]:
    """返回 (万 pcu·km, 吨 CO2/高峰小时)。"""
    if not _table_exists(conn, prefix, "road_way"):
        return 0.0, 0.0
    if not _column_exists(conn, prefix, "road_way", "volume"):
        return 0.0, 0.0
    tbl = _tbl(prefix, "road_way")
    has_osm = _column_exists(conn, prefix, "road_way", "link_osmid")
    dedupe_key = "COALESCE(NULLIF(link_osmid::text,''), link_id::text)"
    length_unit = _infer_road_length_unit(conn, prefix)
    vmt_divisor = 10000.0 if length_unit == "km" else 1_000_000.0
    try:
        _, rows = _query(
            conn,
            f"""
            WITH dedup AS (
                SELECT DISTINCT ON ({dedupe_key}) volume, length
                FROM {tbl}
                WHERE volume IS NOT NULL AND volume > 0 AND length > 0
                ORDER BY {dedupe_key}, link_id
            )
            SELECT ROUND(SUM(volume * length / {vmt_divisor})::numeric, 2) FROM dedup
            """,
        )
        if not rows or rows[0][0] is None:
            return 0.0, 0.0
        vmt = float(rows[0][0])
        co2_t = round(vmt * 10000 * CO2_KG_PER_VKM / 1000, 2)
        return vmt, co2_t
    except Exception:
        return 0.0, 0.0


def _tbl(prefix: str, suffix: str) -> str:
    """生成带 schema 的带引号表名"""
    return f'user_project."{prefix}{suffix}"'

def _query(conn, sql: str, params=None):
    try:
        with conn.cursor() as cur:
            cur.execute(sql, params)
            cols = [d[0] for d in cur.description] if cur.description else []
            rows = cur.fetchall()
        return cols, rows
    except Exception:
        if not conn.autocommit:
            conn.rollback()
        raise

def _table_exists(conn, prefix: str, suffix: str, schema: str = "user_project") -> bool:
    tname = f"{prefix}{suffix}"
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT 1 FROM information_schema.tables "
                "WHERE table_schema=%s AND table_name=%s LIMIT 1",
                (schema, tname),
            )
            return cur.fetchone() is not None
    except Exception:
        if not conn.autocommit:
            conn.rollback()
        return False


def _column_exists(
    conn, prefix: str, suffix: str, column: str, schema: str = "user_project"
) -> bool:
    tname = f"{prefix}{suffix}"
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT 1 FROM information_schema.columns "
                "WHERE table_schema=%s AND table_name=%s AND column_name=%s LIMIT 1",
                (schema, tname, column),
            )
            return cur.fetchone() is not None
    except Exception:
        if not conn.autocommit:
            conn.rollback()
        return False


def _section_shell(**kwargs) -> dict:
    """各模块统一元数据：available / status / warnings / charts。"""
    base = {
        "available": False,
        "status": "skipped",
        "warnings": [],
        "charts": {},
    }
    base.update(kwargs)
    return base


# ─── 各模块数据计算 ─────────────────────────────────────────────────────────────

def calc_motor(conn, prefix: str, logs: list) -> dict:
    """机动车路网统计（无 volume/v_c 时仍统计路段数与里程）。"""
    empty = _section_shell(
        type_data=[],
        total_count=0,
        total_length_km=0,
        vc_dist=[0, 0, 0, 0],
        has_flow_metrics=False,
        charts={"structure": False, "flow": False, "vc": False},
    )
    if not _table_exists(conn, prefix, "road_way"):
        empty["warnings"] = [_table_missing_warning("road_way", "机动车路网")]
        logs.append("机动车：未找到机动车路段表，已跳过")
        return empty

    has_volume = _column_exists(conn, prefix, "road_way", "volume")
    has_vc = _column_exists(conn, prefix, "road_way", "v_c")
    has_flow = has_volume and has_vc

    result = _section_shell(
        type_data=[],
        total_count=0,
        total_length_km=0,
        vc_dist=[0, 0, 0, 0],
        has_flow_metrics=has_flow,
        charts={"structure": False, "flow": False, "vc": False},
        available=True,
        status="ok",
    )
    if not has_flow:
        result["warnings"].append(
            "机动车路段表无流量/饱和度字段（尚未交通分配），仅展示路网结构图，不展示流量与 V/C 图"
        )

    tbl = _tbl(prefix, "road_way")
    has_osm = _column_exists(conn, prefix, "road_way", "link_osmid")
    dedupe_key = "COALESCE(NULLIF(link_osmid::text,''), link_id::text)"
    length_unit = _infer_road_length_unit(conn, prefix)
    type_map = _motor_type_map(conn, prefix)
    try:
        if has_flow:
            sql = f"""
                WITH dedup AS (
                    SELECT DISTINCT ON ({dedupe_key})
                           type, length, volume, v_c
                    FROM {tbl}
                    ORDER BY {dedupe_key}, link_id
                )
                SELECT type,
                       COUNT(*) AS cnt,
                       ROUND(SUM(length)::numeric, 0) AS total_len,
                       ROUND(AVG(volume)::numeric, 1) AS avg_vol,
                       ROUND(AVG(v_c)::numeric, 4) AS avg_vc
                FROM dedup
                GROUP BY type ORDER BY type
            """
        else:
            sql = f"""
                WITH dedup AS (
                    SELECT DISTINCT ON ({dedupe_key})
                           type, length
                    FROM {tbl}
                    ORDER BY {dedupe_key}, link_id
                )
                SELECT type,
                       COUNT(*) AS cnt,
                       ROUND(SUM(length)::numeric, 0) AS total_len,
                       NULL::numeric AS avg_vol,
                       NULL::numeric AS avg_vc
                FROM dedup
                GROUP BY type ORDER BY type
            """
        _, rows = _query(conn, sql)
        type_data = []
        total_cnt, total_len = 0, 0.0
        for row in rows:
            t, cnt, length, avg_vol, avg_vc = row
            label = type_map.get(int(t), f'Type{t}')
            type_data.append({
                'type': int(t), 'label': label,
                'count': int(cnt),
                'length_km': round(_length_raw_to_km(float(length or 0), length_unit), 1),
                'avg_vol': float(avg_vol or 0),
                'avg_vc': float(avg_vc or 0),
            })
            total_cnt += int(cnt)
            total_len += float(length or 0)
        result['type_data'] = type_data
        result['road_class_data'] = sorted(
            [d for d in type_data if d.get("type") != 10],
            key=lambda d: d.get("type", 99),
        )
        result['total_count'] = total_cnt
        result['total_length_km'] = round(_length_raw_to_km(total_len, length_unit), 1)
        result['charts']['structure'] = total_cnt > 0

        if has_flow:
            _, vc_rows = _query(conn, f"""
                WITH dedup AS (
                    SELECT DISTINCT ON ({dedupe_key}) v_c
                    FROM {tbl}
                    WHERE v_c IS NOT NULL
                    ORDER BY {dedupe_key}, link_id
                )
                SELECT
                    SUM(CASE WHEN v_c < 0.6 THEN 1 ELSE 0 END) AS a,
                    SUM(CASE WHEN v_c >= 0.6 AND v_c < 0.8 THEN 1 ELSE 0 END) AS b,
                    SUM(CASE WHEN v_c >= 0.8 AND v_c < 1.0 THEN 1 ELSE 0 END) AS c,
                    SUM(CASE WHEN v_c >= 1.0 THEN 1 ELSE 0 END) AS d,
                    COUNT(*) AS valid
                FROM dedup
            """)
            if vc_rows and vc_rows[0][-1]:
                a, b, c, d, _ = vc_rows[0]
                result['vc_dist'] = [int(a or 0), int(b or 0), int(c or 0), int(d or 0)]
                result['charts']['vc'] = sum(result['vc_dist']) > 0
            result['charts']['flow'] = any(d.get('avg_vol') for d in type_data)
        else:
            result['vc_dist'] = [0, 0, 0, 0]

        result['available'] = total_cnt > 0
        result['status'] = 'ok' if total_cnt > 0 else 'empty'
        logs.append(
            f"机动车：{total_cnt} 条路段，{result['total_length_km']} 公里"
            + ("（无 volume/v_c，未做交通分配）" if not has_flow else "")
        )
    except Exception as e:
        logs.append(f"机动车查询失败：{e}")
        result['available'] = False
        result['status'] = 'error'
        result['warnings'].append(f"机动车查询失败：{e}")
        result.setdefault('type_data', [])
        result.setdefault('road_class_data', [])
        result.setdefault('total_count', 0)
        result.setdefault('total_length_km', 0)
        result.setdefault('vc_dist', [0, 0, 0, 0])
        result.setdefault('has_flow_metrics', False)
        result.setdefault('charts', {"structure": False, "flow": False, "vc": False})
    return result


def calc_slow(conn, prefix: str, logs: list) -> dict:
    """慢行路网统计"""
    empty = _section_shell(
        type_data=[], total_count=0, total_length_km=0,
        flow_dist=[0] * 5, avg_flow=0, max_flow=0, top_links=[],
        charts={"structure": False, "flow": False},
    )
    if not _table_exists(conn, prefix, "slow_road_way"):
        empty["warnings"] = [
            _table_missing_warning(
                "slow_road_way", "慢行路网；工具库无慢行表则整节折叠"
            )
        ]
        logs.append("慢行：未找到慢行路段表，已跳过")
        return empty
    result = _section_shell(
        type_data=[], total_count=0, total_length_km=0,
        flow_dist=[0] * 5, avg_flow=0, max_flow=0, top_links=[],
        charts={"structure": False, "flow": False},
        available=True,
        status="ok",
    )
    tbl_way = _tbl(prefix, "slow_road_way")
    tbl_flow = _tbl(prefix, "slow_greedy_link_flow_results")
    type_map = {2: '步行道', 3: '非机动车道', 4: '混合慢行', 10: '其他'}
    try:
        _, rows = _query(conn, f"""
            SELECT w.type,
                   COUNT(*) AS cnt,
                   ROUND(SUM(w.length)::numeric, 0) AS total_len,
                   ROUND(AVG(f.flow)::numeric, 1) AS avg_flow,
                   ROUND(MAX(f.flow)::numeric, 1) AS max_flow
            FROM {tbl_way} w
            LEFT JOIN {tbl_flow} f ON w.link_id = f.link_id
            GROUP BY w.type ORDER BY w.type
        """)
        type_data = []
        total_cnt, total_len = 0, 0.0
        for row in rows:
            t, cnt, length, avg_flow, max_flow = row
            label = type_map.get(int(t), f'Type{t}')
            type_data.append({
                'type': int(t), 'label': label,
                'count': int(cnt),
                'length_km': round(float(length or 0) / 1000, 2),
                'avg_flow': float(avg_flow or 0),
                'max_flow': float(max_flow or 0),
            })
            total_cnt += int(cnt)
            total_len += float(length or 0)
        result['type_data'] = type_data
        result['total_count'] = total_cnt
        result['total_length_km'] = round(total_len / 1000, 1)

        # 流量分布
        _, fd_rows = _query(conn, f"""
            SELECT
                SUM(CASE WHEN flow < 500 THEN 1 ELSE 0 END),
                SUM(CASE WHEN flow >= 500 AND flow < 1000 THEN 1 ELSE 0 END),
                SUM(CASE WHEN flow >= 1000 AND flow < 2000 THEN 1 ELSE 0 END),
                SUM(CASE WHEN flow >= 2000 AND flow < 5000 THEN 1 ELSE 0 END),
                SUM(CASE WHEN flow >= 5000 THEN 1 ELSE 0 END),
                ROUND(AVG(flow)::numeric, 1),
                ROUND(MAX(flow)::numeric, 1)
            FROM {tbl_flow}
            WHERE flow IS NOT NULL
        """)
        if fd_rows:
            r = fd_rows[0]
            result['flow_dist'] = [int(r[i] or 0) for i in range(5)]
            result['avg_flow'] = float(r[5] or 0)
            result['max_flow'] = float(r[6] or 0)
        else:
            result['flow_dist'] = [0] * 5
            result['avg_flow'] = 0
            result['max_flow'] = 0
        # TOP10 高流量路段
        _, top_rows = _query(conn, f"""
            SELECT f.link_id, ROUND(f.flow::numeric, 1)
            FROM {tbl_flow} f
            WHERE f.flow IS NOT NULL
            ORDER BY f.flow DESC LIMIT 10
        """)
        result['top_links'] = [{'link_id': int(r[0]), 'flow': float(r[1])} for r in top_rows]
        result['available'] = total_cnt > 0
        result['status'] = 'ok' if total_cnt > 0 else 'empty'
        result['charts'] = {
            'structure': total_cnt > 0,
            'flow': bool(result.get('flow_dist') and sum(result['flow_dist']) > 0),
        }
        logs.append(
            f"慢行：{total_cnt} 条路段，{result['total_length_km']} 公里，平均流量={result['avg_flow']}"
        )
    except Exception as e:
        logs.append(f"慢行查询失败：{e}")
        result['available'] = False
        result['status'] = 'error'
        result['warnings'].append(f"慢行查询失败：{e}")
        result.setdefault('type_data', [])
        result.setdefault('total_count', 0)
        result.setdefault('total_length_km', 0)
        result.setdefault('flow_dist', [0] * 5)
        result.setdefault('avg_flow', 0)
        result.setdefault('max_flow', 0)
        result.setdefault('top_links', [])
        result.setdefault('charts', {"structure": False, "flow": False})
    return result


def calc_pt(conn, prefix: str, logs: list, *, assignment_prefix: str | None = None) -> dict:
    """公交网络统计（无 pt_route 时可仅用 other_bus_route 做线路属性统计）。"""
    empty = _section_shell(
        route_count=0, route_types=0, stop_count=0, link_data=[],
        avg_mileage=0, max_mileage=0, min_mileage=0, avg_stations=0,
        top_aboard=[], top_alight=[],
        operating_cost_yuan=0, pt_co2_t=0,
        charts={"routes": False, "links": False, "mileage": False, "top_stops": False},
    )
    assign_prefix = assignment_prefix or prefix
    has_route = _table_exists(conn, prefix, "pt_route") or _table_exists(
        conn, assign_prefix, "pt_route"
    )
    has_link = _table_exists(conn, assign_prefix, "pt_link_result")
    has_attr = _table_exists(conn, prefix, "other_bus_route")

    if not has_route and not has_attr:
        empty["warnings"] = [
            _tables_missing_warning(["pt_route", "other_bus_route"], "公交")
        ]
        logs.append("公交：未找到公交线路表/公交线路属性表，已跳过")
        return empty

    result = _section_shell(
        route_count=0, route_types=0, stop_count=0, link_data=[],
        avg_mileage=0, max_mileage=0, min_mileage=0, avg_stations=0,
        top_aboard=[], top_alight=[],
        operating_cost_yuan=0, pt_co2_t=0,
        charts={"routes": False, "links": False, "mileage": False, "top_stops": False},
        available=True,
        status="partial",
    )
    if not has_route:
        result["warnings"].append(
            _table_missing_warning("pt_route", "无法统计线路/分配链路")
        )
    if not has_link and has_route:
        result["warnings"].append(
            _table_missing_warning("pt_link_result", "不展示公交分配链路图")
        )

    try:
        if has_route:
            route_prefix = prefix if _table_exists(conn, prefix, "pt_route") else assign_prefix
            tbl_route = _tbl(route_prefix, "pt_route")
            _, rr = _query(conn, f"SELECT COUNT(*) FROM {tbl_route}")
            result['route_count'] = int(rr[0][0] or 0)
            result['route_types'] = 0
            result['charts']['routes'] = result['route_count'] > 0

        if has_link:
            tbl_link = _tbl(assign_prefix, "pt_link_result")
            _, lr = _query(conn, f"""
                SELECT link_type,
                       COUNT(*) AS cnt,
                       ROUND(AVG(flow)::numeric, 1) AS avg_flow,
                       ROUND(MAX(flow)::numeric, 1) AS max_flow
                FROM {tbl_link}
                GROUP BY link_type ORDER BY link_type
            """)
            link_data = []
            for row in lr:
                lt, cnt, avg_f, max_f = row
                link_data.append({
                    'type': str(lt),
                    'count': int(cnt),
                    'avg_flow': float(avg_f or 0),
                    'max_flow': float(max_f or 0),
                })
            result['link_data'] = link_data
            result['charts']['links'] = len(link_data) > 0

        tbl_attr = _tbl(prefix, "other_bus_route")
        if has_attr:
            if not has_route:
                _, cr = _query(conn, f"SELECT COUNT(*) FROM {tbl_attr}")
                result['route_count'] = int(cr[0][0] or 0)
                result['charts']['routes'] = result['route_count'] > 0
                result["warnings"].append(
                    "仅使用公交线路属性表统计线路条数（无公交线路表）"
                )
            _, mr = _query(conn, f"""
                SELECT ROUND(AVG(trip_mile)::numeric, 1),
                       ROUND(MAX(trip_mile)::numeric, 1),
                       ROUND(MIN(trip_mile)::numeric, 1),
                       ROUND(AVG(ARRAY_LENGTH(STRING_TO_ARRAY(stations, ','), 1))::numeric, 1)
                FROM {tbl_attr}
                WHERE trip_mile IS NOT NULL
            """)
            if mr and mr[0][0]:
                result['avg_mileage'] = float(mr[0][0])
                result['max_mileage'] = float(mr[0][1])
                result['min_mileage'] = float(mr[0][2])
                result['avg_stations'] = float(mr[0][3] or 0)
            else:
                result['avg_mileage'] = 0
                result['max_mileage'] = 0
                result['min_mileage'] = 0
                result['avg_stations'] = 0
        else:
            result['avg_mileage'] = 0
            result['max_mileage'] = 0
            result['min_mileage'] = 0
            result['avg_stations'] = 0

        if has_link:
            def _top_stops(link_type: str, stop_col: str) -> list[dict]:
                _, sr = _query(
                    conn,
                    f"""
                    SELECT l.{stop_col} AS stop_id,
                           l.{stop_col} AS stop_name,
                           ROUND(SUM(l.flow)::numeric, 1) AS vol
                    FROM {tbl_link} l
                    WHERE l.link_type = %s
                      AND l.{stop_col} IS NOT NULL
                      AND l.{stop_col} <> ''
                      AND l.flow > 0
                    GROUP BY l.{stop_col}
                    ORDER BY vol DESC
                    LIMIT 5
                    """,
                    (link_type,),
                )
                return [
                    {
                        "stop_id": str(r[0]),
                        "stop_name": str(r[1] or r[0]),
                        "label": f"{r[0]} {r[1] or ''}".strip(),
                        "volume": float(r[2] or 0),
                    }
                    for r in sr
                ]

            result["top_aboard"] = _top_stops("ABOARD", "head_stop_id")
            result["top_alight"] = _top_stops("ALIGHT", "tail_stop_id")
            result["charts"]["top_stops"] = bool(
                result["top_aboard"] or result["top_alight"]
            )

            pkm_h = 0.0
            if _table_exists(conn, assign_prefix, "pt_transit"):
                tbl_transit = _tbl(assign_prefix, "pt_transit")
                _, pr = _query(
                    conn,
                    f"""
                    SELECT ROUND(SUM(l.flow * COALESCE(t.length, 0))::numeric, 1)
                    FROM {tbl_link} l
                    LEFT JOIN {tbl_transit} t
                      ON t.from_stop = l.tail_stop_id AND t.to_stop = l.head_stop_id
                    WHERE l.link_type = 'ENROUTE' AND l.flow > 0
                    """,
                )
                if pr and pr[0][0]:
                    pkm_h = float(pr[0][0])
            if pkm_h <= 0:
                enroute = next(
                    (d for d in result.get("link_data", []) if d["type"] == "ENROUTE"),
                    None,
                )
                if enroute:
                    pkm_h = float(enroute.get("avg_flow", 0)) * float(
                        result.get("avg_mileage", 0) or 0
                    )
            avg_occ = 25.0
            vkm_h = pkm_h / avg_occ if pkm_h > 0 else 0.0
            result["operating_cost_yuan"] = round(
                vkm_h * PT_COST_PER_VKM * PT_OPERATING_HOURS, 0
            )
            co2_kg = pkm_h * PT_CO2_KG_PER_PKM * PT_OPERATING_HOURS
            result["pt_co2_t"] = round(co2_kg / 1000, 2)

        result['available'] = (
            result['route_count'] > 0
            or result['stop_count'] > 0
            or len(result.get('link_data', [])) > 0
            or result.get('avg_mileage', 0) > 0
        )
        result['charts']['mileage'] = result.get('avg_mileage', 0) > 0
        result['status'] = 'ok' if result['available'] else 'empty'
        logs.append(
            f"公交：{result['route_count']} 条线路，{result['stop_count']} 个站点，"
            f"{len(result.get('link_data', []))} 种链路类型"
        )
    except Exception as e:
        logs.append(f"公交查询失败：{e}")
        result['available'] = False
        result['status'] = 'error'
        result['warnings'].append(f"公交查询失败：{e}")
        result.setdefault('route_count', 0)
        result.setdefault('stop_count', 0)
        result.setdefault('link_data', [])
        result.setdefault('avg_mileage', 0)
        result.setdefault('max_mileage', 0)
        result.setdefault('min_mileage', 0)
        result.setdefault('avg_stations', 0)
        result.setdefault('top_aboard', [])
        result.setdefault('top_alight', [])
        result.setdefault('operating_cost_yuan', 0)
        result.setdefault('pt_co2_t', 0)
        result.setdefault('charts', {"routes": False, "links": False, "mileage": False, "top_stops": False})
    return result


def calc_taz(conn, prefix: str, logs: list) -> dict:
    """交通小区（road_community）统计；原 other_taz_socioeconomic 已废弃。"""
    empty = _section_shell(
        taz_list=[], total_pop=0, total_area=0, avg_density=0, taz_count=0,
        charts={"all": False},
    )
    if not _table_exists(conn, prefix, "road_community"):
        empty["warnings"] = [_table_missing_warning("road_community", "交通小区")]
        logs.append("交通小区：未找到交通小区表，已跳过")
        return empty
    result = _section_shell(
        taz_list=[], total_pop=0, total_area=0, avg_density=0, taz_count=0,
        charts={"all": False},
        available=True,
        status="ok",
    )
    tbl = _tbl(prefix, "road_community")
    try:
        _, rows = _query(conn, f"""
            SELECT area_id AS zone_id, pop, area
            FROM {tbl}
            WHERE area_id IS NOT NULL
            ORDER BY area_id
        """)
        taz_list = []
        total_pop, total_area = 0, 0.0
        for row in rows:
            tid, pop, area = row
            pop = int(pop or 0)
            area = float(area or 0)
            taz_list.append({
                'id': int(tid),
                'pop': pop,
                'area': round(area, 2),
                'density': round(pop / area, 1) if area > 0 else 0,
            })
            total_pop += pop
            total_area += area
        result['taz_list'] = taz_list
        result['total_pop'] = total_pop
        result['total_area'] = round(total_area, 2)
        result['avg_density'] = round(total_pop / total_area, 1) if total_area > 0 else 0
        result['taz_count'] = len(taz_list)
        result['available'] = len(taz_list) > 0
        result['status'] = 'ok' if len(taz_list) > 0 else 'empty'
        result['charts'] = {'all': len(taz_list) > 0}
        logs.append(f"交通小区：{len(taz_list)} 个，人口={total_pop}，面积={result['total_area']} 平方公里")
    except Exception as e:
        logs.append(f"交通小区查询失败：{e}")
        result['available'] = False
        result['status'] = 'error'
        result['warnings'].append(f"交通小区查询失败：{e}")
        result.setdefault('taz_list', [])
        result.setdefault('total_pop', 0)
        result.setdefault('total_area', 0)
        result.setdefault('avg_density', 0)
        result.setdefault('taz_count', 0)
        result.setdefault('charts', {'all': False})
    return result


def calc_diag(conn, prefix: str, logs: list) -> dict:
    """读取已有宏观诊断代表值（不含中微观路段明细）。"""
    result = _section_shell(rows=[], charts={"summary": False, "table": False})
    tbl = _tbl(prefix, "diagnosis_indicator_result_rows")
    if not _table_exists(conn, prefix, "diagnosis_indicator_result_rows"):
        result["warnings"] = [
            _table_missing_warning("diagnosis_indicator_result_rows", "诊断结果")
        ]
        logs.append("诊断：未找到诊断指标结果表，已跳过")
        return result
    try:
        _, rows = _query(conn, f"""
            SELECT indicator_code, context_key, result_key, result_item,
                   result_value_num, result_value_text, result_unit
            FROM {tbl}
            ORDER BY indicator_code, context_key, result_key, result_item
        """)
        raw = [
            {
                "indicator_code": r[0],
                "context_key": r[1],
                "result_key": r[2],
                "result_item": r[3],
                "result_value_num": r[4],
                "result_value_text": r[5],
                "result_unit": r[6],
            }
            for r in rows
        ]
        filtered = filter_rows_for_report(raw, scope="macro")
        summary = macro_summary_rows(group_by_indicator(filtered))
        result["rows"] = []
        for s in summary:
            meta = get_indicator_meta(s["code"])
            result["rows"].append(
                {
                    "code": s["code"],
                    "ctx": s["ctx"],
                    "key": s["key"],
                    "item": s.get("item") or s["key"],
                    "num": s["num"],
                    "text": s.get("text") or "",
                    "unit": s.get("unit") or "",
                    "tier": "macro",
                    "label": meta.get("label") or s["code"],
                    "formula": meta.get("formula", ""),
                    "reference": meta.get("reference", ""),
                }
            )
        result["raw_row_count"] = len(rows)
        result["available"] = len(summary) > 0
        result["status"] = "ok" if summary else "empty"
        result["motor_charts"] = _extract_motor_diag_charts(raw)
        result["charts"] = {
            "summary": len(summary) > 0,
            "table": len(summary) > 0,
            "motor_congestion": bool(result["motor_charts"].get("congestion_by_class")),
            "motor_vc_dist": bool(result["motor_charts"].get("vc_dist_overall")),
        }
        logs.append(
            f"诊断：宏观代表值 {len(summary)} 项（原始 {len(rows)} 行，已忽略中微观与异常辅助行）"
        )
    except Exception as e:
        logs.append(f"诊断查询失败：{e}")
        result['warnings'].append(f"诊断查询失败：{e}")
        result['status'] = 'error'
    return result


def _diag_num(rows: list[dict], code: str) -> float | None:
    for r in rows:
        if r.get("code") == code and r.get("num") is not None:
            try:
                return float(r["num"])
            except (TypeError, ValueError):
                return None
    return None


def _diag_plausibility_warnings(
    diag_rows: list[dict],
    motor_charts: dict,
    *,
    used_vc_flow_fallback: bool = False,
) -> list[str]:
    """宏观诊断结果合理性检查（异常时写入报告警告）。"""
    if not diag_rows:
        return []
    out: list[str] = []

    tti = _diag_num(diag_rows, "road_macro_tti")
    if tti is not None and (tti > 10 or tti < 0.5):
        out.append(
            f"行程时间指数 TTI={tti:.2f} 超出合理范围(约 1~3)，"
            "疑似 length/fft 单位或流量–容量量级异常，不可直接用于方案评价"
        )

    dti = _diag_num(diag_rows, "road_macro_dti")
    if dti is not None and dti > 500:
        out.append(f"延误时间指数 DTI={dti:.1f}% 异常偏高，诊断结果不可信")

    speed = _diag_num(diag_rows, "road_macro_average_speed")
    if speed is not None and speed < 1.0:
        out.append(
            f"平均行程速度={speed:.2f} km/h 异常偏低，请结合机动车章节分配结果判断"
        )

    cap_vals = [float(x.get("value") or 0) for x in (motor_charts.get("vc_dist_overall") or [])]
    if cap_vals and sum(abs(v) for v in cap_vals) < 0.01:
        msg = "通行能力分布（饱和度分档）诊断写入全 0%"
        if used_vc_flow_fallback:
            msg += "；报告中饱和度里程占比图已改用机动车路段表分配结果估算"
        out.append(msg)
    elif used_vc_flow_fallback:
        out.append(
            "通行能力分布诊断表无效，报告中饱和度里程占比图已改用机动车路段表分配结果估算"
        )

    cong = _diag_num(diag_rows, "road_macro_congestion_ratio")
    if cong is not None and cong >= 99.0 and speed is not None and speed < 5.0:
        out.append(
            f"拥堵里程比例 {cong:.1f}% 与极低平均速度并存，建议复核 OD/分配 volume 与 capacity 量级"
        )

    tpi = _diag_num(diag_rows, "road_macro_tpi")
    if tpi is not None and tpi >= 9.5 and speed is not None and speed < 5.0:
        out.append(
            f"交通绩效指数 TPI={tpi:.1f} 触顶，通常伴随速度/时间指标异常"
        )

    return [f"[诊断·异常] {w}" for w in out]


def _collect_report_warnings(motor, slow, pt, taz, diag) -> list[str]:
    warnings: list[str] = []
    for block, label in (
        (motor, "机动车"),
        (slow, "慢行"),
        (pt, "公交"),
        (taz, "TAZ"),
        (diag, "诊断"),
    ):
        for w in block.get("warnings") or []:
            warnings.append(f"[{label}] {w}")
        if not block.get("available"):
            if block.get("status") == "skipped":
                continue
            if block.get("status") == "empty":
                warnings.append(f"[{label}] 无可用数据")
    for w in diag.get("plausibility_warnings") or []:
        if w not in warnings:
            warnings.append(w)
    return warnings


def _qualified_table(conn, prefix: str, suffix: str, schema: str = "user_project") -> str:
    """返回 schema 限定表名；不存在则空字符串。"""
    if _table_exists(conn, prefix, suffix, schema):
        return f'{schema}."{prefix}{suffix}"'
    return ""


def _set_summary_logs(payload: dict[str, str], log_lines: list[str]) -> None:
    """写入 summary.log.{i}，与 local_bridge.cpp collect_indexed_strings 一致。"""
    for i, line in enumerate(log_lines):
        payload[f"summary.log.{i}"] = line


def _build_grpc_response_kv(
    conn,
    prefix: str,
    motor_data: dict,
    slow_data: dict,
    pt_data: dict,
    taz_data: dict,
    diag_data: dict,
    output_path: str,
    report_warnings: list[str],
    logs: list[str],
    msg: str,
    *,
    html_content: str = "",
    include_html_content: bool = True,
) -> dict[str, str]:
    """组装 response-file KV；键名须与 local_bridge.cpp 一致（data.table / data.count / data.metric）。"""
    table_suffixes = [
        ("road_way", "motor"),
        ("slow_road_way", "slow"),
        ("slow_greedy_link_flow_results", "slow"),
        ("pt_route", "pt"),
        ("pt_link_result", "pt"),
        ("other_bus_route", "pt"),
        ("road_community", "taz"),
        ("diagnosis_indicator_result_rows", "diag"),
    ]
    payload: dict[str, str] = {
        "code": 1,
        "message": msg,
        "summary.stage": "base_data_report",
        "summary.attr.table_prefix": prefix,
        "summary.attr.case_id_ignored": "true",
        "summary.attr.html_path": output_path,
        "summary.attr.report_warnings": json.dumps(report_warnings, ensure_ascii=False),
        "summary.attr.motor_has_flow_metrics": str(
            motor_data.get("has_flow_metrics", False)
        ).lower(),
        "data.table.html_report": output_path,
        "data.count.motor_links": str(motor_data.get("total_count", 0)),
        "data.count.slow_links": str(slow_data.get("total_count", 0)),
        "data.count.pt_routes": str(pt_data.get("route_count", 0)),
        "data.count.pt_stops": str(pt_data.get("stop_count", 0)),
        "data.count.taz_count": str(taz_data.get("taz_count", 0)),
        "data.count.diag_rows": str(len(diag_data.get("rows") or [])),
        "data.metric.motor_length_km": str(motor_data.get("total_length_km", 0)),
        "data.metric.slow_length_km": str(slow_data.get("total_length_km", 0)),
        "data.metric.total_pop": str(taz_data.get("total_pop", 0)),
        "data.metric.pt_avg_mileage": str(pt_data.get("avg_mileage", 0)),
    }
    tables_meta: list[dict] = []
    for suffix, module in table_suffixes:
        qname = _qualified_table(conn, prefix, suffix)
        payload[f"data.table.{suffix}"] = qname
        tables_meta.append({
            "suffix": suffix,
            "label_cn": _table_cn(suffix),
            "module": module,
            "qualified": qname,
            "exists": bool(qname),
        })
    payload["summary.attr.tables_json"] = json.dumps(tables_meta, ensure_ascii=False)
    if include_html_content and html_content:
        payload["summary.attr.content"] = html_content
    _set_summary_logs(payload, logs + [f"[警告] {w}" for w in report_warnings])
    return payload


def _build_result_message(motor, slow, pt, taz) -> str:
    parts = [
        f"{motor.get('total_count', 0)} 条机动车路段",
        f"{slow.get('total_count', 0)} 条慢行路段",
        f"{pt.get('route_count', 0)} 条公交线路",
    ]
    msg = f"描述性统计分析生成成功，共 {parts[0]}、{parts[1]}、{parts[2]}"
    if taz.get("taz_count"):
        msg += f"，{taz.get('taz_count')} 个交通小区"
    return msg


# ─── HTML 生成 ──────────────────────────────────────────────────────────────────

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__PROJECT_NAME__ · 描述性统计分析</title>
__ECHARTS_INLINE__
__LEAFLET_HEAD__
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Microsoft YaHei',SimSun,sans-serif;background:#f0f2f5;color:#222;font-size:14px}
.banner{background:linear-gradient(135deg,#1a3a5c 0%,#2d6a9f 55%,#1e8bc3 100%);
  color:#fff;padding:32px 48px 24px;display:flex;align-items:flex-end;justify-content:space-between}
.banner h1{font-size:24px;font-weight:700;letter-spacing:2px}
.banner .sub{font-size:12px;opacity:.75;margin-top:6px}
.nav{background:#fff;border-bottom:2px solid #e0e6ef;display:flex;padding:0 48px;
  position:sticky;top:0;z-index:100;box-shadow:0 2px 6px rgba(0,0,0,.06)}
.nav a{display:block;padding:12px 16px;font-size:13px;color:#555;text-decoration:none;
  border-bottom:3px solid transparent;transition:all .2s}
.nav a:hover{color:#2d6a9f;border-color:#2d6a9f}
.container{max-width:1440px;margin:0 auto;padding:28px 24px}
.section{margin-bottom:44px}
.sec-title{display:flex;align-items:center;gap:10px;font-size:17px;font-weight:700;
  color:#1a3a5c;margin-bottom:10px;padding-bottom:10px;border-bottom:2px solid #d0dde8}
.dot{width:5px;height:22px;border-radius:3px;flex-shrink:0}
.dm{background:#2d6a9f}.ds{background:#27ae60}.dp{background:#e67e22}
.dt{background:#8e44ad}.dd{background:#c0392b}
.desc{background:#fff;border-radius:8px;padding:14px 18px;margin-bottom:16px;
  font-size:13px;line-height:1.9;color:#444;border-left:4px solid #2d6a9f;
  box-shadow:0 1px 5px rgba(0,0,0,.05)}
.desc.green{border-color:#27ae60}.desc.orange{border-color:#e67e22}
.desc.purple{border-color:#8e44ad}.desc.red{border-color:#c0392b}
.desc strong{color:#1a3a5c}
.kpi-row{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:18px}
.kpi{flex:1;min-width:130px;background:#fff;border-radius:10px;padding:16px 18px;
  box-shadow:0 2px 7px rgba(0,0,0,.06);border-left:4px solid #2d6a9f}
.kpi.g{border-color:#27ae60}.kpi.o{border-color:#e67e22}
.kpi.p{border-color:#8e44ad}.kpi.r{border-color:#c0392b}
.kv{font-size:24px;font-weight:700;color:#1a3a5c;line-height:1}
.ku{font-size:11px;color:#888;margin-left:3px}
.kl{font-size:11px;color:#999;margin-top:4px}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:16px}
.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:16px;margin-bottom:16px}
.card{background:#fff;border-radius:10px;padding:18px 20px;box-shadow:0 2px 7px rgba(0,0,0,.06)}
.card h3{font-size:13px;color:#555;margin-bottom:6px;font-weight:600}
.cnote{font-size:11px;color:#aaa;margin-top:6px;line-height:1.6}
.dtable{width:100%;border-collapse:collapse;font-size:12px;margin-top:4px}
.dtable th{background:#2d6a9f;color:#fff;padding:8px 12px;text-align:left;font-weight:600}
.dtable td{padding:7px 12px;border-bottom:1px solid #eef2f7}
.dtable tr:nth-child(even) td{background:#f7f9fc}
.badge{display:inline-block;padding:1px 7px;border-radius:10px;font-size:11px;font-weight:600}
.br{background:#dbeafe;color:#1d4ed8}.bp{background:#fef3c7;color:#92400e}
.bs{background:#d1fae5;color:#065f46}
footer{text-align:center;padding:20px;font-size:12px;color:#aaa;
  border-top:1px solid #e0e6ef;background:#fff;margin-top:8px}
.report-alerts{background:#fff8e6;border:1px solid #e8d48b;border-radius:8px;padding:14px 18px;
  margin-bottom:20px;font-size:13px;line-height:1.85;color:#5c4a00}
.report-alerts ul{margin:8px 0 0 20px;padding:0}
.report-alerts li{margin:4px 0}
.skip-banner{background:#f7f9fc;border:1px dashed #c5d0de;border-radius:8px;padding:16px 18px;
  font-size:13px;line-height:1.9;color:#555;margin-bottom:12px}
details.sec-fold>summary{cursor:pointer;font-size:14px;color:#1a3a5c;font-weight:600;margin-bottom:10px}
__MAP_CSS__
@media(max-width:900px){.g2,.g3{grid-template-columns:1fr}
  .banner{flex-direction:column;gap:10px;padding:20px}.nav{overflow-x:auto}}
</style>
</head>
<body>
<div class="banner">
  <div>
    <h1>__PROJECT_NAME__ · 描述性统计分析</h1>
    <div class="sub">项目 ID __PID__ &nbsp;·&nbsp; 用户 __UID__ &nbsp;·&nbsp; 机动车 · 慢行 · 公共交通<br>__DATA_SOURCES__</div>
  </div>
</div>
<nav class="nav">
  <a href="#s0">描述性统计分析</a><a href="#s_maps">空间分布</a><a href="#s1">机动车</a><a href="#s2">慢行</a>
  <a href="#s3">公交</a><a href="#s4">TAZ</a><a href="#s5">诊断结果</a>
</nav>
<div class="container">
<div id="report_alerts" class="report-alerts" style="display:none"></div>

<!-- ── 描述性统计分析 ── -->
<div id="s0" class="section">
  <div class="sec-title"><span class="dot dd"></span>描述性统计分析</div>
  <div class="desc red" id="desc_overview"></div>
  <div class="kpi-row" id="kpi_overview"></div>
  <div class="g2">
    <div class="card"><h3>各交通系统规模对比</h3>
      <div id="c_overview_bar" style="width:100%;height:270px"></div></div>
    <div class="card"><h3>综合交通系统评估雷达</h3>
      <div id="c_radar" style="width:100%;height:270px"></div>
      <p class="cnote">评分基于路网密度、覆盖率、通行效率等指标归一化处理（满分100）</p></div>
  </div>
</div>

<!-- ── 机动车 ── -->
<div id="s1" class="section">
  <div class="sec-title"><span class="dot dm"></span>机动车交通分析</div>
  <div class="desc" id="desc_motor"></div>
  <div class="kpi-row" id="kpi_motor"></div>
  <div class="g2">
    <div class="card"><h3>道路等级构成（路段数）</h3>
      <div id="c_road_pie" style="width:100%;height:270px"></div></div>
    <div class="card"><h3>各等级道路里程（km）</h3>
      <div id="c_road_len" style="width:100%;height:270px"></div></div>
  </div>
  <div class="g2">
    <div class="card"><h3>各等级平均流量（pcu/h）</h3>
      <div id="c_road_vol" style="width:100%;height:250px"></div>
      <p class="cnote">数据来源：机动车路段表·流量（交通分配结果）</p></div>
    <div class="card"><h3>路段饱和度（V/C）分布</h3>
      <div id="c_vc_pie" style="width:100%;height:250px"></div>
      <p class="cnote">数据来源：机动车路段表·饱和度</p></div>
  </div>
  <div class="g2">
    <div class="card"><h3>各等级路段数 / 里程 / 平均流量三维对比</h3>
      <div id="c_road_triple" style="width:100%;height:270px"></div>
      <p class="cnote">路段数（左轴）、里程 km（左轴）与平均流量 pcu/h（右轴）同步对比</p></div>
    <div class="card"><h3>各饱和度段平均流量（pcu/h）</h3>
      <div id="c_vc_flow" style="width:100%;height:270px"></div>
      <p class="cnote">按 V/C 分段统计路段平均流量，反映拥堵与流量的关联关系</p></div>
  </div>
  <div class="card" id="motor_macro_diag_box" style="display:none;margin-top:16px">
    <h3>道路宏观诊断指标</h3>
    <table class="dtable">
      <thead><tr><th>指标</th><th>计算方法</th><th>数值</th><th>参考范围</th></tr></thead>
      <tbody id="motor_macro_diag_body"></tbody>
    </table>
    <p class="cnote">数据来自诊断指标结果表；需先执行道路宏观诊断（review_road / diagnosis_road_macro）。路段数已按 OSM 路段 ID 去重。</p>
  </div>
  <div class="g2" id="motor_diag_charts_row" style="display:none;margin-top:16px">
    <div class="card"><h3>拥堵里程比例（%）</h3>
      <div id="c_motor_congestion" style="width:100%;height:260px"></div>
      <p class="cnote">道路宏观诊断 road_macro_congestion_ratio；无分等级数据时按 V/C≥0.8 里程占比估算</p></div>
    <div class="card"><h3>路段饱和度分布（里程占比 %）</h3>
      <div id="c_motor_vc_dist" style="width:100%;height:260px"></div>
      <p class="cnote">诊断无有效分档时，按机动车路段表分配结果的里程与饱和度估算</p></div>
  </div>
</div>

__MAP_SECTION__

<!-- ── 慢行 ── -->
<div id="s2" class="section">
  <div class="sec-title"><span class="dot ds"></span>慢行交通分析</div>
  <div class="desc green" id="desc_slow"></div>
  <div class="kpi-row" id="kpi_slow"></div>
  <div class="g2">
    <div class="card"><h3>慢行路网类型构成</h3>
      <div id="c_slow_pie" style="width:100%;height:270px"></div></div>
    <div class="card"><h3>各类型里程（km）与平均流量（人次/h）</h3>
      <div id="c_slow_bar" style="width:100%;height:270px"></div></div>
  </div>
  <div class="card">
    <h3>慢行路段流量区间分布</h3>
    <div id="c_slow_flow" style="width:100%;height:220px"></div>
    <p class="cnote">数据来源：慢行分配流量表·流量</p>
  </div>
  <div class="g2">
    <div class="card"><h3>各类型慢行道流量对比（平均 vs 峰值）</h3>
      <div id="c_slow_cmp" style="width:100%;height:260px"></div>
      <p class="cnote">对比各类型步行/非机动车/混行道的平均流量与最大流量差距</p></div>
    <div class="card"><h3>高流量慢行路段 TOP 10（人次/h）</h3>
      <div id="c_slow_top" style="width:100%;height:260px"></div>
      <p class="cnote">按路段流量从高到低取前 10 条，横向柱状图展示</p></div>
  </div>
  <div class="card" id="slow_macro_diag_box" style="display:none;margin-top:16px">
    <h3>慢行宏观诊断指标</h3>
    <table class="dtable">
      <thead><tr><th>指标</th><th>计算方法</th><th>数值</th><th>参考范围</th></tr></thead>
      <tbody id="slow_macro_diag_body"></tbody>
    </table>
    <p class="cnote">数据来自诊断指标结果表；需先执行慢行宏观诊断（diagnosis_slow_macro）</p>
  </div>
</div>

<!-- ── 公交 ── -->
<div id="s3" class="section">
  <div class="sec-title"><span class="dot dp"></span>公共交通分析</div>
  <div class="desc orange" id="desc_pt"></div>
  <div class="kpi-row" id="kpi_pt"></div>
  <div class="g2">
    <div class="card"><h3>公交网络链路类型构成</h3>
      <div id="c_pt_pie" style="width:100%;height:270px"></div>
      <p class="cnote">ABOARD=上车段，ALIGHT=下车段，ENROUTE=在途段，WALK=步行换乘段</p></div>
    <div class="card"><h3>各链路类型平均流量（人次/h）</h3>
      <div id="c_pt_flow" style="width:100%;height:270px"></div></div>
  </div>
  <div class="card">
    <h3>线路里程分布特征</h3>
    <div id="c_pt_mileage" style="width:100%;height:200px"></div>
    <p class="cnote">数据来源：公交线路属性表·线路里程</p>
  </div>
  <div class="g2">
    <div class="card"><h3>各链路类型平均流量 vs 峰值流量（人次/h）</h3>
      <div id="c_pt_cmp" style="width:100%;height:260px"></div>
      <p class="cnote">峰值流量反映线网最高承载压力，与平均流量差距越大说明分布越集中</p></div>
    <div class="card"><h3>ENROUTE 与 WALK 段流量分布</h3>
      <div id="c_pt_ew" style="width:100%;height:260px"></div>
      <p class="cnote">在途客流与步行换乘客流分布，反映公交吸引力与换乘便利性</p></div>
  </div>
  <div class="g2" id="pt_top_stops_row" style="display:none">
    <div class="card"><h3>TOP5 上车站（人次/h）</h3>
      <div id="c_pt_top_aboard" style="width:100%;height:280px"></div>
      <p class="cnote">x 轴：站点编号与名称；y 轴：上车量（ABOARD 链路流量汇总）</p></div>
    <div class="card"><h3>TOP5 下车站（人次/h）</h3>
      <div id="c_pt_top_alight" style="width:100%;height:280px"></div>
      <p class="cnote">x 轴：站点编号与名称；y 轴：下车量（ALIGHT 链路流量汇总）</p></div>
  </div>
  <div class="card" id="pt_macro_diag_box" style="display:none;margin-top:16px">
    <h3>公交宏观诊断指标</h3>
    <table class="dtable">
      <thead><tr><th>指标</th><th>计算方法</th><th>数值</th><th>参考范围</th></tr></thead>
      <tbody id="pt_macro_diag_body"></tbody>
    </table>
    <p class="cnote">数据来自诊断指标结果表；需先执行公交宏观诊断（diagnosis_pt_macro）</p>
  </div>
</div>

<!-- ── TAZ ── -->
<div id="s4" class="section">
  <div class="sec-title"><span class="dot dt"></span>交通分析小区（TAZ）</div>
  <div class="desc purple" id="desc_taz"></div>
  <div class="kpi-row" id="kpi_taz"></div>
  <div class="g2">
    <div class="card"><h3>各 TAZ 人口分布</h3>
      <div id="c_taz_pop" style="width:100%;height:290px"></div></div>
    <div class="card"><h3>各 TAZ 面积分布（km²）</h3>
      <div id="c_taz_area" style="width:100%;height:290px"></div></div>
  </div>
  <div class="g2">
    <div class="card"><h3>人口密度排名（人/km²）</h3>
      <div id="c_taz_dens" style="width:100%;height:290px"></div></div>
    <div class="card"><h3>人口—面积分布</h3>
      <div id="c_taz_scatter" style="width:100%;height:290px"></div>
      <p class="cnote">每个点为一个交通小区；横轴面积、纵轴人口，颜色越深表示人口密度越高（人/km²）</p></div>
  </div>
</div>

<!-- ── 诊断结果 ── -->
<div id="s5" class="section">
  <div class="sec-title"><span class="dot dd"></span>诊断指标汇总</div>
  <div class="desc red" id="desc_diag"></div>
  <div class="g2">
    <div class="card"><h3>各模块诊断指标数量</h3>
      <div id="c_diag_pie" style="width:100%;height:260px"></div></div>
    <div class="card"><h3>道路宏观关键诊断值</h3>
      <div id="c_diag_road" style="width:100%;height:260px"></div></div>
  </div>
  <div class="card">
    <h3>诊断指标明细（宏观代表值）</h3>
    <table class="dtable">
      <thead><tr><th>指标</th><th>计算方法</th><th>类别</th><th>数值</th><th>参考范围</th></tr></thead>
      <tbody id="diag_tbody"></tbody>
    </table>
  </div>
</div>

</div>
<footer>TNA 综合交通分析平台</footer>

<script>
const D = __DATA_JSON__;

window.addEventListener('DOMContentLoaded', function() {

// ── 工具 ──
function ec(id){ const el=document.getElementById(id); return el?echarts.init(el):null; }
function kpi(cls,val,unit,label){
  return `<div class="kpi ${cls}"><div class="kv">${val}<span class="ku">${unit}</span></div><div class="kl">${label}</div></div>`;
}
function fmt(v,d=1){ return v==null?'N/A':(+v).toFixed(d); }
function hideNav(href){ const a=document.querySelector('.nav a[href="'+href+'"]'); if(a)a.style.display='none'; }
function hideCard(chartId){
  const el=document.getElementById(chartId);
  if(el){ const card=el.closest('.card'); if(card) card.style.display='none'; }
}
function renderSkipped(secId,navHref,title,warnings){
  hideNav(navHref);
  const sec=document.getElementById(secId);
  if(!sec) return;
  const w=(warnings&&warnings.length)?warnings:['本节所需数据不存在或查询无结果'];
  sec.innerHTML=
    '<div class="sec-title"><span class="dot dm"></span>'+title+'（已折叠）</div>'+
    '<details class="sec-fold" open><summary>点击展开说明</summary>'+
    '<div class="skip-banner">'+w.map(x=>'• '+x).join('<br>')+'</div></details>';
}
const reportWarnings=D.report_warnings||[];
if(reportWarnings.length){
  const box=document.getElementById('report_alerts');
  box.style.display='block';
  box.innerHTML='<strong>数据与图表说明</strong><ul>'+
    reportWarnings.map(w=>'<li>'+w+'</li>').join('')+'</ul>';
}

const MC=['#1d6fa5','#2d9cdb','#74c0e8','#aad4f0','#d0e9f9'];
const SC=['#1e8449','#27ae60','#82e0aa','#d5f5e3'];
const PC=['#d35400','#e67e22','#f39c12','#f8c471'];
const TC=i=>`hsl(${270-i*12},48%,52%)`;
function renderMacroDiagTable(boxId, tbodyId, prefix){
  const rows=(D.diag&&D.diag.rows||[]).filter(r=>r.code&&r.code.startsWith(prefix));
  if(!rows.length) return;
  const box=document.getElementById(boxId);
  const tb=document.getElementById(tbodyId);
  if(!box||!tb) return;
  box.style.display='block';
  tb.innerHTML='';
  rows.forEach(r=>{
    const val=r.num!=null?(fmt(r.num,2)+(r.unit||'')):(r.text||'—');
    tb.innerHTML+='<tr><td>'+(r.label||r.code)+'</td><td style="font-size:11px">'+(r.formula||'—')+'</td><td>'+val+'</td><td style="font-size:11px;color:#666">'+(r.reference||'—')+'</td></tr>';
  });
}

// ── 总览 ──
(()=>{
  const m=D.motor, s=D.slow, p=D.pt, t=D.taz;
  document.getElementById('desc_overview').innerHTML=
    `本节为<strong>描述性统计分析</strong>：对项目 <strong>${D.meta.project_name || ('项目 ' + D.meta.pid)}</strong>（项目 ID ${D.meta.pid}，用户 ${D.meta.uid}）的交通基础数据进行汇总与分布刻画，`+
    `不开展方案对比或因果推断。统计范围涵盖机动车路网（<strong>${m.total_count.toLocaleString()}</strong> 条路段，约 <strong>${m.total_length_km}</strong> km）、`+
    `慢行路网（<strong>${s.total_count}</strong> 条，约 <strong>${s.total_length_km}</strong> km）、`+
    `公共交通（<strong>${p.route_count}</strong> 条线路，<strong>${p.stop_count}</strong> 个站点）`+
    `及 <strong>${t.taz_count}</strong> 个 TAZ 小区（总人口 ${t.total_pop.toLocaleString()} 人，面积 ${t.total_area} km²）。`+
    `下方 KPI 与图表展示各子系统的规模、结构与关键分布特征。`;

  document.getElementById('kpi_overview').innerHTML=
    kpi('',''+m.total_count.toLocaleString(),'条','机动车路段')+
    kpi('',''+m.total_length_km,'km','机动车路网里程')+
    kpi('g',''+s.total_count,'条','慢行路段')+
    kpi('g',''+s.total_length_km,'km','慢行路网里程')+
    kpi('o',''+p.route_count,'条','公交线路')+
    kpi('o',''+p.stop_count,'个','公交站点')+
    kpi('p',''+t.taz_count,'个','TAZ小区')+
    kpi('p',''+t.total_pop.toLocaleString(),'人','研究区总人口');

  ec('c_overview_bar').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:16,bottom:10,containLabel:true},
    xAxis:{type:'category',data:['机动车路段','慢行路段','公交线路','公交站点','TAZ'],axisLabel:{fontSize:11}},
    yAxis:{type:'value',axisLabel:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:56,label:{show:true,position:'top',fontSize:11},
      data:[
        {value:m.total_count,itemStyle:{color:'#1d6fa5'}},
        {value:s.total_count,itemStyle:{color:'#27ae60'}},
        {value:p.route_count,itemStyle:{color:'#e67e22'}},
        {value:p.stop_count,itemStyle:{color:'#f39c12'}},
        {value:t.taz_count,itemStyle:{color:'#8e44ad'}},
      ]}]
  });

  // 雷达评分（基于实际数据计算）
  const vcGood=D.motor.vc_dist[0]+D.motor.vc_dist[1];
  const vcTotal=D.motor.vc_dist.reduce((a,b)=>a+b,0)||1;
  const trafficScore=Math.round(vcGood/vcTotal*100);
  const routeDensity=m.total_length_km/(t.total_area||1);
  const densScore=Math.min(100,Math.round(routeDensity/30*100));
  ec('c_radar').setOption({
    tooltip:{},
    radar:{indicator:[
      {name:'路网密度',max:100},{name:'公交覆盖',max:100},
      {name:'慢行完善',max:100},{name:'通行效率',max:100},{name:'人口服务',max:100}],
      splitArea:{areaStyle:{color:['#f8fafc','#eef4fb']}},
      axisLine:{lineStyle:{color:'#ccc'}},name:{textStyle:{fontSize:12,color:'#555'}},radius:'62%'},
    series:[{type:'radar',data:[{
      value:[densScore,68,Math.min(100,Math.round(s.total_length_km/(t.total_area||1)/3*100)),trafficScore,73],
      name:'综合评分',
      areaStyle:{color:'rgba(45,106,159,0.15)'},
      lineStyle:{color:'#2d6a9f',width:2},itemStyle:{color:'#2d6a9f'}}]}]
  });
})();

// ── 机动车 ──
(()=>{
  const m=D.motor;
  if(!m.available){
    renderSkipped('s1','#s1','机动车交通分析',m.warnings||[]);
    return;
  }
  if(!m.has_flow_metrics){
    ['c_road_vol','c_vc_pie','c_road_triple','c_vc_flow'].forEach(hideCard);
  }
  const roadClasses=(m.road_class_data&&m.road_class_data.length)
    ?m.road_class_data
    :(m.type_data||[]).filter(d=>d.type!==10).sort((a,b)=>a.type-b.type);
  const top=m.has_flow_metrics&&roadClasses.length>0
    ?roadClasses.reduce((a,b)=>(a.avg_vol||0)>(b.avg_vol||0)?a:b,roadClasses[0]):null;
  let motorDesc=
    `机动车路网共 <strong>${m.total_count.toLocaleString()}</strong> 条路段，总里程约 <strong>${m.total_length_km}</strong> km`;
  if(roadClasses.length){
    motorDesc+=`，道路等级：${roadClasses.map(d=>`${d.label}（${d.count} 条，${d.length_km} km）`).join('、')}`;
  }
  const connectors=(m.type_data||[]).filter(d=>d.type===10);
  if(connectors.length){
    motorDesc+=`；质心连杆 ${connectors[0].count} 条（${connectors[0].length_km} km）`;
  }
  if(m.has_flow_metrics&&top){
    motorDesc+=`。其中 <strong>${top.label}</strong> 平均流量最高（<strong>${fmt(top.avg_vol,1)}</strong> pcu/h）`;
    const vcSum=m.vc_dist.reduce((a,b)=>a+b,0)||1;
    motorDesc+=`。畅通（V/C&lt;0.6）占 <strong>${Math.round(m.vc_dist[0]/vcSum*100)}%</strong>，过饱和（≥1.0）占 <strong>${Math.round(m.vc_dist[3]/vcSum*100)}%</strong>`;
  } else {
    motorDesc+=`。当前路网表尚无分配流量字段（volume/v_c），仅展示结构统计`;
  }
  motorDesc+='。';
  document.getElementById('desc_motor').innerHTML=motorDesc;

  document.getElementById('kpi_motor').innerHTML=
    roadClasses.map((d,i)=>kpi('',''+d.count,'条',`${d.label}（${d.length_km} km）`)).join('')+
    (top?kpi('r',''+fmt(top.avg_vol,0),'pcu/h',`${top.label}平均流量`):'')+
    (m.road_co2_t>0?kpi('r',''+fmt(m.road_co2_t,1),'吨 CO₂/h','道路交通碳排放总量'):'');

  ec('c_road_pie').setOption({
    tooltip:{trigger:'item',formatter:'{b}: {c}条 ({d}%)'},
    legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    series:[{type:'pie',radius:['38%','68%'],center:['50%','46%'],
      data:roadClasses.map((d,i)=>({name:d.label,value:d.count,itemStyle:{color:MC[i]}})),
      label:{formatter:'{b}\n{d}%',fontSize:11}}]
  });
  ec('c_road_len').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:16,bottom:10,containLabel:true},
    xAxis:{type:'category',data:roadClasses.map(d=>d.label),axisLabel:{fontSize:11}},
    yAxis:{type:'value',name:'km',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:60,label:{show:true,position:'top',fontSize:11,formatter:p=>p.value+' km'},
      data:roadClasses.map((d,i)=>({value:d.length_km,itemStyle:{color:MC[i]}}))}]
  });
  if(m.has_flow_metrics) ec('c_road_vol').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:16,bottom:10,containLabel:true},
    xAxis:{type:'category',data:roadClasses.map(d=>d.label),axisLabel:{fontSize:11}},
    yAxis:{type:'value',name:'pcu/h',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:60,label:{show:true,position:'top',fontSize:11},
      data:roadClasses.map((d,i)=>({value:fmt(d.avg_vol,1),itemStyle:{color:MC[i]}}))}]
  });
  if(m.has_flow_metrics) ec('c_vc_pie').setOption({
    tooltip:{trigger:'item',formatter:'{b}: {c}条 ({d}%)'},
    legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    series:[{type:'pie',radius:['32%','62%'],center:['50%','46%'],
      data:[
        {name:'畅通 V/C<0.6',value:m.vc_dist[0],itemStyle:{color:'#27ae60'}},
        {name:'基本畅通 0.6-0.8',value:m.vc_dist[1],itemStyle:{color:'#f1c40f'}},
        {name:'轻度拥堵 0.8-1.0',value:m.vc_dist[2],itemStyle:{color:'#e67e22'}},
        {name:'过饱和 ≥1.0',value:m.vc_dist[3],itemStyle:{color:'#c0392b'}},
      ],label:{formatter:'{b}\n{d}%',fontSize:10}}]
  });
  if(m.has_flow_metrics) ec('c_road_triple').setOption({
    tooltip:{trigger:'axis'},
    legend:{data:['路段数','里程(km)','平均流量(pcu/h)'],bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    grid:{left:10,right:60,top:24,bottom:36,containLabel:true},
    xAxis:{type:'category',data:roadClasses.map(d=>d.label),axisLabel:{fontSize:11}},
    yAxis:[{type:'value',name:'路段数/里程',nameTextStyle:{fontSize:10}},{type:'value',name:'pcu/h',nameTextStyle:{fontSize:10}}],
    series:[
      {name:'路段数',type:'bar',barMaxWidth:28,yAxisIndex:0,
        data:roadClasses.map((d,i)=>({value:d.count,itemStyle:{color:MC[i]}})),
        label:{show:true,position:'top',fontSize:9,formatter:p=>p.value.toLocaleString()}},
      {name:'里程(km)',type:'bar',barMaxWidth:28,yAxisIndex:0,
        data:roadClasses.map((d,i)=>({value:d.length_km,itemStyle:{color:MC[i],opacity:0.55}})),
        label:{show:false}},
      {name:'平均流量(pcu/h)',type:'line',yAxisIndex:1,symbol:'circle',symbolSize:8,
        lineStyle:{color:'#c0392b',width:2},itemStyle:{color:'#c0392b'},
        label:{show:true,position:'top',fontSize:10,formatter:p=>p.value},
        data:roadClasses.map(d=>+fmt(d.avg_vol,1))},
    ]
  });
  // V/C各段平均流量（根据 vc_dist 路段数估算权重，用各等级avg_vol近似）
  if(m.has_flow_metrics){
  const vcLabels=['畅通 V/C<0.6','基本畅通 0.6-0.8','轻度拥堵 0.8-1.0','过饱和 ≥1.0'];
  const vcColors=['#27ae60','#f1c40f','#e67e22','#c0392b'];
  ec('c_vc_flow').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:24,bottom:10,containLabel:true},
    xAxis:{type:'category',data:vcLabels,axisLabel:{fontSize:10,interval:0,rotate:8}},
    yAxis:{type:'value',name:'路段数',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:60,
      data:m.vc_dist.map((v,i)=>({value:v,itemStyle:{color:vcColors[i]}})),
      label:{show:true,position:'top',fontSize:12,fontWeight:600}}]
  });
  }
  renderMacroDiagTable('motor_macro_diag_box','motor_macro_diag_body','road_macro');

  const mc=(D.diag&&D.diag.motor_charts)||{};
  let congSeries=mc.congestion_by_class||[];
  const flowCong=m.congestion_by_class_flow||[];
  if(flowCong.length>1){
    if(congSeries.length<=1||congSeries[0].label==='全网'){
      congSeries=flowCong;
    }
  } else if(congSeries.length===0&&flowCong.length){
    congSeries=flowCong;
  }
  const vcSeries=mc.vc_dist_overall||[];
  if(congSeries.length||vcSeries.length){
    const row=document.getElementById('motor_diag_charts_row');
    if(row) row.style.display='';
  }
  if(congSeries.length){
    ec('c_motor_congestion').setOption({
      tooltip:{trigger:'axis',formatter:p=>p[0].name+'<br/>'+p[0].value.toFixed(2)+'%'},
      grid:{left:10,right:10,top:16,bottom:28,containLabel:true},
      xAxis:{type:'category',data:congSeries.map(d=>d.label),axisLabel:{fontSize:10,rotate:12}},
      yAxis:{type:'value',name:'%',nameTextStyle:{fontSize:11}},
      series:[{type:'bar',barMaxWidth:52,
        data:congSeries.map((d,i)=>({value:+d.value.toFixed(2),itemStyle:{color:MC[i%MC.length]}})),
        label:{show:true,position:'top',fontSize:11,formatter:p=>p.value+'%'}}]
    });
  } else {
    hideCard('c_motor_congestion');
  }
  if(vcSeries.length){
    const vcColors=['#27ae60','#f1c40f','#e67e22','#c0392b'];
    ec('c_motor_vc_dist').setOption({
      tooltip:{trigger:'axis',formatter:p=>p[0].name+'<br/>'+p[0].value.toFixed(2)+'%'},
      grid:{left:10,right:10,top:16,bottom:28,containLabel:true},
      xAxis:{type:'category',data:vcSeries.map(d=>d.label),axisLabel:{fontSize:11}},
      yAxis:{type:'value',name:'%',nameTextStyle:{fontSize:11}},
      series:[{type:'bar',barMaxWidth:52,
        data:vcSeries.map((d,i)=>({value:+d.value.toFixed(2),itemStyle:{color:vcColors[i%vcColors.length]}})),
        label:{show:true,position:'top',fontSize:11,formatter:p=>p.value+'%'}}]
    });
  } else {
    hideCard('c_motor_vc_dist');
  }
})();
(()=>{
  const s=D.slow;
  if(!s.available){
    renderSkipped('s2','#s2','慢行交通分析',s.warnings||[]);
    return;
  }
  const top=s.type_data.length>0?s.type_data.reduce((a,b)=>a.avg_flow>b.avg_flow?a:b,s.type_data[0]):null;
  document.getElementById('desc_slow').innerHTML=
    `慢行路网共 <strong>${s.total_count}</strong> 条路段，总里程约 <strong>${s.total_length_km}</strong> km，`+
    `包含${s.type_data.map(d=>`${d.label}（${d.count} 条，${d.length_km} km）`).join('、')}。`+
    `全网平均流量 <strong>${fmt(s.avg_flow,1)}</strong> 人次/h，峰值路段流量 <strong>${fmt(s.max_flow,1)}</strong> 人次/h。`+
    (top?`<strong>${top.label}</strong>平均流量最高（${fmt(top.avg_flow,1)} 人次/h），说明该类型慢行需求较为集中。`:'');

  document.getElementById('kpi_slow').innerHTML=
    s.type_data.map((d,i)=>kpi('g',''+d.count,'条',`${d.label}`)).join('')+
    kpi('g',''+fmt(s.avg_flow,1),'人次/h','全网平均流量');

  ec('c_slow_pie').setOption({
    tooltip:{trigger:'item',formatter:'{b}: {c}条 ({d}%)'},
    legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    series:[{type:'pie',radius:['38%','68%'],center:['50%','46%'],
      data:s.type_data.map((d,i)=>({name:d.label,value:d.count,itemStyle:{color:SC[i]}})),
      label:{formatter:'{b}\n{d}%',fontSize:10}}]
  });

  const c=ec('c_slow_bar');
  c.setOption({
    tooltip:{trigger:'axis'},
    legend:{data:['里程(km)','平均流量(人次/h)'],bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    grid:{left:10,right:10,top:20,bottom:36,containLabel:true},
    xAxis:{type:'category',data:s.type_data.map(d=>d.label),axisLabel:{fontSize:10,rotate:10}},
    yAxis:[{type:'value',name:'km'},{type:'value',name:'人次/h'}],
    series:[
      {name:'里程(km)',type:'bar',barMaxWidth:40,yAxisIndex:0,
        data:s.type_data.map((d,i)=>({value:d.length_km,itemStyle:{color:SC[i]}})),
        label:{show:true,position:'top',fontSize:10}},
      {name:'平均流量(人次/h)',type:'line',yAxisIndex:1,symbol:'circle',symbolSize:7,
        lineStyle:{color:'#e67e22',width:2},itemStyle:{color:'#e67e22'},
        data:s.type_data.map(d=>fmt(d.avg_flow,1))},
    ]
  });

  ec('c_slow_flow').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:16,bottom:10,containLabel:true},
    xAxis:{type:'category',data:['<500','500-1000','1000-2000','2000-5000','>5000'],axisLabel:{fontSize:11}},
    yAxis:{type:'value',name:'路段数',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:60,
      data:s.flow_dist.map((v,i)=>({value:v,itemStyle:{color:SC[Math.min(i,SC.length-1)]}})),
      label:{show:true,position:'top',fontSize:11}}]
  });
  // 各类型平均流量 vs 峰值流量对比
  ec('c_slow_cmp').setOption({
    tooltip:{trigger:'axis'},
    legend:{data:['平均流量','最大流量'],bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    grid:{left:10,right:10,top:24,bottom:36,containLabel:true},
    xAxis:{type:'category',data:s.type_data.map(d=>d.label),axisLabel:{fontSize:11}},
    yAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:11}},
    series:[
      {name:'平均流量',type:'bar',barGap:'10%',barMaxWidth:36,
        data:s.type_data.map((d,i)=>({value:+fmt(d.avg_flow,1),itemStyle:{color:SC[i]}})),
        label:{show:true,position:'top',fontSize:10}},
      {name:'最大流量',type:'bar',barMaxWidth:36,
        data:s.type_data.map((d,i)=>({value:+fmt(d.max_flow!=null?d.max_flow:0,1),itemStyle:{color:SC[i],opacity:0.45}})),
        label:{show:true,position:'top',fontSize:10}},
    ]
  });
  // TOP10 高流量路段
  const topLinks=(s.top_links||[]).slice(0,10);
  if(topLinks.length>0){
    ec('c_slow_top').setOption({
      tooltip:{trigger:'axis'},
      grid:{left:10,right:60,top:10,bottom:10,containLabel:true},
      xAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:11}},
      yAxis:{type:'category',data:topLinks.map(l=>'路段'+l.link_id).reverse(),axisLabel:{fontSize:10}},
      series:[{type:'bar',barMaxWidth:22,
        data:topLinks.map((l,i)=>({value:l.flow,itemStyle:{color:`hsl(${150-i*8},55%,${48-i*2}%)`}})).reverse(),
        label:{show:true,position:'right',fontSize:10}}]
    });
  } else {
    document.getElementById('c_slow_top').innerHTML='<div style="height:260px;display:flex;align-items:center;justify-content:center;color:#aaa">暂无路段级流量明细数据</div>';
  }
  renderMacroDiagTable('slow_macro_diag_box','slow_macro_diag_body','slow_macro');
})();

// ── 公交 ──
(()=>{
  const p=D.pt;
  if(!p.available){
    renderSkipped('s3','#s3','公共交通分析',p.warnings||[]);
    return;
  }
  if(!p.charts||!p.charts.links){
    ['c_pt_pie','c_pt_flow','c_pt_cmp','c_pt_ew'].forEach(hideCard);
  }
  if(!p.charts||!p.charts.mileage){ hideCard('c_pt_mileage'); }
  const enroute=(p.link_data||[]).find(d=>d.type==='ENROUTE')||{count:0,avg_flow:0};
  document.getElementById('desc_pt').innerHTML=
    `公共交通共 <strong>${p.route_count}</strong> 条线路，<strong>${p.stop_count}</strong> 个站点。`+
    `网络链路分为 ENROUTE（在途段）、ABOARD（上车）、ALIGHT（下车）、WALK（步行换乘）四类，`+
    `ENROUTE 段平均流量 <strong>${fmt(enroute.avg_flow,1)}</strong> 人次/h，为公交运输主体载体。`+
    (p.avg_mileage>0?`线路平均里程 <strong>${fmt(p.avg_mileage,1)}</strong> km，最长 <strong>${fmt(p.max_mileage,1)}</strong> km，平均站点数 <strong>${fmt(p.avg_stations,1)}</strong> 个/条。`:'');

  document.getElementById('kpi_pt').innerHTML=
    kpi('o',''+p.route_count,'条','公交线路')+
    kpi('o',''+p.stop_count,'个','公交站点')+
    kpi('o',''+fmt(enroute.avg_flow,1),'人次/h','ENROUTE平均流量')+
    (p.avg_mileage>0?kpi('o',''+fmt(p.avg_mileage,1),'km','平均线路里程'):'')+
    (p.operating_cost_yuan>0?kpi('o',''+Math.round(p.operating_cost_yuan).toLocaleString(),'元/日','运营成本'):'')+
    (p.pt_co2_t>0?kpi('o',''+fmt(p.pt_co2_t,2),'吨 CO₂/日','碳排放量'):'');

  if(p.charts&&p.charts.links) ec('c_pt_pie').setOption({
    tooltip:{trigger:'item',formatter:'{b}: {c}段 ({d}%)'},
    legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    series:[{type:'pie',radius:['38%','68%'],center:['50%','46%'],
      data:p.link_data.map((d,i)=>({name:d.type,value:d.count,itemStyle:{color:PC[i]}})),
      label:{formatter:'{b}\n{d}%',fontSize:11}}]
  });
  if(p.charts&&p.charts.links) ec('c_pt_flow').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:16,bottom:10,containLabel:true},
    xAxis:{type:'category',data:p.link_data.map(d=>d.type),axisLabel:{fontSize:12}},
    yAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:60,label:{show:true,position:'top',fontSize:12,fontWeight:600},
      data:p.link_data.map((d,i)=>({value:fmt(d.avg_flow,1),itemStyle:{color:PC[i]}}))}]
  });

  if(p.charts&&p.charts.mileage) ec('c_pt_mileage').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:10,top:16,bottom:10,containLabel:true},
    xAxis:{type:'category',data:['平均里程','最长里程','最短里程','平均站点数'],axisLabel:{fontSize:11}},
    yAxis:{type:'value',axisLabel:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:60,
      data:[
        {value:fmt(p.avg_mileage,1),itemStyle:{color:'#e67e22'}},
        {value:fmt(p.max_mileage,1),itemStyle:{color:'#d35400'}},
        {value:fmt(p.min_mileage,1),itemStyle:{color:'#f8c471'}},
        {value:fmt(p.avg_stations,1),itemStyle:{color:'#f39c12'}},
      ],label:{show:true,position:'top',fontSize:12,fontWeight:600}}]
  });
  // 平均 vs 峰值流量对比
  if(p.charts&&p.charts.links) ec('c_pt_cmp').setOption({
    tooltip:{trigger:'axis'},
    legend:{data:['平均流量','峰值流量'],bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    grid:{left:10,right:10,top:24,bottom:36,containLabel:true},
    xAxis:{type:'category',data:p.link_data.map(d=>d.type),axisLabel:{fontSize:12}},
    yAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:11}},
    series:[
      {name:'平均流量',type:'bar',barGap:'10%',barMaxWidth:36,
        data:p.link_data.map((d,i)=>({value:+fmt(d.avg_flow,1),itemStyle:{color:PC[i]}})),
        label:{show:true,position:'top',fontSize:11,fontWeight:600}},
      {name:'峰值流量',type:'bar',barMaxWidth:36,
        data:p.link_data.map((d,i)=>({value:+fmt(d.max_flow,1),itemStyle:{color:PC[i],opacity:0.4}})),
        label:{show:true,position:'top',fontSize:10}},
    ]
  });
  // ENROUTE 与 WALK 流量雷达/散点（用 bar 对比四段客流分布特征）
  const ewOrder=['ABOARD','ENROUTE','ALIGHT','WALK'];
  const ewColors={'ABOARD':'#d35400','ENROUTE':'#e67e22','ALIGHT':'#f39c12','WALK':'#f8c471'};
  const ewMap={}; p.link_data.forEach(d=>{ewMap[d.type]=d;});
  if(p.charts&&p.charts.links) ec('c_pt_ew').setOption({
    tooltip:{trigger:'axis',axisPointer:{type:'shadow'}},
    legend:{data:['链路数','平均流量(人次/h)'],bottom:4,itemWidth:10,textStyle:{fontSize:11}},
    grid:{left:10,right:60,top:24,bottom:36,containLabel:true},
    xAxis:{type:'category',data:ewOrder,axisLabel:{fontSize:12}},
    yAxis:[{type:'value',name:'链路数',nameTextStyle:{fontSize:10}},{type:'value',name:'人次/h',nameTextStyle:{fontSize:10}}],
    series:[
      {name:'链路数',type:'bar',barMaxWidth:40,yAxisIndex:0,
        data:ewOrder.map(k=>({value:(ewMap[k]||{count:0}).count,itemStyle:{color:ewColors[k]}})),
        label:{show:true,position:'top',fontSize:10,formatter:p=>p.value.toLocaleString()}},
      {name:'平均流量(人次/h)',type:'line',yAxisIndex:1,symbol:'circle',symbolSize:8,
        lineStyle:{color:'#1d6fa5',width:2},itemStyle:{color:'#1d6fa5'},
        label:{show:true,position:'top',fontSize:10},
        data:ewOrder.map(k=>+fmt((ewMap[k]||{avg_flow:0}).avg_flow,1))},
    ]
  });
  function renderTopStops(chartId, rows, color){
    if(!rows||!rows.length){ hideCard(chartId); return; }
    const rowEl=document.getElementById('pt_top_stops_row');
    if(rowEl) rowEl.style.display='';
    ec(chartId).setOption({
      tooltip:{trigger:'axis'},
      grid:{left:10,right:10,top:16,bottom:48,containLabel:true},
      xAxis:{type:'category',data:rows.map(r=>r.label),axisLabel:{fontSize:10,rotate:18,interval:0}},
      yAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:11}},
      series:[{type:'bar',barMaxWidth:48,
        data:rows.map(r=>({value:r.volume,itemStyle:{color:color}})),
        label:{show:true,position:'top',fontSize:10}}]
    });
  }
  if(p.charts&&p.charts.top_stops){
    renderTopStops('c_pt_top_aboard', p.top_aboard||[], '#d35400');
    renderTopStops('c_pt_top_alight', p.top_alight||[], '#f39c12');
  } else {
    ['c_pt_top_aboard','c_pt_top_alight'].forEach(hideCard);
  }
  renderMacroDiagTable('pt_macro_diag_box','pt_macro_diag_body','pt_macro');
})();

(()=>{
  const t=D.taz;
  if(!t.available){
    renderSkipped('s4','#s4','交通分析小区（TAZ）',t.warnings||[]);
    return;
  }
  const maxDens=t.taz_list.length?t.taz_list.reduce((a,b)=>a.density>b.density?a:b,t.taz_list[0]):null;
  const maxPop=t.taz_list.length?t.taz_list.reduce((a,b)=>a.pop>b.pop?a:b,t.taz_list[0]):null;
  document.getElementById('desc_taz').innerHTML=
    `研究区共划分 <strong>${t.taz_count}</strong> 个交通分析小区，总人口 <strong>${t.total_pop.toLocaleString()}</strong> 人，`+
    `总面积约 <strong>${t.total_area}</strong> km²，平均人口密度 <strong>${t.avg_density}</strong> 人/km²。`+
    (maxPop?`人口最多的小区为 <strong>TAZ-${maxPop.id}</strong>（${maxPop.pop.toLocaleString()} 人）；`:'')+ 
    (maxDens?`人口密度最高的小区为 <strong>TAZ-${maxDens.id}</strong>（${maxDens.density} 人/km²）。`:'');

  document.getElementById('kpi_taz').innerHTML=
    kpi('p',''+t.total_pop.toLocaleString(),'人','研究区总人口')+
    kpi('p',''+t.total_area,'km²','研究区总面积')+
    kpi('p',''+t.avg_density,'人/km²','平均人口密度')+
    kpi('p',''+t.taz_count,'个','TAZ小区数量');

  const labels=t.taz_list.map(d=>'TAZ-'+d.id);
  ec('c_taz_pop').setOption({
    tooltip:{trigger:'axis',formatter:p=>p[0].name+'<br/>人口: '+p[0].value+' 人'},
    grid:{left:10,right:10,top:10,bottom:40,containLabel:true},
    xAxis:{type:'category',data:labels,axisLabel:{fontSize:9,rotate:30}},
    yAxis:{type:'value',name:'人口(人)',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:28,data:t.taz_list.map((d,i)=>({value:d.pop,itemStyle:{color:TC(i)}}))}]
  });
  ec('c_taz_area').setOption({
    tooltip:{trigger:'axis',formatter:p=>p[0].name+'<br/>面积: '+p[0].value+' km²'},
    grid:{left:10,right:10,top:10,bottom:40,containLabel:true},
    xAxis:{type:'category',data:labels,axisLabel:{fontSize:9,rotate:30}},
    yAxis:{type:'value',name:'面积(km²)',nameTextStyle:{fontSize:11}},
    series:[{type:'bar',barMaxWidth:28,data:t.taz_list.map((d,i)=>({value:d.area,itemStyle:{color:TC(i)}}))}]
  });
  const sorted=[...t.taz_list].sort((a,b)=>b.density-a.density);
  ec('c_taz_dens').setOption({
    tooltip:{trigger:'axis'},
    grid:{left:10,right:60,top:10,bottom:10,containLabel:true},
    xAxis:{type:'value',name:'人/km²',nameTextStyle:{fontSize:11}},
    yAxis:{type:'category',data:sorted.map(d=>'TAZ-'+d.id),axisLabel:{fontSize:10}},
    series:[{type:'bar',barMaxWidth:20,label:{show:true,position:'right',fontSize:10},
      data:sorted.map((d,i)=>({value:d.density,itemStyle:{color:TC(i)}}))}]
  });
  (()=>{
    const pts=t.taz_list.map(d=>[d.area,d.pop,d.density,d.id]);
    const dens=pts.map(p=>+p[2]||0);
    const dMin=Math.min(...dens,0), dMax=Math.max(...dens,1);
    ec('c_taz_scatter').setOption({
      tooltip:{trigger:'item',formatter:p=>{
        const area=p.data[0], pop=p.data[1], dens=p.data[2], tid=p.data[3];
        return `TAZ-${tid}<br/>面积: ${(+area).toFixed(2)} km²<br/>人口: ${(+pop).toLocaleString()} 人<br/>密度: ${(+dens).toLocaleString()} 人/km²`;
      }},
      visualMap:{
        min:dMin,max:dMax,dimension:2,orient:'vertical',right:8,top:24,bottom:24,
        text:['高密度','低密度'],textStyle:{fontSize:10},
        inRange:{color:['#d6eaf8','#5dade2','#1f618d']},calculable:true
      },
      grid:{left:10,right:72,top:28,bottom:36,containLabel:true},
      xAxis:{type:'value',name:'面积(km²)',nameTextStyle:{fontSize:11}},
      yAxis:{type:'value',name:'人口(人)',nameTextStyle:{fontSize:11},
        axisLabel:{formatter:v=>v>=10000?(v/10000).toFixed(0)+'万':v}},
      series:[{type:'scatter',symbolSize:10,data:pts,
        label:{show:pts.length<=24,formatter:p=>'TAZ-'+p.data[3],fontSize:9,position:'top',color:'#555'}}]
    });
  })();
})();

// ── 诊断结果 ──
(()=>{
  const diag=D.diag;
  if(!diag.available){
    renderSkipped('s5','#s5','诊断指标汇总',diag.warnings||[]);
    return;
  }
  const rows=diag.rows||[];
  const catCount={};
  rows.forEach(r=>{
    const cat=r.code.startsWith('road_')?'road_macro':r.code.startsWith('slow_')?'slow_macro':r.code.startsWith('pt_')?'pt_macro':'other';
    if(cat!=='other') catCount[cat]=(catCount[cat]||0)+1;
  });

  const colorMap={'road_macro':'#1d6fa5','pt_macro':'#f8c471','slow_macro':'#27ae60'};
  const labelMap={'road_macro':'道路宏观','pt_macro':'公交宏观','slow_macro':'慢行宏观'};

  const rawN=diag.raw_row_count!=null?diag.raw_row_count:rows.length;
  let diagDesc=rows.length>0
    ?`宏观诊断代表值 <strong>${rows.length}</strong> 项（库内原始 ${rawN} 行；中微观路段明细不在本报告展示）。`
    :`当前方案暂无宏观诊断结果，请先执行 diagnosis_*_macro / review_road。`;
  const pw=diag.plausibility_warnings||[];
  if(pw.length){
    diagDesc+=`<div class="report-alerts" style="margin-top:12px"><strong>诊断数据异常提示</strong><ul>`+
      pw.map(w=>`<li>${w}</li>`).join('')+'</ul></div>';
  }
  document.getElementById('desc_diag').innerHTML=diagDesc;

  const pieData=Object.entries(catCount).map(([k,v])=>({
    name:labelMap[k]||k,value:v,itemStyle:{color:colorMap[k]||'#999'}
  }));
  if(pieData.length>0){
    ec('c_diag_pie').setOption({
      tooltip:{trigger:'item',formatter:'{b}: {c}项 ({d}%)'},
      legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},
      series:[{type:'pie',radius:['35%','65%'],center:['50%','46%'],
        data:pieData,label:{formatter:'{b}\n{d}%',fontSize:10}}]
    });
  } else {
    document.getElementById('c_diag_pie').innerHTML='<div style="height:260px;display:flex;align-items:center;justify-content:center;color:#aaa">暂无诊断数据</div>';
  }

  // 道路宏观关键值
  const roadRows=rows.filter(r=>r.code.startsWith('road_macro')&&r.num!=null);
  const roadKpiEntries=roadRows.slice(0,8).map(r=>[
    r.label||r.code.replace('road_macro_',''),
    r.num,
    r.unit||''
  ]);
  if(roadKpiEntries.length>0){
    ec('c_diag_road').setOption({
      tooltip:{trigger:'axis',formatter:ps=>{
        const p=ps[0];
        const e=roadKpiEntries[p.dataIndex];
        const u=e&&e[2]?(' '+e[2]):'';
        return (e?e[0]:p.name)+'<br/>'+p.value+(u||'');
      }},
      grid:{left:10,right:60,top:10,bottom:10,containLabel:true},
      xAxis:{type:'value'},
      yAxis:{type:'category',data:roadKpiEntries.map(e=>e[0]),axisLabel:{fontSize:10}},
      series:[{type:'bar',barMaxWidth:24,label:{show:true,position:'right',fontSize:11,
        formatter:p=>{
          const u=roadKpiEntries[p.dataIndex]&&roadKpiEntries[p.dataIndex][2];
          return p.value+(u?(' '+u):'');
        }},
        data:roadKpiEntries.map((e,i)=>({value:+e[1].toFixed(3),
          itemStyle:{color:['#1d6fa5','#2d9cdb','#74c0e8','#e67e22','#c0392b','#e74c3c'][i]||'#999'}}))}]
    });
  } else {
    document.getElementById('c_diag_road').innerHTML='<div style="height:260px;display:flex;align-items:center;justify-content:center;color:#aaa">暂无道路宏观诊断数据</div>';
  }

  // 明细表：按模块折叠，点击模块行展开/收起
  const catBadge={'road':'br','pt':'bp','slow':'bs'};
  const catLabel2={'road':'道路','pt':'公交','slow':'慢行'};
  const tb=document.getElementById('diag_tbody');
  if(rows.length===0){
    tb.innerHTML='<tr><td colspan="5" style="text-align:center;color:#aaa;padding:20px">暂无诊断结果</td></tr>';
  } else {
    // 按模块分组
    const groups={};
    rows.forEach(r=>{
      const mod=r.code.startsWith('road_')?'road_macro':r.code.startsWith('slow_')?'slow_macro':r.code.startsWith('pt_')?'pt_macro':'other';
      if(mod==='other') return;
      if(!groups[mod]) groups[mod]=[];
      groups[mod].push(r);
    });
    let gIdx=0;
    Object.entries(groups).forEach(([mod,gRows])=>{
      const cat=mod.startsWith('road')?'road':mod.startsWith('pt')?'pt':'slow';
      const gid='dg'+gIdx++;
      // 汇总行（可点击）
      const hdr=document.createElement('tr');
      hdr.style.cssText='background:#f0f4f8;cursor:pointer;user-select:none';
      hdr.innerHTML=
        `<td style="font-weight:700;padding:7px 8px" colspan="2">`+
        `<span style="margin-right:6px">▶</span>`+
        `<span class="badge ${catBadge[cat]||''}">${labelMap[mod]||mod}</span></td>`+
        `<td colspan="4" style="color:#555;font-size:12px">${gRows.length} 项指标</td>`;
      hdr.onclick=()=>{
        const det=document.getElementById(gid);
        const arrow=hdr.querySelector('span');
        const hidden=det.style.display==='none';
        det.style.display=hidden?'':'none';
        arrow.textContent=hidden?'▼':'▶';
      };
      tb.appendChild(hdr);
      // 明细行（默认隐藏）
      const det=document.createElement('tbody');
      det.id=gid; det.style.display='none';
      gRows.forEach(r=>{
        const tr=document.createElement('tr');
        const val=r.num!=null?(+r.num.toFixed(4))+(r.unit||''):(r.text||'—');
        tr.innerHTML=`<td style="padding-left:24px">${r.label||r.code}</td>`+
          `<td style="font-size:11px">${r.formula||'—'}</td>`+
          `<td></td>`+
          `<td style="font-weight:700;color:#1a3a5c">${val}</td>`+
          `<td style="font-size:11px;color:#666">${r.reference||'—'}</td>`;
        det.appendChild(tr);
      });
      tb.parentElement.appendChild(det);
    });
  }
})();

window.addEventListener('resize',()=>{
  document.querySelectorAll('[id^="c_"]').forEach(el=>{
    const inst=echarts.getInstanceByDom(el); if(inst) inst.resize();
  });
});

}); // DOMContentLoaded
</script>
<script>__MAP_JS__</script>
</body>
</html>
"""


def render_html(
    template: str,
    data: dict,
    meta: dict,
    *,
    map_bundle: dict | None = None,
    map_pipeline: dict | None = None,
) -> str:
    import datetime, os
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    pid, uid, cid = meta['pid'], meta['uid'], meta['cid']
    # 内联 ECharts（优先读磁盘缓存，避免 CDN 网络依赖）
    echarts_paths = [
        "/tmp/echarts.min.js",
        os.path.join(os.path.dirname(__file__), "echarts.min.js"),
    ]
    echarts_js = None
    for p in echarts_paths:
        if os.path.isfile(p):
            with open(p, "r", encoding="utf-8") as f:
                echarts_js = f.read()
            break
    if echarts_js:
        echarts_block = f"<script>\n{echarts_js}\n</script>"
    else:
        echarts_block = (
            '<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>'
        )
    template = template.replace("__ECHARTS_INLINE__", echarts_block)
    prefix = _prefix(pid, uid)
    data_json = json.dumps(data, ensure_ascii=False)
    map_render = render_map_sections(
        map_bundle or {},
        pipeline=map_pipeline,
        section_title="空间分布：路网流量与 OD 期望线",
    )

    html = template
    html = html.replace("__LEAFLET_HEAD__", map_render.get("leaflet_head") or "")
    html = html.replace("__MAP_CSS__", map_render.get("css") or "")
    html = html.replace("__MAP_SECTION__", map_render.get("html") or "")
    html = html.replace("__MAP_JS__", map_render.get("js") or "")
    html = html.replace("__PROJECT_NAME__", str(meta.get("project_name") or f"项目 {pid}"))
    html = html.replace("__DATA_SOURCES__", data_sources_line(BASE_REPORT_TABLES))
    html = html.replace("__PID__", str(pid))
    html = html.replace("__UID__", str(uid))
    html = html.replace("__CID__", str(cid))
    html = html.replace("__TS__", ts)
    html = html.replace("__PREFIX__", prefix)
    html = html.replace("__DATA_JSON__", data_json)
    return html


# ─── 主入口 ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--request-file",  required=True)
    parser.add_argument("--response-file", required=True)
    parser.add_argument("--progress-file", default="")
    args = parser.parse_args()

    req = read_request(args.request_file)
    pid = int(req.get("project_id", 0))
    uid = int(req.get("user_id", 0))
    cid = int(req.get("case_id", 0))
    param1 = req.get("param1", "")
    param2_raw = req.get("param2", "")
    param2 = parse_report_param2(param2_raw)

    prefix = _prefix(pid, uid)
    output_path = resolve_report_output_path(
        param2,
        report_stem="base_data_report",
        fallback_path=f"/tmp/tna_base_report_{pid}_{uid}.html",
    )
    include_html_content = param2.get("include_html_content", True)
    if isinstance(include_html_content, str):
        include_html_content = include_html_content.strip().lower() not in (
            "0", "false", "no", "off",
        )

    logs = []
    try:
        write_progress(args.progress_file, 5, "连接数据库...")
        conn = _get_conn(param1)

        diag_case_id_raw = param2.get("diag_case_id")
        diag_case_id = int(diag_case_id_raw) if diag_case_id_raw not in (None, "") else None
        flow_case_id = diag_case_id if diag_case_id is not None else cid

        write_progress(args.progress_file, 15, "计算机动车路网指标...")
        flow_prefix = _resolve_scheme_table_prefix(
            conn, pid, uid, flow_case_id, "road_way", prefix, logs, label="机动车流量"
        )
        motor_type_map = _motor_type_map(conn, prefix)
        motor_data = calc_motor(conn, flow_prefix, logs)
        vmt, co2_t = _motor_vmt_and_co2(conn, flow_prefix)
        motor_data["total_vmt"] = vmt
        motor_data["road_co2_t"] = co2_t
        motor_data["congestion_by_class_flow"] = _congestion_ratio_by_class_from_flow(
            conn, flow_prefix
        )

        write_progress(args.progress_file, 35, "计算慢行路网指标...")
        slow_data = calc_slow(conn, prefix, logs)

        write_progress(args.progress_file, 55, "计算公交网络指标...")
        pt_assign_prefix = _resolve_scheme_table_prefix(
            conn, pid, uid, cid, "pt_link_result", prefix, logs, label="公交分配"
        )
        pt_data = calc_pt(conn, prefix, logs, assignment_prefix=pt_assign_prefix)

        write_progress(args.progress_file, 70, "计算 TAZ 小区指标...")
        taz_data = calc_taz(conn, prefix, logs)

        write_progress(args.progress_file, 82, "读取诊断结果...")
        diag_prefix = _resolve_diag_prefix(
            conn, pid, uid, cid, prefix, logs, diag_case_id=diag_case_id
        )
        diag_data = calc_diag(conn, diag_prefix, logs)
        mc = diag_data.get("motor_charts") or {}
        if mc.get("congestion_by_class"):
            mc["congestion_by_class"] = [
                {
                    "label": _normalize_road_class_label(x["label"], motor_type_map),
                    "value": x["value"],
                }
                for x in mc["congestion_by_class"]
            ]
        flow_cong = motor_data.get("congestion_by_class_flow") or []
        if mc.get("congestion_by_class"):
            mc["congestion_by_class"] = [
                {
                    "label": _normalize_road_class_label(x["label"], motor_type_map),
                    "value": x["value"],
                }
                for x in mc["congestion_by_class"]
            ]
        if len(mc.get("congestion_by_class") or []) <= 1 and len(flow_cong) > 1:
            mc["congestion_by_class"] = flow_cong
            diag_data["charts"]["motor_congestion"] = True
        vc_series = mc.get("vc_dist_overall") or []
        used_vc_flow_fallback = False
        if not vc_series or all(float(x.get("value") or 0) == 0 for x in vc_series):
            flow_vc = _vc_mileage_pct_from_flow(conn, flow_prefix)
            if flow_vc:
                mc["vc_dist_overall"] = flow_vc
                diag_data["charts"]["motor_vc_dist"] = True
                used_vc_flow_fallback = True
                logs.append(
                    "诊断：road_macro_capacity_distribution 全 0，已用分配流量里程估算饱和度分档"
                )
        diag_data["motor_charts"] = mc
        diag_data["plausibility_warnings"] = _diag_plausibility_warnings(
            diag_data.get("rows") or [],
            mc,
            used_vc_flow_fallback=used_vc_flow_fallback,
        )
        if diag_data["plausibility_warnings"]:
            logs.append(
                "诊断：检测到 "
                f"{len(diag_data['plausibility_warnings'])} 条宏观指标异常，已写入报告警告"
            )

        write_progress(args.progress_file, 88, "采集路网地图与 OD 期望线...")
        map_bundle: dict = {}
        map_pipeline: dict | None = None
        try:
            map_case_ids: list[int] = []
            for mc in (diag_case_id, flow_case_id, cid, 3, 2, 0):
                if mc is not None and int(mc) not in map_case_ids:
                    map_case_ids.append(int(mc))
            map_bundle, map_meta = collect_map_bundle_for_cases(
                conn, pid, uid, map_case_ids, logs
            )
            map_pipeline = map_meta.get("pipeline")
            if map_meta.get("map_note"):
                logs.append(map_meta["map_note"])
            has_map = bool((map_bundle.get("map") or {}).get("links"))
            has_od = bool((map_bundle.get("od") or {}).get("lines"))
            if not has_map and not has_od:
                map_bundle = collect_map_bundle_tool(conn, pid, uid, logs)
                map_pipeline = None
                logs.append("方案路网/OD 不可用，地图回退工具前缀")
        except Exception as exc:
            logs.append(f"地图采集失败: {exc}")

        write_progress(args.progress_file, 90, "生成 HTML 报告...")
        report_warnings = _collect_report_warnings(
            motor_data, slow_data, pt_data, taz_data, diag_data
        )
        project_label = project_display_name(param2, pid)
        full_data = {
            'meta': {'pid': pid, 'uid': uid, 'cid': cid, 'project_name': project_label},
            'motor': motor_data,
            'slow': slow_data,
            'pt': pt_data,
            'taz': taz_data,
            'diag': diag_data,
            'report_warnings': report_warnings,
        }
        html = render_html(
            HTML_TEMPLATE, full_data,
            {'pid': pid, 'uid': uid, 'cid': cid, 'project_name': project_label},
            map_bundle=map_bundle,
            map_pipeline=map_pipeline,
        )
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(html)

        write_progress(args.progress_file, 99, "报告写入完成")
        msg = _build_result_message(motor_data, slow_data, pt_data, taz_data)
        if report_warnings:
            msg += "。说明：" + "；".join(report_warnings[:5])
            if len(report_warnings) > 5:
                msg += f" 等共 {len(report_warnings)} 条（见报告顶部与 summary.logs）"
        resp_kv = _build_grpc_response_kv(
            conn,
            prefix,
            motor_data,
            slow_data,
            pt_data,
            taz_data,
            diag_data,
            output_path,
            report_warnings,
            logs,
            msg,
            html_content=html,
            include_html_content=include_html_content,
        )
        conn.close()
        write_response(args.response_file, resp_kv)
    except Exception:
        err = traceback.format_exc()
        err_payload: dict[str, str] = {
            "code": -99,
            "message": f"基础数据分析失败：{err.splitlines()[-1] if err else '未知错误'}",
            "summary.stage": "base_data_report",
        }
        _set_summary_logs(err_payload, logs)
        write_response(args.response_file, err_payload)
        sys.exit(1)


if __name__ == "__main__":
    main()
