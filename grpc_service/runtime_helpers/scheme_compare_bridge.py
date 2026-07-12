#!/usr/bin/env python3
"""
scheme_compare_bridge.py
方案对比分析报告 Python Bridge

入参（KV 文件，与其他 bridge 相同协议）：

**两方案对比（兼容旧版）**
  project_id, user_id, case_id       — 改造方案（待评估）
  param2 (JSON)：
    base_case_id   int   必填（两方案）  现状/基准方案 case_id
    project_name   str   可选  **param2 内**项目名称；缺省 output_path 时用于文件名前缀与页眉展示
    type, output_path  同前

**多方案对比（新增）**
  param2 (JSON)：
    case_ids              [int,...]  参与对比的方案 ID 列表；**基准固定为 min(case_ids)**
    compare_case_ids      [int,...]  同 case_ids（别名）
  客户端只需传 case_ids；无需 base_case_id

  示例：{"case_ids":[48,44,45]}  → 基准 case44，对比 45、48

  GIS 修改记录（param2 顶层）：
    gis_edit_log   array  GIS 图层修改文案（字符串或 {"text":"..."} 对象）
    ids            string/array  关注路段/要素 ID（可选）

  道路图层框选（param2 顶层 bbox，GeoJSON Polygon 或 [south,west,north,east]）：
    各 case 独立 ST_Intersects 解析 link_id，再聚合对比指标
    summary.attr.link_ids_by_case / link_count_by_case 回传解析结果

出参：
  code=1 / -1
  summary.attributes（KV 键 summary.attr.*）：html_path, base_case_id, case_ids
"""
import os, sys, json, math, re, traceback
from pathlib import Path

try:
    from .network_map_collect import collect_map_bundle_for_cases
    from .network_map_report import render_map_sections
    from .report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
    from .gis_edit_log_utils import collect_gis_edit_log, render_gis_edit_log_section
    from .compare_scope_utils import (
        kv_unescape,
        normalize_bbox_param,
        extract_bbox_from_param2,
        resolve_motor_link_ids_by_bbox,
    )
    from .table_name_cn import (
        table_cn,
        type_cn,
        humanize_db_message,
        data_sources_line,
        tables_for_scheme_compare,
    )
except ImportError:
    from network_map_collect import collect_map_bundle_for_cases
    from network_map_report import render_map_sections
    from report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
    from gis_edit_log_utils import collect_gis_edit_log, render_gis_edit_log_section
    from compare_scope_utils import (
        kv_unescape,
        normalize_bbox_param,
        extract_bbox_from_param2,
        resolve_motor_link_ids_by_bbox,
    )
    from table_name_cn import (
        table_cn,
        type_cn,
        humanize_db_message,
        data_sources_line,
        tables_for_scheme_compare,
    )

# ── KV 协议 ──────────────────────────────────────────────────────────────────
def _read_kv(path):
    kv = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = kv_unescape(v.strip())
    return kv

def _write_kv(path, d):
    with open(path, "w", encoding="utf-8") as f:
        for k, v in d.items():
            f.write(f"{k}={v}\n")

def _fail(out_path, msg):
    _write_kv(out_path, {"code": "-1", "message": msg})
    sys.exit(0)

def _success(out_path, html_path, scheme_ids: list[int], base_cid: int, extra: dict | None = None):
    ids_str = ",".join(str(i) for i in scheme_ids)
    if len(scheme_ids) == 2:
        other = next(c for c in scheme_ids if c != base_cid)
        msg = f"方案对比报告生成成功：基准 case{base_cid} vs case{other}"
    else:
        msg = f"方案对比报告生成成功：基准 case{base_cid}，共 {len(scheme_ids)} 个方案 [{ids_str}]"
    payload = {
        "code": "1",
        "message": msg,
        "summary.attr.html_path": html_path,
        "summary.attr.base_case_id": str(base_cid),
        "summary.attr.case_id": str(scheme_ids[-1] if scheme_ids else base_cid),
        "summary.attr.case_ids": ids_str,
        "summary.attr.scheme_count": str(len(scheme_ids)),
    }
    if extra:
        for k, v in extra.items():
            payload[f"summary.attr.{k}"] = v if isinstance(v, str) else json.dumps(v, ensure_ascii=False)
    _write_kv(out_path, payload)

# ── DB ───────────────────────────────────────────────────────────────────────
def _load_db_conf(conf_path="/opt/algorithms/db.conf"):
    cfg = {}
    for p in [conf_path, "/home/giss/opt_algorithms/db.conf"]:
        try:
            with open(p) as f:
                for line in f:
                    line = line.strip()
                    if "=" in line and not line.startswith("#"):
                        k, v = line.split("=", 1)
                        cfg[k.strip()] = v.strip()
            if cfg:
                break
        except Exception:
            pass
    return cfg

def _connect(cfg):
    import psycopg2
    return psycopg2.connect(
        host=cfg.get("host", "localhost"),
        port=int(cfg.get("port", 5432)),
        dbname=cfg.get("dbname", "urban"),
        user=cfg.get("user", "urban"),
        password=cfg.get("password", ""),
    )


def _pg_rollback(conn) -> None:
    try:
        conn.rollback()
    except Exception:
        pass


def _tbl(prefix, suffix):
    return f'user_project."{prefix}{suffix}"'

def _prefix(pid, uid, cid):
    return f"project{pid}_user{uid}_case{cid}_"


_SCHEME_COLORS = [
    "#2980b9", "#e67e22", "#27ae60", "#8e44ad", "#16a085", "#c0392b", "#f39c12", "#2c3e50",
]
_MAX_SCHEMES = 8


def _scheme_label(cid: int, base_cid: int) -> str:
    return f"Case{cid}" + (" (基准)" if cid == base_cid else "")


def _parse_int_list(raw) -> list[int]:
    if raw is None:
        return []
    if isinstance(raw, list):
        return [int(x) for x in raw]
    s = str(raw).strip()
    if not s:
        return []
    if s.startswith("["):
        return [int(x) for x in json.loads(s)]
    return [int(x.strip()) for x in s.replace(";", ",").split(",") if x.strip()]


def _dedupe_ids(ids: list[int]) -> list[int]:
    seen: set[int] = set()
    out: list[int] = []
    for i in ids:
        if i not in seen:
            seen.add(i)
            out.append(i)
    return out


def _extract_case_ids_fallback(raw: str) -> list[int] | None:
    """param2 JSON 损坏时尝试从原始字符串提取 case_ids。"""
    if not raw:
        return None
    m = re.search(r'"case_ids"\s*:\s*\[([^\]]*)\]', raw)
    if not m:
        m = re.search(r'"compare_case_ids"\s*:\s*\[([^\]]*)\]', raw)
    if not m:
        return None
    try:
        return _parse_int_list("[" + m.group(1) + "]")
    except Exception:
        return None


def _parse_scheme_param2(kv: dict) -> tuple[dict, str]:
    """
    解析 param2；合并嵌套 param2；返回 (dict, raw_for_error)。
    """
    raw = kv.get("param2", "")
    p2 = parse_report_param2(raw)
    inner = p2.get("param2")
    if isinstance(inner, dict):
        merged = {**inner, **{k: v for k, v in p2.items() if k != "param2"}}
        p2 = merged
    elif isinstance(inner, str) and inner.strip():
        p2 = {**parse_report_param2(inner), **{k: v for k, v in p2.items() if k != "param2"}}

    if not p2.get("case_ids") and not p2.get("compare_case_ids"):
        fb = _extract_case_ids_fallback(str(raw))
        if fb:
            p2["case_ids"] = fb
    return p2, str(raw)


def _resolve_scheme_ids(kv: dict, p2: dict) -> tuple[list[int], int]:
    """返回 (有序方案 ID 列表, 基准 case_id)。"""
    cid_top = int(kv.get("case_id", -1))
    base_raw = p2.get("base_case_id")
    list_raw = p2.get("case_ids")
    if list_raw is None:
        list_raw = p2.get("compare_case_ids")

    if list_raw is not None:
        ids = _dedupe_ids(_parse_int_list(list_raw))
        if len(ids) < 1:
            raise ValueError("case_ids / compare_case_ids 不能为空")
        base_cid = min(ids)
        scheme_ids = [base_cid] + [i for i in ids if i != base_cid]
        if cid_top >= 0 and cid_top != base_cid and cid_top not in scheme_ids:
            scheme_ids.append(cid_top)
        if len(scheme_ids) < 2:
            if cid_top >= 0 and cid_top != base_cid:
                scheme_ids = _dedupe_ids([base_cid, cid_top])
            else:
                raise ValueError("多方案对比至少需要 2 个不同的 case_id")
        if len(scheme_ids) > _MAX_SCHEMES:
            raise ValueError(f"最多支持 {_MAX_SCHEMES} 个方案对比，当前 {len(scheme_ids)} 个")
        if base_cid not in scheme_ids:
            scheme_ids = [base_cid] + [i for i in scheme_ids if i != base_cid]
        return scheme_ids, base_cid

    if base_raw is None or str(base_raw).strip() == "":
        raise ValueError(
            "param2 缺少方案 ID：请传 base_case_id + case_id（两方案），"
            "或 case_ids / compare_case_ids（多方案）"
        )
    base_cid = int(base_raw)
    if cid_top < 0:
        raise ValueError("两方案对比时顶层 case_id 必填（改造方案 ID，允许 0）")
    if base_cid == cid_top:
        raise ValueError(f"改造方案 case_id({cid_top}) 与 base_case_id({base_cid}) 不能相同")
    return [base_cid, cid_top], base_cid

def _q(conn, sql):
    with conn.cursor() as cur:
        cur.execute(sql)
        cols = [d[0] for d in cur.description] if cur.description else []
        rows = cur.fetchall()
    return cols, rows

def _safe(v, dec=2):
    try:
        return round(float(v), dec) if v is not None else 0.0
    except Exception:
        return 0.0

# ── 数据采集 ─────────────────────────────────────────────────────────────────

def _motor_stats(conn, prefix, logs, link_ids: list[int] | None = None):
    """机动车网络统计：按路段等级汇总 VMT/VHT/V_C，返回综合指标"""
    tbl = _tbl(prefix, "road_way")
    r = {"ok": False, "type_data": [], "total_vmt": 0, "total_vht": 0,
         "avg_vc": 0, "avg_speed": 0, "vc_dist": [0,0,0,0],
         "congested_pct": 0, "total_links": 0, "scoped_link_count": 0}
    if link_ids is not None and not link_ids:
        logs.append(f"机动车[{table_cn('road_way')}] 框选范围：0 条路段")
        return r
    link_filter = ""
    if link_ids is not None:
        ids_sql = ",".join(str(int(x)) for x in link_ids)
        link_filter = f" AND w.link_id IN ({ids_sql})"
        r["scoped_link_count"] = len(link_ids)
    try:
        _, rows = _q(conn, f"""
            SELECT w.type,
                   COUNT(*) AS cnt,
                   ROUND(SUM(w.length/1000.0)::numeric,2) AS len_km,
                   ROUND(AVG(w.volume)::numeric,1) AS avg_vol,
                   ROUND(SUM(w.volume * w.length / 1e6)::numeric,2) AS vmt,
                   ROUND(SUM(w.volume * w.length / 1e6 / NULLIF(w.speedlimit,0))::numeric,3) AS vht,
                   ROUND(AVG(w.v_c)::numeric,4) AS avg_vc,
                   ROUND(AVG(CASE WHEN w.v_c > 0 THEN w.speedlimit * (1 - w.v_c*0.3) ELSE w.speedlimit END)::numeric,1) AS avg_spd,
                   SUM(CASE WHEN w.v_c < 0.6  THEN 1 ELSE 0 END) AS va,
                   SUM(CASE WHEN w.v_c >= 0.6 AND w.v_c < 0.8 THEN 1 ELSE 0 END) AS vb,
                   SUM(CASE WHEN w.v_c >= 0.8 AND w.v_c < 1.0 THEN 1 ELSE 0 END) AS vc,
                   SUM(CASE WHEN w.v_c >= 1.0 THEN 1 ELSE 0 END) AS vd
            FROM {tbl} w
            WHERE w.type IS NOT NULL{link_filter}
            GROUP BY w.type ORDER BY w.type
        """)
        type_map = {1:'快速路', 2:'主干路', 3:'次干路', 4:'支路', 5:'其他'}
        total_vmt = total_vht = 0.0
        vc_dist = [0, 0, 0, 0]
        type_data = []
        for row in rows:
            rt, cnt, lkm, avg_vol, vmt, vht, avg_vc, avg_spd, va, vb, vc, vd = row
            label = type_map.get(int(rt), f'Type{rt}')
            vmt = _safe(vmt); vht = _safe(vht)
            type_data.append({
                "label": label, "count": int(cnt), "len_km": _safe(lkm),
                "avg_vol": _safe(avg_vol, 1), "vmt": vmt, "vht": vht,
                "avg_vc": _safe(avg_vc, 3), "avg_spd": _safe(avg_spd, 1),
            })
            total_vmt += vmt; total_vht += vht
            vc_dist[0] += int(va or 0); vc_dist[1] += int(vb or 0)
            vc_dist[2] += int(vc or 0); vc_dist[3] += int(vd or 0)
        total_links = sum(vc_dist)
        r.update({
            "ok": True, "type_data": type_data,
            "total_vmt": round(total_vmt, 2), "total_vht": round(total_vht, 3),
            "avg_vc": round(sum(d["avg_vc"]*d["count"] for d in type_data) / max(total_links,1), 3),
            "avg_speed": round(sum(d["avg_spd"]*d["count"] for d in type_data) / max(total_links,1), 1),
            "vc_dist": vc_dist, "total_links": total_links,
            "congested_pct": round((vc_dist[2]+vc_dist[3])/max(total_links,1)*100, 1),
        })
        logs.append(
            f"机动车：{table_cn('road_way')} VMT={total_vmt:.2f}，VHT={total_vht:.3f}，拥堵占比={r['congested_pct']}%"
        )
    except Exception as e:
        _pg_rollback(conn)
        logs.append(f"机动车统计失败（{table_cn('road_way')}）：{humanize_db_message(e)}")
    return r

def _slow_stats(conn, prefix, logs, link_ids: list[int] | None = None):
    """慢行网络统计：按路段类型汇总流量/PMT，连通性"""
    tbl_way  = _tbl(prefix, "slow_road_way")
    tbl_flow = _tbl(prefix, "slow_greedy_link_flow_results")
    r = {"ok": False, "type_data": [], "total_pmt": 0, "avg_flow": 0, "max_flow": 0,
         "network_len_km": 0, "high_flow_pct": 0, "scoped_link_count": 0}
    if link_ids is not None and not link_ids:
        logs.append(f"慢行[{table_cn('slow_road_way')}] 框选范围：0 条路段")
        return r
    link_filter = ""
    flow_filter = ""
    if link_ids is not None:
        ids_sql = ",".join(str(int(x)) for x in link_ids)
        link_filter = f" AND w.link_id IN ({ids_sql})"
        flow_filter = f" WHERE link_id IN ({ids_sql})"
        r["scoped_link_count"] = len(link_ids)
    try:
        _, rows = _q(conn, f"""
            SELECT w.type,
                   COUNT(*) AS cnt,
                   ROUND(SUM(w.length/1000.0)::numeric,2) AS len_km,
                   ROUND(AVG(f.flow)::numeric,1) AS avg_flow,
                   ROUND(MAX(f.flow)::numeric,1) AS max_flow,
                   ROUND(SUM(f.flow * w.length / 1e6)::numeric,2) AS pmt
            FROM {tbl_way} w
            LEFT JOIN {tbl_flow} f ON w.link_id = f.link_id
            WHERE w.type IS NOT NULL{link_filter}
            GROUP BY w.type ORDER BY w.type
        """)
        type_map = {1:'机动车道', 2:'步行道', 3:'非机动车道', 4:'混合慢行', 10:'其他'}
        type_data = []; total_pmt = 0.0; net_len = 0.0
        for row in rows:
            t, cnt, lkm, avg_fl, max_fl, pmt = row
            label = type_map.get(int(t), f'Type{t}')
            pmt = _safe(pmt); lkm = _safe(lkm)
            type_data.append({"label": label, "count": int(cnt), "len_km": lkm,
                               "avg_flow": _safe(avg_fl,1), "max_flow": _safe(max_fl,1), "pmt": pmt})
            total_pmt += pmt; net_len += lkm
        _, fr = _q(conn, f"SELECT ROUND(AVG(flow)::numeric,1), ROUND(MAX(flow)::numeric,1), COUNT(*), SUM(CASE WHEN flow>500 THEN 1 ELSE 0 END) FROM {tbl_flow}{flow_filter or ' WHERE flow IS NOT NULL'}{' AND flow IS NOT NULL' if flow_filter else ''}")
        gf = fr[0] if fr else (0, 0, 1, 0)
        high_pct = round(_safe(gf[3]) / max(int(gf[2] or 1), 1) * 100, 1)
        r.update({
            "ok": True, "type_data": type_data,
            "total_pmt": round(total_pmt, 2), "avg_flow": _safe(gf[0], 1),
            "max_flow": _safe(gf[1], 1), "network_len_km": round(net_len, 2),
            "high_flow_pct": high_pct,
        })
        logs.append(
            f"慢行：{table_cn('slow_road_way')} PMT={total_pmt:.2f}，平均流量={r['avg_flow']}，高流量占比={high_pct}%"
        )
    except Exception as e:
        _pg_rollback(conn)
        logs.append(f"慢行统计失败（{table_cn('slow_road_way')}）：{humanize_db_message(e)}")
    return r

def _pt_stats(conn, prefix, logs):
    """公交网络统计：线路/站点/断面客流"""
    r = {"ok": False, "route_count": 0, "stop_count": 0,
         "link_data": [], "enroute_avg": 0, "enroute_total": 0,
         "aboard_total": 0, "max_section_flow": 0}
    try:
        _, rr = _q(conn, f"SELECT COUNT(*) FROM {_tbl(prefix,'pt_route')}")
        _, sr = _q(conn, f"SELECT COUNT(*) FROM {_tbl(prefix,'pt_stop')}")
        route_cnt = int(rr[0][0]) if rr else 0
        stop_cnt  = int(sr[0][0]) if sr else 0
        _, lr = _q(conn, f"""
            SELECT link_type,
                   COUNT(*) AS cnt,
                   ROUND(AVG(flow)::numeric,1) AS avg_flow,
                   ROUND(MAX(flow)::numeric,1) AS max_flow,
                   ROUND(SUM(flow)::numeric,1) AS total_flow
            FROM {_tbl(prefix,'pt_link_result')}
            WHERE flow IS NOT NULL
            GROUP BY link_type ORDER BY link_type
        """)
        link_data = [{"type": str(row[0]), "count": int(row[1]),
                      "avg_flow": _safe(row[2],1), "max_flow": _safe(row[3],1),
                      "total_flow": _safe(row[4],1)} for row in lr]
        enroute = next((d for d in link_data if d["type"] == "ENROUTE"), {"avg_flow":0,"total_flow":0,"max_flow":0})
        aboard  = next((d for d in link_data if d["type"] == "ABOARD"),  {"total_flow":0})
        r.update({
            "ok": True, "route_count": route_cnt, "stop_count": stop_cnt,
            "link_data": link_data, "enroute_avg": enroute["avg_flow"],
            "enroute_total": enroute["total_flow"],
            "max_section_flow": enroute["max_flow"],
            "aboard_total": aboard["total_flow"],
        })
        logs.append(
            f"公交：{table_cn('pt_route')} {route_cnt} 条，{table_cn('pt_stop')} {stop_cnt} 个，断面均流={enroute['avg_flow']}"
        )
    except Exception as e:
        _pg_rollback(conn)
        logs.append(f"公交统计失败（{table_cn('pt_route')}）：{humanize_db_message(e)}")
    return r

# ── 综合评分 ─────────────────────────────────────────────────────────────────
# 雷达图维度（满分100分，越高越好）
# 机动车：拥堵率低→高分，速度高→高分，VMT效率
# 慢行：PMT高→高分，高峰流量覆盖
# 公交：线路利用率、断面客流均衡性

def _score_motor(m):
    if not m.get("ok"):
        return {"畅通度": 0, "网络效率": 0, "出行时间效益": 0, "碳排放效率": 0}
    cong_pct = m.get("congested_pct", 50)
    avg_vc   = m.get("avg_vc", 1.0)
    avg_spd  = m.get("avg_speed", 20)
    vmt      = m.get("total_vmt", 0)
    vht      = m.get("total_vht", 1)
    畅通度    = max(0, min(100, 100 - cong_pct * 2))
    网络效率  = max(0, min(100, (1 - avg_vc) * 120))
    出行时间效益 = max(0, min(100, (vmt / max(vht, 0.001)) / 1.5))  # 速度归一化
    碳排放效率 = max(0, min(100, 80 - cong_pct))  # 拥堵越少油耗越省
    return {"畅通度": round(畅通度,1), "网络效率": round(网络效率,1),
            "出行时间效益": round(出行时间效益,1), "碳排放效率": round(碳排放效率,1)}

def _score_slow(s):
    if not s.get("ok"):
        return {"慢行吸引力": 0, "路网覆盖效益": 0, "绿色出行贡献": 0}
    pmt    = s.get("total_pmt", 0)
    avgf   = s.get("avg_flow", 0)
    hpct   = s.get("high_flow_pct", 0)
    慢行吸引力 = max(0, min(100, min(pmt / 5.0, 1) * 100))
    路网覆盖效益 = max(0, min(100, min(avgf / 200, 1) * 100))
    绿色出行贡献 = max(0, min(100, hpct * 2))
    return {"慢行吸引力": round(慢行吸引力,1), "路网覆盖效益": round(路网覆盖效益,1),
            "绿色出行贡献": round(绿色出行贡献,1)}

def _score_pt(p):
    if not p.get("ok"):
        return {"公交服务水平": 0, "线路利用率": 0, "客流覆盖效益": 0}
    enr_avg = p.get("enroute_avg", 0)
    enr_tot = p.get("enroute_total", 0)
    aboard  = p.get("aboard_total", 0)
    公交服务水平 = max(0, min(100, min(enr_avg / 100, 1) * 100))
    线路利用率  = max(0, min(100, min(enr_tot / 50000, 1) * 100))
    客流覆盖效益 = max(0, min(100, min(aboard / 100000, 1) * 100))
    return {"公交服务水平": round(公交服务水平,1), "线路利用率": round(线路利用率,1),
            "客流覆盖效益": round(客流覆盖效益,1)}

# ── HTML 生成 ─────────────────────────────────────────────────────────────────

_CSS = """
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:"Times New Roman","宋体",serif;background:#f0f4f8;color:#1a1a2e;font-size:14px;line-height:1.6}
.hdr{background:linear-gradient(120deg,#0d2137 0%,#1b4a82 60%,#2471a3 100%);color:#fff;padding:22px 48px 18px}
.hdr h1{font-size:20px;font-weight:700;letter-spacing:.5px}
.section .sub{font-size:12px;color:#555;margin:0 0 12px}
.nav{background:#1b4a82;display:flex;gap:2px;padding:0 32px}
.nav a{color:#c8dff5;text-decoration:none;padding:8px 18px;font-size:12px;border-bottom:3px solid transparent;transition:.2s}
.nav a:hover,.nav a.active{color:#fff;border-bottom-color:#5dade2}
.wrap{max-width:1280px;margin:24px auto;padding:0 24px}
.section{background:#fff;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.08);padding:24px;margin-bottom:20px}
.sec-title{font-size:15px;font-weight:700;color:#1b4a82;border-left:4px solid #2471a3;padding-left:10px;margin-bottom:16px}
.badge-row{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:16px}
.badge{flex:1;min-width:140px;background:linear-gradient(135deg,#eaf2fb,#d6eaf8);border-radius:8px;padding:14px 18px;text-align:center}
.badge .lbl{font-size:11px;color:#5d6d7e;margin-bottom:4px}
.badge .val{font-size:22px;font-weight:700;color:#1b4a82}
.badge .unit{font-size:11px;color:#5d6d7e;margin-left:2px}
.badge.up .val{color:#27ae60}
.badge.dn .val{color:#e74c3c}
.badge.neu .val{color:#2471a3}
.chart-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(380px,1fr));gap:16px;margin-top:12px}
.chart-box{height:300px;border-radius:6px;background:#fafbfc;border:1px solid #eaecef}
.chart-box.wide{height:320px}
.chart-box.tall{height:380px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{background:#1b4a82;color:#fff;padding:8px 12px;text-align:center;font-weight:600}
td{padding:7px 12px;text-align:center;border-bottom:1px solid #eaecef}
tr:last-child td{border-bottom:none}
.imp{font-weight:700}
.up{color:#27ae60}.dn{color:#e74c3c}.neu{color:#2471a3}
footer{text-align:center;font-size:11px;color:#aaa;padding:20px 0 32px}
"""

def _echarts_cdn():
    cdn = "/tmp/echarts.min.js"
    if os.path.exists(cdn):
        try:
            with open(cdn, encoding="utf-8") as f:
                return "<script>" + f.read() + "</script>"
        except Exception:
            pass
    return '<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>'

def _delta_badge(label, base_val, new_val, unit, lower_better=False, fmt=".1f"):
    if base_val == 0 and new_val == 0:
        cls = "neu"
        delta_str = "—"
    else:
        delta = new_val - base_val
        pct = (delta / base_val * 100) if base_val != 0 else 0
        better = (delta < 0) if lower_better else (delta > 0)
        cls = "up" if better else ("dn" if delta != 0 else "neu")
        sign = "+" if delta > 0 else ""
        delta_str = f"{sign}{delta:{fmt}} ({sign}{pct:.1f}%)"
    nv = f"{new_val:{fmt}}"
    bv = f"{base_val:{fmt}}"
    return f'''<div class="badge {cls}">
  <div class="lbl">{label}</div>
  <div class="val">{nv}<span class="unit">{unit}</span></div>
  <div class="lbl" style="margin-top:4px">现状: {bv} &nbsp; 变化: {delta_str}</div>
</div>'''

def _kpi_badge(label, val, unit, cls="neu", fmt=".1f"):
    return f'''<div class="badge {cls}">
  <div class="lbl">{label}</div>
  <div class="val">{val:{fmt}}<span class="unit">{unit}</span></div>
</div>'''

# ── JS 图表生成 ───────────────────────────────────────────────────────────────

def _radar_js(base_scores, new_scores, base_label, new_label):
    dims = list(base_scores.keys()) + list(new_scores.keys())
    dims = list(dict.fromkeys(dims))
    bv = [base_scores.get(d, 0) for d in dims]
    nv = [new_scores.get(d, 0) for d in dims]
    indicator = json.dumps([{"name": d, "max": 100} for d in dims])
    bv_js = json.dumps(bv)
    nv_js = json.dumps(nv)
    bl_js = json.dumps(base_label)
    nl_js = json.dumps(new_label)
    return """(function(){
var _ec=function(id){return echarts.init(document.getElementById(id))};
var c=_ec('radar_chart');
c.setOption({
  tooltip:{trigger:'item',textStyle:{fontSize:12}},
  legend:{bottom:4,textStyle:{fontSize:12},data:[""" + bl_js + "," + nl_js + """]},
  grid:{top:10,bottom:50,left:40,right:40},
  radar:{indicator:""" + indicator + """,
    axisName:{fontSize:11,color:'#333'},
    splitLine:{lineStyle:{color:'#ddd'}},
    splitArea:{areaStyle:{color:['rgba(180,200,255,.08)','rgba(180,200,255,.04)']}}},
  series:[{type:'radar',
    data:[
      {value:""" + bv_js + """,name:""" + bl_js + """,
       lineStyle:{color:'#2980b9',width:2},
       areaStyle:{color:'rgba(41,128,185,.2)'},
       itemStyle:{color:'#2980b9'}},
      {value:""" + nv_js + """,name:""" + nl_js + """,
       lineStyle:{color:'#e67e22',width:2.5},
       areaStyle:{color:'rgba(230,126,34,.25)'},
       itemStyle:{color:'#e67e22'}}
    ],
    symbolSize:5}]});
window.addEventListener('resize',function(){c.resize()});
})();"""

def _bar_compare_js(chart_id, title, labels, base_vals, new_vals, base_label, new_label, unit=""):
    ljs = json.dumps(labels)
    bjs = json.dumps(base_vals)
    njs = json.dumps(new_vals)
    tjs = json.dumps(title)
    bljs = json.dumps(base_label + (f"({unit})" if unit else ""))
    nljs = json.dumps(new_label + (f"({unit})" if unit else ""))
    return "(function(){" + """
var _ec=function(id){return echarts.init(document.getElementById(id))};
var c=_ec('""" + chart_id + """');
c.setOption({
  title:{text:""" + tjs + """,left:'center',textStyle:{fontSize:13,fontWeight:'bold',color:'#1b4a82'}},
  tooltip:{trigger:'axis',axisPointer:{type:'shadow'},textStyle:{fontSize:12}},
  legend:{bottom:4,textStyle:{fontSize:11},data:[""" + bljs + "," + nljs + """]},
  grid:{top:44,bottom:50,left:48,right:20,containLabel:true},
  xAxis:{type:'category',data:""" + ljs + """,axisLabel:{fontSize:11,rotate:15}},
  yAxis:{type:'value',axisLabel:{fontSize:11},splitLine:{lineStyle:{type:'dashed',color:'#e0e0e0'}}},
  series:[
    {name:""" + bljs + """,type:'bar',data:""" + bjs + """,barGap:'10%',
     itemStyle:{color:'#2980b9'},barMaxWidth:36,label:{show:true,position:'top',fontSize:10}},
    {name:""" + nljs + """,type:'bar',data:""" + njs + """,
     itemStyle:{color:'#e67e22'},barMaxWidth:36,label:{show:true,position:'top',fontSize:10}}
  ]});
window.addEventListener('resize',function(){c.resize()});
})();"""

def _vc_dist_js(chart_id, base_dist, new_dist, base_label, new_label):
    lvls = ['畅通(V/C<0.6)', '基本畅通(0.6-0.8)', '轻度拥堵(0.8-1.0)', '严重拥堵(≥1.0)']
    colors = ['#27ae60', '#f39c12', '#e67e22', '#e74c3c']
    series = []
    for i, (lv, col) in enumerate(zip(lvls, colors)):
        series.append({
            "name": lv, "type": "bar",
            "stack": "base", "data": [base_dist[i]],
            "itemStyle": {"color": col + "aa"}
        })
        series.append({
            "name": lv, "type": "bar",
            "stack": "new", "data": [new_dist[i]],
            "itemStyle": {"color": col}
        })
    sjs = json.dumps(series)
    bljs = json.dumps(base_label)
    nljs = json.dumps(new_label)
    return "(function(){" + """
var _ec=function(id){return echarts.init(document.getElementById(id))};
var c=_ec('""" + chart_id + """');
c.setOption({
  title:{text:'V/C 拥堵分布对比',left:'center',textStyle:{fontSize:13,fontWeight:'bold',color:'#1b4a82'}},
  tooltip:{trigger:'axis',axisPointer:{type:'shadow'},textStyle:{fontSize:12}},
  legend:{bottom:2,textStyle:{fontSize:10}},
  grid:{top:44,bottom:52,left:60,right:20},
  xAxis:{type:'category',data:[""" + bljs + "," + nljs + """],axisLabel:{fontSize:12}},
  yAxis:{type:'value',name:'路段数',nameTextStyle:{fontSize:11},axisLabel:{fontSize:11},
         splitLine:{lineStyle:{type:'dashed',color:'#e0e0e0'}}},
  series:""" + sjs + """});
window.addEventListener('resize',function(){c.resize()});
})();"""

def _line_type_js(chart_id, title, type_labels, base_vals, new_vals, base_label, new_label, unit=""):
    ljs = json.dumps(type_labels)
    bjs = json.dumps(base_vals)
    njs = json.dumps(new_vals)
    tjs = json.dumps(title)
    bljs = json.dumps(base_label)
    nljs = json.dumps(new_label)
    ujs = json.dumps(unit)
    return "(function(){" + """
var _ec=function(id){return echarts.init(document.getElementById(id))};
var c=_ec('""" + chart_id + """');
c.setOption({
  title:{text:""" + tjs + """,left:'center',textStyle:{fontSize:13,fontWeight:'bold',color:'#1b4a82'}},
  tooltip:{trigger:'axis',textStyle:{fontSize:12}},
  legend:{bottom:4,textStyle:{fontSize:11},data:[""" + bljs + "," + nljs + """]},
  grid:{top:44,bottom:50,left:52,right:20,containLabel:true},
  xAxis:{type:'category',data:""" + ljs + """,axisLabel:{fontSize:11}},
  yAxis:{type:'value',name:""" + ujs + """,nameTextStyle:{fontSize:11},axisLabel:{fontSize:11},
         splitLine:{lineStyle:{type:'dashed',color:'#e0e0e0'}}},
  series:[
    {name:""" + bljs + """,type:'line',data:""" + bjs + """,
     lineStyle:{width:2,color:'#2980b9'},symbol:'circle',symbolSize:7,
     itemStyle:{color:'#2980b9'},label:{show:true,fontSize:10,position:'top'}},
    {name:""" + nljs + """,type:'line',data:""" + njs + """,
     lineStyle:{width:2.5,color:'#e67e22'},symbol:'diamond',symbolSize:8,
     itemStyle:{color:'#e67e22'},label:{show:true,fontSize:10,position:'top'}}
  ]});
window.addEventListener('resize',function(){c.resize()});
})();"""

def _waterfall_js(chart_id, title, items, values):
    ljs = json.dumps(items)
    vjs = json.dumps(values)
    colors = ['#2980b9' if v >= 0 else '#e74c3c' for v in values]
    cjs = json.dumps(colors)
    tjs = json.dumps(title)
    return "(function(){" + """
var _ec=function(id){return echarts.init(document.getElementById(id))};
var c=_ec('""" + chart_id + """');
var data=""" + vjs + """;
var colors=""" + cjs + """;
c.setOption({
  title:{text:""" + tjs + """,left:'center',textStyle:{fontSize:13,fontWeight:'bold',color:'#1b4a82'}},
  tooltip:{trigger:'axis',formatter:function(p){return p[0].name+': '+(p[0].value>=0?'+':'')+p[0].value.toFixed(1)},textStyle:{fontSize:12}},
  grid:{top:44,bottom:60,left:60,right:20,containLabel:true},
  xAxis:{type:'category',data:""" + ljs + """,axisLabel:{fontSize:11,rotate:20}},
  yAxis:{type:'value',axisLabel:{fontSize:11},
         splitLine:{lineStyle:{type:'dashed',color:'#e0e0e0'}},
         axisLine:{onZero:true}},
  series:[{type:'bar',data:data.map(function(v,i){return{value:v,itemStyle:{color:colors[i]}}}),
    barMaxWidth:48,label:{show:true,position:'top',fontSize:10,formatter:function(p){return(p.value>=0?'+':'')+p.value.toFixed(1)}}}]});
window.addEventListener('resize',function(){c.resize()});
})();"""

def _multi_radar_js(dims, scores_by_cid, scheme_ids, base_cid):
    indicator = json.dumps([{"name": d, "max": 100} for d in dims])
    data_parts = []
    legend_parts = []
    for i, scid in enumerate(scheme_ids):
        lbl = _scheme_label(scid, base_cid)
        legend_parts.append(json.dumps(lbl))
        vals = [scores_by_cid.get(scid, {}).get(d, 0) for d in dims]
        col = _SCHEME_COLORS[i % len(_SCHEME_COLORS)]
        w = 2.5 if scid == base_cid else 2
        data_parts.append(
            "{value:" + json.dumps(vals) + ",name:" + json.dumps(lbl)
            + ",lineStyle:{color:'" + col + "',width:" + str(w) + "}"
            + ",areaStyle:{color:'" + col + "33'}"
            + ",itemStyle:{color:'" + col + "'}}"
        )
    return """(function(){
var c=echarts.init(document.getElementById('radar_chart'));
c.setOption({
  tooltip:{trigger:'item',textStyle:{fontSize:12}},
  legend:{bottom:4,textStyle:{fontSize:11},data:[""" + ",".join(legend_parts) + """]},
  radar:{indicator:""" + indicator + """,
    axisName:{fontSize:11,color:'#333'},
    splitLine:{lineStyle:{color:'#ddd'}}},
  series:[{type:'radar',data:[""" + ",".join(data_parts) + """],symbolSize:5}]});
window.addEventListener('resize',function(){c.resize()});
})();"""


def _multi_bar_js(chart_id, title, labels, vals_by_cid, scheme_ids, base_cid, unit=""):
    ljs = json.dumps(labels)
    tjs = json.dumps(title)
    series_js = []
    legend_js = []
    for i, scid in enumerate(scheme_ids):
        lbl = _scheme_label(scid, base_cid) + (f"({unit})" if unit else "")
        legend_js.append(json.dumps(lbl))
        col = _SCHEME_COLORS[i % len(_SCHEME_COLORS)]
        vals = vals_by_cid.get(scid, [])
        series_js.append(
            "{name:" + json.dumps(lbl) + ",type:'bar',data:" + json.dumps(vals)
            + ",itemStyle:{color:'" + col + "'},barMaxWidth:28,"
            "label:{show:true,position:'top',fontSize:9}}"
        )
    return "(function(){" + """
var c=echarts.init(document.getElementById('""" + chart_id + """'));
c.setOption({
  title:{text:""" + tjs + """,left:'center',textStyle:{fontSize:13,fontWeight:'bold',color:'#1b4a82'}},
  tooltip:{trigger:'axis',axisPointer:{type:'shadow'}},
  legend:{bottom:2,textStyle:{fontSize:10},data:[""" + ",".join(legend_js) + """]},
  grid:{top:44,bottom:56,left:48,right:16,containLabel:true},
  xAxis:{type:'category',data:""" + ljs + """,axisLabel:{fontSize:10,rotate:18}},
  yAxis:{type:'value',axisLabel:{fontSize:10},splitLine:{lineStyle:{type:'dashed',color:'#e0e0e0'}}},
  series:[""" + ",".join(series_js) + """]});
window.addEventListener('resize',function(){c.resize()});
})();"""


def _multi_line_js(chart_id, title, labels, vals_by_cid, scheme_ids, base_cid, unit=""):
    ljs = json.dumps(labels)
    tjs = json.dumps(title)
    ujs = json.dumps(unit)
    series_js = []
    legend_js = []
    for i, scid in enumerate(scheme_ids):
        lbl = _scheme_label(scid, base_cid)
        legend_js.append(json.dumps(lbl))
        col = _SCHEME_COLORS[i % len(_SCHEME_COLORS)]
        vals = vals_by_cid.get(scid, [])
        series_js.append(
            "{name:" + json.dumps(lbl) + ",type:'line',data:" + json.dumps(vals)
            + ",lineStyle:{width:2,color:'" + col + "'},symbol:'circle',symbolSize:6,"
            "itemStyle:{color:'" + col + "'},label:{show:false}}"
        )
    return "(function(){" + """
var c=echarts.init(document.getElementById('""" + chart_id + """'));
c.setOption({
  title:{text:""" + tjs + """,left:'center',textStyle:{fontSize:13,fontWeight:'bold',color:'#1b4a82'}},
  tooltip:{trigger:'axis'},
  legend:{bottom:4,textStyle:{fontSize:10},data:[""" + ",".join(legend_js) + """]},
  grid:{top:44,bottom:52,left:52,right:16,containLabel:true},
  xAxis:{type:'category',data:""" + ljs + """,axisLabel:{fontSize:10}},
  yAxis:{type:'value',name:""" + ujs + """,axisLabel:{fontSize:10},
         splitLine:{lineStyle:{type:'dashed',color:'#e0e0e0'}}},
  series:[""" + ",".join(series_js) + """]});
window.addEventListener('resize',function(){c.resize()});
})();"""

# ── 报告组装 ─────────────────────────────────────────────────────────────────

def _build_html(pid, uid, scheme_ids, base_cid, stats_by_cid, cmp_type, logs, conn=None,
                project_name: str = "", gis_edit_log: dict | None = None):
    n_schemes = len(scheme_ids)
    base_m = stats_by_cid.get(base_cid, {}).get("motor", {"ok": False})
    base_s = stats_by_cid.get(base_cid, {}).get("slow", {"ok": False})
    base_p = stats_by_cid.get(base_cid, {}).get("pt", {"ok": False})
    compare_ids = [c for c in scheme_ids if c != base_cid]
    new_cid = compare_ids[0] if len(compare_ids) == 1 else (compare_ids[-1] if compare_ids else base_cid)
    new_m = stats_by_cid.get(new_cid, {}).get("motor", {"ok": False})
    new_s = stats_by_cid.get(new_cid, {}).get("slow", {"ok": False})
    new_p = stats_by_cid.get(new_cid, {}).get("pt", {"ok": False})

    base_label = _scheme_label(base_cid, base_cid)
    new_label = _scheme_label(new_cid, base_cid)
    scheme_sub = "、".join(f"Case{c}" for c in scheme_ids)
    title = f"{project_name or f'项目 {pid}'} — 方案对比分析报告（用户 {uid}）"
    data_src = data_sources_line(tables_for_scheme_compare(cmp_type))
    cmp_type_cn = type_cn(cmp_type)
    multi_mode = n_schemes > 2

    # ── KPI 徽章 ──────────────────────────────────────────────────────────────
    motor_badges = slow_badges = pt_badges = ""
    if cmp_type in ("motor", "all") and new_m.get("ok") and base_m.get("ok"):
        motor_badges = (
            _delta_badge("总VMT", base_m["total_vmt"], new_m["total_vmt"], "万pcu·km", lower_better=False, fmt=".2f") +
            _delta_badge("总VHT", base_m["total_vht"], new_m["total_vht"], "万pcu·h", lower_better=True, fmt=".3f") +
            _delta_badge("平均V/C", base_m["avg_vc"], new_m["avg_vc"], "", lower_better=True, fmt=".3f") +
            _delta_badge("平均速度", base_m["avg_speed"], new_m["avg_speed"], "km/h", lower_better=False, fmt=".1f") +
            _delta_badge("拥堵路段率", base_m["congested_pct"], new_m["congested_pct"], "%", lower_better=True, fmt=".1f")
        )
    elif cmp_type in ("motor", "all") and new_m.get("ok"):
        motor_badges = (
            _kpi_badge("总VMT", new_m["total_vmt"], "万pcu·km", fmt=".2f") +
            _kpi_badge("总VHT", new_m["total_vht"], "万pcu·h", fmt=".3f") +
            _kpi_badge("平均V/C", new_m["avg_vc"], "", fmt=".3f") +
            _kpi_badge("拥堵路段率", new_m["congested_pct"], "%", fmt=".1f")
        )

    if cmp_type in ("slow", "all") and new_s.get("ok") and base_s.get("ok"):
        slow_badges = (
            _delta_badge("慢行PMT", base_s["total_pmt"], new_s["total_pmt"], "万人·km", lower_better=False, fmt=".2f") +
            _delta_badge("平均流量", base_s["avg_flow"], new_s["avg_flow"], "人次/h", lower_better=False, fmt=".1f") +
            _delta_badge("峰值流量", base_s["max_flow"], new_s["max_flow"], "人次/h", lower_better=False, fmt=".1f") +
            _delta_badge("高流量路段占比", base_s["high_flow_pct"], new_s["high_flow_pct"], "%", lower_better=False, fmt=".1f")
        )
    elif cmp_type in ("slow", "all") and new_s.get("ok"):
        slow_badges = (
            _kpi_badge("慢行PMT", new_s["total_pmt"], "万人·km", fmt=".2f") +
            _kpi_badge("平均流量", new_s["avg_flow"], "人次/h", fmt=".1f") +
            _kpi_badge("路网长度", new_s["network_len_km"], "km", fmt=".1f")
        )

    if cmp_type in ("pt", "all") and new_p.get("ok") and base_p.get("ok"):
        pt_badges = (
            _delta_badge("线路数", base_p["route_count"], new_p["route_count"], "条", lower_better=False, fmt=".0f") +
            _delta_badge("站点数", base_p["stop_count"], new_p["stop_count"], "个", lower_better=False, fmt=".0f") +
            _delta_badge("断面平均客流", base_p["enroute_avg"], new_p["enroute_avg"], "人次/班", lower_better=False, fmt=".1f") +
            _delta_badge("最大断面客流", base_p["max_section_flow"], new_p["max_section_flow"], "人次/班", lower_better=False, fmt=".1f")
        )
    elif cmp_type in ("pt", "all") and new_p.get("ok"):
        pt_badges = (
            _kpi_badge("线路数", new_p["route_count"], "条", fmt=".0f") +
            _kpi_badge("站点数", new_p["stop_count"], "个", fmt=".0f") +
            _kpi_badge("断面平均客流", new_p["enroute_avg"], "人次/班", fmt=".1f")
        )

    # ── 雷达图数据 ─────────────────────────────────────────────────────────────
    base_scores_all: dict = {}
    scores_by_cid: dict[int, dict] = {}
    for scid in scheme_ids:
        sm = stats_by_cid.get(scid, {}).get("motor", {})
        ss = stats_by_cid.get(scid, {}).get("slow", {})
        sp = stats_by_cid.get(scid, {}).get("pt", {})
        sc: dict = {}
        if cmp_type in ("motor", "all"):
            sc.update(_score_motor(sm))
        if cmp_type in ("slow", "all"):
            sc.update(_score_slow(ss))
        if cmp_type in ("pt", "all"):
            sc.update(_score_pt(sp))
        scores_by_cid[scid] = sc
        if scid == base_cid:
            base_scores_all = dict(sc)
    new_scores_all = scores_by_cid.get(new_cid, {}) if not multi_mode else {}

    # ── 图表 JS ────────────────────────────────────────────────────────────────
    chart_js_parts = []

    dims = list(dict.fromkeys(
        list(base_scores_all.keys()) + [k for sc in scores_by_cid.values() for k in sc]
    ))

    if multi_mode and dims:
        chart_js_parts.append(_multi_radar_js(dims, scores_by_cid, scheme_ids, base_cid))
    elif base_scores_all and new_scores_all:
        chart_js_parts.append(_radar_js(base_scores_all, new_scores_all, base_label, new_label))

    # 2. 机动车
    if cmp_type in ("motor", "all") and any(
        stats_by_cid.get(c, {}).get("motor", {}).get("ok") for c in scheme_ids
    ):
        all_labels = []
        for scid in scheme_ids:
            sm = stats_by_cid.get(scid, {}).get("motor", {})
            for d in sm.get("type_data", []):
                if d["label"] not in all_labels:
                    all_labels.append(d["label"])
        if multi_mode and all_labels:
            vmt_by = {}; vht_by = {}; vc_by = {}; spd_by = {}
            for scid in scheme_ids:
                sm = stats_by_cid.get(scid, {}).get("motor", {})
                m = {d["label"]: d for d in sm.get("type_data", [])}
                vmt_by[scid] = [m.get(l, {}).get("vmt", 0) for l in all_labels]
                vht_by[scid] = [m.get(l, {}).get("vht", 0) for l in all_labels]
                vc_by[scid] = [m.get(l, {}).get("avg_vc", 0) for l in all_labels]
                spd_by[scid] = [m.get(l, {}).get("avg_spd", 0) for l in all_labels]
            chart_js_parts.append(_multi_bar_js(
                "motor_vmt", "各等级道路VMT对比", all_labels, vmt_by, scheme_ids, base_cid, "万pcu·km"))
            chart_js_parts.append(_multi_bar_js(
                "motor_vht", "各等级道路VHT对比", all_labels, vht_by, scheme_ids, base_cid, "万pcu·h"))
            chart_js_parts.append(_multi_line_js(
                "motor_vc", "各等级道路平均V/C对比", all_labels, vc_by, scheme_ids, base_cid, "V/C"))
            chart_js_parts.append(_multi_line_js(
                "motor_spd", "各等级道路平均速度对比", all_labels, spd_by, scheme_ids, base_cid, "km/h"))
        elif not multi_mode and (new_m.get("ok") or base_m.get("ok")):
            bm_td = base_m.get("type_data", [])
            nm_td = new_m.get("type_data", [])
            all_labels = list(dict.fromkeys([d["label"] for d in bm_td] + [d["label"] for d in nm_td]))
            bm_map = {d["label"]: d for d in bm_td}
            nm_map = {d["label"]: d for d in nm_td}
            vmt_b = [bm_map.get(l, {}).get("vmt", 0) for l in all_labels]
            vmt_n = [nm_map.get(l, {}).get("vmt", 0) for l in all_labels]
            vht_b = [bm_map.get(l, {}).get("vht", 0) for l in all_labels]
            vht_n = [nm_map.get(l, {}).get("vht", 0) for l in all_labels]
            vc_b = [bm_map.get(l, {}).get("avg_vc", 0) for l in all_labels]
            vc_n = [nm_map.get(l, {}).get("avg_vc", 0) for l in all_labels]
            spd_b = [bm_map.get(l, {}).get("avg_spd", 0) for l in all_labels]
            spd_n = [nm_map.get(l, {}).get("avg_spd", 0) for l in all_labels]
            chart_js_parts.append(_bar_compare_js("motor_vmt", "各等级道路VMT对比", all_labels, vmt_b, vmt_n, base_label, new_label, "万pcu·km"))
            chart_js_parts.append(_bar_compare_js("motor_vht", "各等级道路VHT对比", all_labels, vht_b, vht_n, base_label, new_label, "万pcu·h"))
            chart_js_parts.append(_line_type_js("motor_vc", "各等级道路平均V/C对比", all_labels, vc_b, vc_n, base_label, new_label, "V/C"))
            chart_js_parts.append(_line_type_js("motor_spd", "各等级道路平均速度对比", all_labels, spd_b, spd_n, base_label, new_label, "km/h"))
            if base_m.get("vc_dist") and new_m.get("vc_dist"):
                chart_js_parts.append(_vc_dist_js("motor_vcdist", base_m["vc_dist"], new_m["vc_dist"], base_label, new_label))

    # 3. 慢行
    if cmp_type in ("slow", "all") and any(
        stats_by_cid.get(c, {}).get("slow", {}).get("ok") for c in scheme_ids
    ):
        if multi_mode:
            s_labels = []
            for scid in scheme_ids:
                for d in stats_by_cid.get(scid, {}).get("slow", {}).get("type_data", []):
                    if d["label"] not in s_labels:
                        s_labels.append(d["label"])
            if s_labels:
                pmt_by = {}; af_by = {}
                for scid in scheme_ids:
                    m = {d["label"]: d for d in stats_by_cid.get(scid, {}).get("slow", {}).get("type_data", [])}
                    pmt_by[scid] = [m.get(l, {}).get("pmt", 0) for l in s_labels]
                    af_by[scid] = [m.get(l, {}).get("avg_flow", 0) for l in s_labels]
                chart_js_parts.append(_multi_bar_js(
                    "slow_pmt", "各类型慢行路段PMT对比", s_labels, pmt_by, scheme_ids, base_cid, "万人·km"))
                chart_js_parts.append(_multi_line_js(
                    "slow_flow", "各类型慢行路段平均流量对比", s_labels, af_by, scheme_ids, base_cid, "人次/h"))
        elif new_s.get("ok") or base_s.get("ok"):
            bs_td = base_s.get("type_data", [])
            ns_td = new_s.get("type_data", [])
            s_labels = list(dict.fromkeys([d["label"] for d in bs_td] + [d["label"] for d in ns_td]))
            bs_map = {d["label"]: d for d in bs_td}
            ns_map = {d["label"]: d for d in ns_td}
            pmt_b = [bs_map.get(l, {}).get("pmt", 0) for l in s_labels]
            pmt_n = [ns_map.get(l, {}).get("pmt", 0) for l in s_labels]
            af_b = [bs_map.get(l, {}).get("avg_flow", 0) for l in s_labels]
            af_n = [ns_map.get(l, {}).get("avg_flow", 0) for l in s_labels]
            chart_js_parts.append(_bar_compare_js("slow_pmt", "各类型慢行路段PMT对比", s_labels, pmt_b, pmt_n, base_label, new_label, "万人·km"))
            chart_js_parts.append(_line_type_js("slow_flow", "各类型慢行路段平均流量对比", s_labels, af_b, af_n, base_label, new_label, "人次/h"))

    # 4. 公交
    if cmp_type in ("pt", "all") and any(
        stats_by_cid.get(c, {}).get("pt", {}).get("ok") for c in scheme_ids
    ):
        if multi_mode:
            p_labels = []
            for scid in scheme_ids:
                for d in stats_by_cid.get(scid, {}).get("pt", {}).get("link_data", []):
                    if d["type"] not in p_labels:
                        p_labels.append(d["type"])
            if p_labels:
                af_by = {}; tf_by = {}
                for scid in scheme_ids:
                    m = {d["type"]: d for d in stats_by_cid.get(scid, {}).get("pt", {}).get("link_data", [])}
                    af_by[scid] = [m.get(l, {}).get("avg_flow", 0) for l in p_labels]
                    tf_by[scid] = [m.get(l, {}).get("total_flow", 0) for l in p_labels]
                chart_js_parts.append(_multi_bar_js(
                    "pt_avgflow", "公交各链路类型平均客流对比", p_labels, af_by, scheme_ids, base_cid, "人次/班"))
                chart_js_parts.append(_multi_bar_js(
                    "pt_totflow", "公交各链路类型总客流对比", p_labels, tf_by, scheme_ids, base_cid, "人次"))
        elif new_p.get("ok") or base_p.get("ok"):
            bp_ld = base_p.get("link_data", [])
            np_ld = new_p.get("link_data", [])
            p_labels = list(dict.fromkeys([d["type"] for d in bp_ld] + [d["type"] for d in np_ld]))
            bp_map = {d["type"]: d for d in bp_ld}
            np_map = {d["type"]: d for d in np_ld}
            af_pb = [bp_map.get(l, {}).get("avg_flow", 0) for l in p_labels]
            af_pn = [np_map.get(l, {}).get("avg_flow", 0) for l in p_labels]
            tf_pb = [bp_map.get(l, {}).get("total_flow", 0) for l in p_labels]
            tf_pn = [np_map.get(l, {}).get("total_flow", 0) for l in p_labels]
            chart_js_parts.append(_bar_compare_js("pt_avgflow", "公交各链路类型平均客流对比", p_labels, af_pb, af_pn, base_label, new_label, "人次/班"))
            chart_js_parts.append(_bar_compare_js("pt_totflow", "公交各链路类型总客流对比", p_labels, tf_pb, tf_pn, base_label, new_label, "人次"))

    # 5. 效益瀑布图（仅两方案）或相对基准表（多方案）
    waterfall_items = []; waterfall_vals = []
    if not multi_mode and cmp_type in ("motor", "all") and new_m.get("ok") and base_m.get("ok"):
        waterfall_items.append("VMT变化\n(万pcu·km)")
        waterfall_vals.append(round(new_m["total_vmt"] - base_m["total_vmt"], 2))
        waterfall_items.append("VHT节省\n(万pcu·h)")
        waterfall_vals.append(round(base_m["total_vht"] - new_m["total_vht"], 3))
        waterfall_items.append("拥堵路段\n减少数")
        waterfall_vals.append(round(
            (base_m["vc_dist"][2]+base_m["vc_dist"][3]) - (new_m["vc_dist"][2]+new_m["vc_dist"][3]), 0))
    if cmp_type in ("slow", "all") and new_s.get("ok") and base_s.get("ok"):
        waterfall_items.append("慢行PMT\n变化(万人·km)")
        waterfall_vals.append(round(new_s["total_pmt"] - base_s["total_pmt"], 2))
    if cmp_type in ("pt", "all") and new_p.get("ok") and base_p.get("ok"):
        waterfall_items.append("断面客流\n变化(人次/班)")
        waterfall_vals.append(round(new_p["enroute_avg"] - base_p["enroute_avg"], 1))
    if waterfall_items:
        chart_js_parts.append(_waterfall_js("benefit_waterfall", "改造效益综合变化量", waterfall_items, waterfall_vals))

    chart_js = "\n".join(chart_js_parts)

    multi_delta_rows = ""
    if multi_mode and base_m.get("ok"):
        for scid in compare_ids:
            sm = stats_by_cid.get(scid, {}).get("motor", {})
            if not sm.get("ok"):
                continue
            dv = sm.get("total_vmt", 0) - base_m.get("total_vmt", 0)
            multi_delta_rows += (
                f"<tr><td class='imp'>Case{scid}</td><td>{sm.get('total_vmt',0):.2f}</td>"
                f"<td class='{'up' if dv>0 else 'dn'}'>{dv:+.2f}</td>"
                f"<td>{sm.get('avg_vc',0):.3f}</td></tr>"
            )

    # ── 各专项图表区 HTML ──────────────────────────────────────────────────────
    def chart_section(sec_id, title_text, chart_ids, extra_html=""):
        boxes = "".join(f'<div class="chart-box" id="{cid}"></div>' for cid in chart_ids)
        return f'''<div id="{sec_id}" class="section">
  <div class="sec-title">{title_text}</div>
  {extra_html}
  <div class="chart-grid">{boxes}</div>
</div>'''

    sections = []

    gis_section_html = render_gis_edit_log_section(gis_edit_log)
    if gis_section_html:
        sections.append(gis_section_html)

    # 综合评分雷达
    if base_scores_all or scores_by_cid:
        if multi_mode and dims:
            hdr = "".join(f"<th>{_scheme_label(c, base_cid)}</th>" for c in scheme_ids)
            body_rows = ""
            for d in dims:
                cells = "".join(
                    f"<td>{scores_by_cid.get(c, {}).get(d, 0):.1f}</td>" for c in scheme_ids
                )
                body_rows += f"<tr><td class='imp'>{d}</td>{cells}</tr>"
            sections.append(f'''<div id="overview" class="section">
  <div class="sec-title">综合评分对比（雷达图 · {n_schemes} 方案）</div>
  <div style="display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap">
    <div class="chart-box tall" id="radar_chart" style="flex:1;min-width:320px;max-width:600px"></div>
    <div style="flex:1;min-width:280px;overflow:auto">
      <table><thead><tr><th>维度</th>{hdr}</tr></thead><tbody>{body_rows}</tbody></table>
    </div>
  </div>
</div>''')
            if multi_delta_rows:
                sections.append(f'''<div class="section"><div class="sec-title">机动车 VMT 相对基准 (Case{base_cid})</div>
<table><thead><tr><th>方案</th><th>VMT</th><th>相对基准</th><th>平均V/C</th></tr></thead>
<tbody>{multi_delta_rows}</tbody></table></div>''')
        elif base_scores_all:
            sections.append(f'''<div id="overview" class="section">
  <div class="sec-title">综合评分对比（雷达图）</div>
  <div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap">
    <div class="chart-box tall" id="radar_chart" style="flex:1;min-width:320px;max-width:560px"></div>
    <div style="flex:1;min-width:260px">
      <table><thead><tr><th>维度</th><th>{base_label}</th><th>{new_label}</th><th>变化</th></tr></thead><tbody>'''
            + "".join(f'<tr><td class="imp">{d}</td>'
                      f'<td>{base_scores_all.get(d,0):.1f}</td>'
                      f'<td class="{"up" if new_scores_all.get(d,0)>=base_scores_all.get(d,0) else "dn"}">{new_scores_all.get(d,0):.1f}</td>'
                      f'<td class="{"up" if new_scores_all.get(d,0)>=base_scores_all.get(d,0) else "dn"}">'
                      f'{"+" if new_scores_all.get(d,0)>=base_scores_all.get(d,0) else ""}'
                      f'{new_scores_all.get(d,0)-base_scores_all.get(d,0):.1f}</td></tr>'
                      for d in base_scores_all)
            + f'''</tbody></table>
    </div>
  </div>
</div>''')

    # 机动车专项
    if motor_badges or cmp_type in ("motor", "all"):
        motor_chart_ids = []
        if cmp_type in ("motor", "all") and (new_m.get("ok") or base_m.get("ok")):
            motor_chart_ids = ["motor_vmt", "motor_vht", "motor_vc", "motor_spd"]
            if base_m.get("vc_dist") and new_m.get("vc_dist"):
                motor_chart_ids.append("motor_vcdist")
        badge_html = f'<div class="badge-row">{motor_badges}</div>' if motor_badges else ""
        _boxes = "".join('<div class="chart-box" id="' + c + '"></div>' for c in motor_chart_ids)
        sections.append('<div id="motor_sec" class="section"><div class="sec-title">机动车交通对比</div>'
                        + badge_html + '<div class="chart-grid">' + _boxes + '</div></div>')

    # 慢行专项
    if slow_badges or cmp_type in ("slow", "all"):
        slow_chart_ids = []
        if cmp_type in ("slow", "all") and (new_s.get("ok") or base_s.get("ok")):
            slow_chart_ids = ["slow_pmt", "slow_flow"]
        badge_html = f'<div class="badge-row">{slow_badges}</div>' if slow_badges else ""
        _boxes = "".join('<div class="chart-box" id="' + c + '"></div>' for c in slow_chart_ids)
        sections.append('<div id="slow_sec" class="section"><div class="sec-title">慢行交通对比</div>'
                        + badge_html + '<div class="chart-grid">' + _boxes + '</div></div>')

    # 公交专项
    if pt_badges or cmp_type in ("pt", "all"):
        pt_chart_ids = []
        if cmp_type in ("pt", "all") and (new_p.get("ok") or base_p.get("ok")):
            pt_chart_ids = ["pt_avgflow", "pt_totflow"]
        badge_html = f'<div class="badge-row">{pt_badges}</div>' if pt_badges else ""
        _boxes = "".join('<div class="chart-box" id="' + c + '"></div>' for c in pt_chart_ids)
        sections.append('<div id="pt_sec" class="section"><div class="sec-title">公共交通对比</div>'
                        + badge_html + '<div class="chart-grid">' + _boxes + '</div></div>')

    # 效益综合
    if waterfall_items:
        sections.append(f'<div id="benefit_sec" class="section"><div class="sec-title">综合效益变化量</div>'
                        + '<div class="chart-grid"><div class="chart-box wide" id="benefit_waterfall"></div></div></div>')

    map_extra_head = ""
    map_extra_css = ""
    map_section_html = ""
    map_js = ""
    if conn is not None and cmp_type in ("motor", "all"):
        try:
            _pg_rollback(conn)
            map_candidates = [new_cid, base_cid] + [
                c for c in scheme_ids if c not in (new_cid, base_cid)
            ] + [3]
            map_bundle, map_meta = collect_map_bundle_for_cases(
                conn, pid, uid, map_candidates, logs
            )
            map_cid = map_meta.get("map_case_id", new_cid)
            map_note = map_meta.get("map_note") or ""
            pipeline = map_meta.get("pipeline") or {}
            mr = render_map_sections(map_bundle, pipeline=pipeline)
            map_extra_head = mr.get("leaflet_head") or ""
            map_extra_css = mr.get("css") or ""
            title_line = f"对比方案 Case{new_cid} 路网流量与 OD 期望线"
            if map_note:
                title_line += f"（{map_note}）"
            elif int(map_cid) != int(new_cid):
                title_line = (
                    f"路网流量与 OD 期望线（示意 case{map_cid}；"
                    f"对比主方案 case{new_cid} 无几何）"
                )
            map_section_html = (mr.get("html") or "").replace(
                "空间分布：路网流量与 OD 期望线",
                title_line,
            )
            map_js = mr.get("js") or ""
        except Exception as exc:
            _pg_rollback(conn)
            logs.append(f"地图采集失败: {exc}")
    if map_section_html:
        insert_at = 1 if sections else 0
        sections.insert(insert_at, map_section_html)

    # 日志
    log_rows = "".join(f"<tr><td style='text-align:left;font-family:monospace;font-size:11px'>{l}</td></tr>" for l in logs[-20:])
    sections.append(f'<div class="section"><div class="sec-title">计算日志</div><table>{log_rows}</table></div>')

    nav_links = '<a href="#overview" class="active">综合评分</a>'
    if gis_section_html:
        nav_links = '<a href="#gis_edit_sec" class="active">GIS修改</a>' + nav_links.replace(' class="active"', '', 1)
    if map_section_html:
        nav_links += '<a href="#s_maps">空间分布</a>'
    nav_links += (
        '<a href="#motor_sec">机动车</a>'
        '<a href="#slow_sec">慢行</a>'
        '<a href="#pt_sec">公交</a>'
        '<a href="#benefit_sec">效益汇总</a>'
    )

    map_js_tag = f"<script>{map_js}</script>" if map_js else ""

    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
{map_extra_head}
<style>{_CSS}{map_extra_css}</style>
</head>
<body>
<div class="hdr">
  <h1>{title}</h1>
  <div class="sub">项目名：{project_name or f'项目 {pid}'} &nbsp;|&nbsp; 项目 ID：{pid} &nbsp;|&nbsp; 用户 {uid} &nbsp;|&nbsp; 对比方案：{scheme_sub} &nbsp;|&nbsp; 基准：Case {base_cid} &nbsp;|&nbsp; 对比维度：{cmp_type_cn} &nbsp;|&nbsp; 共 {n_schemes} 个方案<br>{data_src}</div>
</div>
<nav class="nav">{nav_links}</nav>
<div class="wrap">
{"".join(sections)}
</div>
<footer>{project_name or f'项目 {pid}'} · 方案对比分析报告 &nbsp;·&nbsp; 城市交通网络预测与分析平台</footer>
{_echarts_cdn()}
<script>
document.addEventListener('DOMContentLoaded', function() {{
{chart_js}
}});
</script>
{map_js_tag}
</body>
</html>"""

# ── 主流程 ────────────────────────────────────────────────────────────────────
def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--request-file")
    parser.add_argument("--response-file")
    args, _ = parser.parse_known_args()

    req_path  = args.request_file  or os.environ.get("TNA_INPUT_FILE",  "")
    resp_path = args.response_file or os.environ.get("TNA_OUTPUT_FILE", "")
    if not req_path or not resp_path:
        print("[scheme_compare_bridge] missing --request-file / --response-file", file=sys.stderr)
        sys.exit(1)

    kv = _read_kv(req_path)
    pid = int(kv.get("project_id", 0))
    uid = int(kv.get("user_id", 0))

    p2, raw_param2 = _parse_scheme_param2(kv)

    try:
        scheme_ids, base_cid = _resolve_scheme_ids(kv, p2)
    except ValueError as e:
        if "缺少方案 ID" in str(e) and raw_param2.strip():
            _fail(
                resp_path,
                f"{e}（param2 解析可能失败，请检查 JSON 格式；原始长度={len(raw_param2)}）",
            )
        _fail(resp_path, str(e))

    cmp_type = str(p2.get("type", "all")).lower()
    if cmp_type not in ("motor", "slow", "pt", "all"):
        cmp_type = "all"
    ids_tag = "-".join(str(i) for i in scheme_ids)
    out_path = resolve_report_output_path(
        p2,
        report_stem=f"scheme_compare_cases_{ids_tag}",
        fallback_path=f"/tmp/tna_compare_report_{pid}_{uid}_cases_{ids_tag}.html",
    )

    if pid <= 0 or uid <= 0:
        _fail(resp_path, "project_id and user_id must be > 0")

    logs = []
    db_conf_path = os.environ.get("TNA_DB_CONF", "/opt/algorithms/db.conf")

    try:
        cfg = _load_db_conf(db_conf_path)
        if not cfg:
            _fail(resp_path, f"无法读取数据库配置: {db_conf_path}")
        conn = _connect(cfg)
    except Exception as e:
        _fail(resp_path, f"数据库连接失败: {e}")

    empty = {"ok": False}
    stats_by_cid: dict[int, dict] = {}
    link_ids_by_case: dict[int, list[int]] = {}
    bbox_norm = normalize_bbox_param(extract_bbox_from_param2(p2))
    scope_active = bbox_norm is not None

    try:
        for scid in scheme_ids:
            pfx = _prefix(pid, uid, scid)
            motor_links: list[int] | None = None
            slow_links: list[int] | None = None
            if scope_active:
                motor_links = resolve_motor_link_ids_by_bbox(
                    conn, _tbl(pfx, "road_way"), bbox_norm, logs=logs
                )
                link_ids_by_case[scid] = motor_links
                slow_links = motor_links
            stats_by_cid[scid] = {"motor": empty, "slow": empty, "pt": empty}
            if cmp_type in ("motor", "all"):
                stats_by_cid[scid]["motor"] = _motor_stats(conn, pfx, logs, motor_links)
            if cmp_type in ("slow", "all"):
                stats_by_cid[scid]["slow"] = _slow_stats(conn, pfx, logs, slow_links)
            if cmp_type in ("pt", "all"):
                stats_by_cid[scid]["pt"] = _pt_stats(conn, pfx, logs)
    except Exception as e:
        logs.append(f"[ERROR] {e}")
        logs.append(traceback.format_exc()[-300:])

    html = _build_html(pid, uid, scheme_ids, base_cid, stats_by_cid, cmp_type, logs, conn=conn,
                     project_name=project_display_name(p2, pid),
                     gis_edit_log=collect_gis_edit_log(p2))

    try:
        conn.close()
    except Exception:
        pass
    try:
        Path(out_path).write_text(html, encoding="utf-8")
    except Exception as e:
        _fail(resp_path, f"写 HTML 失败: {e}")

    _success(
        resp_path,
        out_path,
        scheme_ids,
        base_cid,
        extra={
            "compare_scope_bbox": bbox_norm,
            "link_ids_by_case": link_ids_by_case,
            "link_count_by_case": {str(k): len(v) for k, v in link_ids_by_case.items()},
        }
        if scope_active
        else None,
    )

if __name__ == "__main__":
    main()
