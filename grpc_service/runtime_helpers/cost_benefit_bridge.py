#!/usr/bin/env python3
"""
cost_benefit_bridge.py
成本效益分析报告 Python Bridge

入参（通过环境变量 KV 文件传递，与 base_report_bridge.py 完全相同的协议）：
  project_id, user_id, case_id
  param2 (JSON)：
    type         str  必填  "motor" / "slow" / "pt"
    base_case_id int  可选  现状方案 case_id；与 case_id 不同且有效时做对比；未传或与 case_id 相同则仅分析 case_id 单方案
    project_name str  可选  **param2 内**项目名称；缺省 output_path 时用于文件名前缀与页眉展示
    output_path  str  可选  HTML 输出路径

出参（写回 KV 文件）：
  code=1 / -1
  data.attributes.html_path
"""
import os, sys, json, math, traceback
from pathlib import Path

try:
    from .report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
    from .table_name_cn import data_sources_line, tables_for_cba, type_cn
except ImportError:
    from report_output_utils import resolve_report_output_path, project_display_name, parse_report_param2
    from table_name_cn import data_sources_line, tables_for_cba, type_cn

try:
    from .network_map_collect import collect_map_bundle_for_cases
    from .network_map_report import render_map_sections
except ImportError:
    from network_map_collect import collect_map_bundle_for_cases
    from network_map_report import render_map_sections

try:
    from .indicator_report_meta import CBA_MOTOR_GLOSSARY
except ImportError:
    from indicator_report_meta import CBA_MOTOR_GLOSSARY

# ── KV 协议 ──────────────────────────────────────────────────────────────────
def _read_kv(path: str) -> dict:
    kv = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
    return kv

def _write_kv(path: str, d: dict):
    with open(path, "w", encoding="utf-8") as f:
        for k, v in d.items():
            f.write(f"{k}={v}\n")

def _fail(out_path: str, msg: str):
    _write_kv(out_path, {"code": "-1", "message": msg})
    sys.exit(0)

def _success(out_path: str, html_path: str, *, has_compare: bool, cid: int, base_cid: int | None, logs: list[str]):
    if has_compare and base_cid is not None:
        msg = f"成本效益分析报告生成成功（对比：改造 case{cid} vs 现状 case{base_cid}）"
    else:
        msg = f"成本效益分析报告生成成功（单方案 case{cid}）"
    kv = {
        "code": "1",
        "message": msg,
        "data.attributes.html_path": html_path,
        "summary.stage": "cost_benefit_report",
    }
    if logs:
        kv["summary.logs"] = "\n".join(logs[-30:])
    _write_kv(out_path, kv)

def _load_echarts_script() -> str:
    for p in (
        "/tmp/echarts.min.js",
        os.path.join(os.path.dirname(__file__), "echarts.min.js"),
    ):
        if os.path.isfile(p):
            with open(p, "r", encoding="utf-8") as f:
                return f.read()
    return ""

# ── DB 工具 ──────────────────────────────────────────────────────────────────
def _load_db_conf(conf_path: str = "/opt/algorithms/db.conf") -> dict:
    cfg = {}
    try:
        with open(conf_path) as f:
            for line in f:
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip()
    except Exception:
        pass
    return cfg

def _connect(cfg: dict):
    import psycopg2
    return psycopg2.connect(
        host=cfg.get("host", "localhost"),
        port=int(cfg.get("port", 5432)),
        dbname=cfg.get("dbname", "urban"),
        user=cfg.get("user", "urban"),
        password=cfg.get("password", ""),
    )

def _tbl(prefix: str, suffix: str) -> str:
    return f'user_project."{prefix}{suffix}"'

def _prefix(pid: int, uid: int, cid: int) -> str:
    return f"project{pid}_user{uid}_case{cid}_"

def _query(conn, sql: str):
    with conn.cursor() as cur:
        cur.execute(sql)
        cols = [d[0] for d in cur.description] if cur.description else []
        rows = cur.fetchall()
    return cols, rows

def _fmt(v, dec=1):
    if v is None:
        return "—"
    try:
        return f"{float(v):.{dec}f}"
    except Exception:
        return str(v)

# ── 数据计算层 ─────────────────────────────────────────────────────────────────

def _calc_motor(conn, prefix: str, logs: list) -> dict:
    tbl_way  = _tbl(prefix, "road_way")
    result = {}
    try:
        # road_way 已包含 volume/v_c（由 greedy 回写），直接按 type 聚合
        _, rows = _query(conn, f"""
            SELECT w.type,
                   COUNT(*) AS cnt,
                   ROUND(SUM(w.length)::numeric,0) AS total_len,
                   ROUND(AVG(w.volume)::numeric,1) AS avg_vol,
                   ROUND(SUM(w.volume * w.length / 1e6)::numeric,1) AS vmt,
                   ROUND(SUM(w.volume * w.length / 1e6 / NULLIF(w.speedlimit,0))::numeric,2) AS vht,
                   ROUND(AVG(w.v_c)::numeric,3) AS avg_vc,
                   SUM(CASE WHEN w.v_c < 0.6 THEN 1 ELSE 0 END) AS vc_a,
                   SUM(CASE WHEN w.v_c >= 0.6 AND w.v_c < 0.8 THEN 1 ELSE 0 END) AS vc_b,
                   SUM(CASE WHEN w.v_c >= 0.8 AND w.v_c < 1.0 THEN 1 ELSE 0 END) AS vc_c,
                   SUM(CASE WHEN w.v_c >= 1.0 THEN 1 ELSE 0 END) AS vc_d
            FROM {tbl_way} w
            WHERE w.type IS NOT NULL
            GROUP BY w.type ORDER BY w.type
        """)
        type_map = {1:'快速路',2:'主干路',3:'次干路',4:'支路',5:'其他'}
        type_data, total_vmt, total_vht = [], 0.0, 0.0
        vc_dist = [0, 0, 0, 0]
        for r in rows:
            rt, cnt, tlen, avg_vol, vmt, vht, avg_vc, va, vb, vc, vd = r
            label = type_map.get(int(rt), f'Type{rt}')
            vmt = float(vmt or 0); vht = float(vht or 0)
            type_data.append({'label': label, 'count': int(cnt),
                               'length_km': round(float(tlen or 0)/1000, 2),
                               'avg_vol': float(avg_vol or 0),
                               'vmt': vmt, 'vht': vht,
                               'avg_vc': float(avg_vc or 0)})
            total_vmt += vmt; total_vht += vht
            vc_dist[0] += int(va or 0); vc_dist[1] += int(vb or 0)
            vc_dist[2] += int(vc or 0); vc_dist[3] += int(vd or 0)
        result['type_data'] = type_data
        result['total_vmt'] = round(total_vmt, 1)
        result['total_vht'] = round(total_vht, 2)
        result['vc_dist'] = vc_dist
        result['congested_count'] = vc_dist[2] + vc_dist[3]
        logs.append(f"motor: vmt={total_vmt:.1f} 万pcu·km, vht={total_vht:.2f} 万pcu·h, congested={vc_dist[3]}")
    except Exception as e:
        logs.append(f"motor calc failed: {e}")
        result.setdefault('type_data', [])
        result.setdefault('total_vmt', 0)
        result.setdefault('total_vht', 0)
        result.setdefault('vc_dist', [0,0,0,0])
        result.setdefault('congested_count', 0)
    return result

def _calc_slow(conn, prefix: str, logs: list) -> dict:
    tbl_way  = _tbl(prefix, "slow_road_way")
    tbl_flow = _tbl(prefix, "slow_greedy_link_flow_results")
    tbl_impact = _tbl(prefix, "slow_impact")
    result = {}
    try:
        # slow_road_way 按 type 分组，JOIN slow_greedy_link_flow_results 取 flow
        _, rows = _query(conn, f"""
            SELECT w.type,
                   COUNT(*) AS cnt,
                   ROUND(SUM(w.length)::numeric,0) AS total_len,
                   ROUND(AVG(f.flow)::numeric,1) AS avg_flow,
                   ROUND(MAX(f.flow)::numeric,1) AS max_flow,
                   ROUND(SUM(f.flow * w.length / 1e6)::numeric,1) AS pmt
            FROM {tbl_way} w
            LEFT JOIN {tbl_flow} f ON w.link_id = f.link_id
            WHERE w.type IS NOT NULL
            GROUP BY w.type ORDER BY w.type
        """)
        type_map = {1:'机动车道',2:'步行道',3:'非机动车道',4:'混合慢行',10:'其他'}
        type_data, total_pmt = [], 0.0
        for r in rows:
            t, cnt, tlen, avg_flow, max_flow, pmt = r
            label = type_map.get(int(t), f'Type{t}')
            pmt = float(pmt or 0)
            type_data.append({'label': label, 'count': int(cnt),
                               'length_km': round(float(tlen or 0)/1000, 2),
                               'avg_flow': float(avg_flow or 0),
                               'max_flow': float(max_flow or 0),
                               'pmt': pmt})
            total_pmt += pmt
        result['type_data'] = type_data
        result['total_pmt'] = round(total_pmt, 1)
        _, fr = _query(conn, f"SELECT ROUND(AVG(flow)::numeric,1), ROUND(MAX(flow)::numeric,1) FROM {tbl_flow} WHERE flow IS NOT NULL")
        result['avg_flow'] = float(fr[0][0] or 0) if fr else 0
        result['max_flow'] = float(fr[0][1] or 0) if fr else 0
        _, top_rows = _query(conn, f"""
            SELECT f.link_id, ROUND(f.flow::numeric,1)
            FROM {tbl_flow} f WHERE f.flow IS NOT NULL
            ORDER BY f.flow DESC LIMIT 10
        """)
        result['top_links'] = [{'link_id': int(r[0]), 'flow': float(r[1])} for r in top_rows]
        _, sr = _query(conn, f"""
            SELECT ROUND(AVG(CASE WHEN COALESCE(w.capacity_slow_adj,0)>0 THEN COALESCE(w.volume, f.flow::double precision)/w.capacity_slow_adj END)::numeric,4) AS avg_vc,
                   ROUND(MAX(CASE WHEN COALESCE(w.capacity_slow_adj,0)>0 THEN COALESCE(w.volume, f.flow::double precision)/w.capacity_slow_adj END)::numeric,4) AS max_vc,
                   SUM(CASE WHEN COALESCE(w.capacity_slow_adj,0)>0 AND COALESCE(w.volume, f.flow::double precision)/w.capacity_slow_adj >= 1.0 THEN 1 ELSE 0 END) AS high_vc_count,
                   SUM(CASE WHEN COALESCE(w.capacity_slow_adj,0)>0 AND COALESCE(w.volume, f.flow::double precision)/w.capacity_slow_adj >= 0.8 THEN 1 ELSE 0 END) AS pressure_count,
                   ROUND(AVG(w.capacity)::numeric,1) AS avg_capacity,
                   ROUND(AVG(w.capacity_slow_adj)::numeric,1) AS avg_capacity_slow_adj,
                   ROUND(AVG(GREATEST(COALESCE(w.capacity,0)-COALESCE(w.capacity_slow_adj,0),0))::numeric,1) AS avg_capacity_reduction
            FROM {tbl_way} w
            LEFT JOIN {tbl_flow} f ON w.link_id = f.link_id
            WHERE COALESCE(w.type,0) <> 10
        """)
        if sr:
            r = sr[0]
            result['avg_vc'] = float(r[0] or 0)
            result['max_vc'] = float(r[1] or 0)
            result['high_vc_count'] = int(r[2] or 0)
            result['pressure_count'] = int(r[3] or 0)
            result['avg_capacity'] = float(r[4] or 0)
            result['avg_capacity_slow_adj'] = float(r[5] or 0)
            result['avg_capacity_reduction'] = float(r[6] or 0)

        # Planning-standard indicators from slow_impact:
        # 1) facility_standard_pct: length share where bike_lane_width >= req_bike_width
        # 2) safety_coverage_pct: length share with physical separation or required lateral clearance
        try:
            _, pr = _query(conn, f"""
                SELECT
                    ROUND((100.0 * SUM(CASE
                        WHEN COALESCE(i.aff_lane_num,0) > 0
                         AND COALESCE(i.bike_lane_width,0) >= COALESCE(i.req_bike_width,0)
                        THEN COALESCE(w.length,0) ELSE 0 END)
                        / NULLIF(SUM(CASE WHEN COALESCE(i.aff_lane_num,0) > 0 THEN COALESCE(w.length,0) ELSE 0 END),0))::numeric, 2) AS facility_standard_pct,
                    ROUND((100.0 * SUM(CASE
                        WHEN COALESCE(i.aff_lane_num,0) > 0
                         AND (
                            COALESCE(i.separation_type,0) = 2
                            OR (
                                CASE WHEN COALESCE(i.has_parking,0)=1
                                     THEN COALESCE(i.lat_safety_clear,0) >= 0.9
                                     ELSE COALESCE(i.lat_safety_clear,0) >= 0.5
                                END
                            )
                         )
                        THEN COALESCE(w.length,0) ELSE 0 END)
                        / NULLIF(SUM(CASE WHEN COALESCE(i.aff_lane_num,0) > 0 THEN COALESCE(w.length,0) ELSE 0 END),0))::numeric, 2) AS safety_coverage_pct
                FROM {tbl_impact} i
                JOIN {tbl_way} w ON w.link_id = i.link_id
                WHERE COALESCE(w.type,0) <> 10
            """)
            if pr:
                result['facility_standard_pct'] = float(pr[0][0] or 0)
                result['safety_coverage_pct'] = float(pr[0][1] or 0)
        except Exception as e:
            try:
                conn.rollback()
            except Exception:
                pass
            logs.append(f"slow planning metrics failed: {e}")
            result.setdefault('facility_standard_pct', 0)
            result.setdefault('safety_coverage_pct', 0)

        logs.append(f"slow: pmt={total_pmt:.1f}, avg_flow={result['avg_flow']}, avg_vc={result.get('avg_vc',0)}")
    except Exception as e:
        logs.append(f"slow calc failed: {e}")
        result.setdefault('type_data', [])
        result.setdefault('total_pmt', 0)
        result.setdefault('avg_flow', 0)
        result.setdefault('max_flow', 0)
        result.setdefault('top_links', [])
        result.setdefault('avg_vc', 0)
        result.setdefault('max_vc', 0)
        result.setdefault('high_vc_count', 0)
        result.setdefault('pressure_count', 0)
        result.setdefault('avg_capacity', 0)
        result.setdefault('avg_capacity_slow_adj', 0)
        result.setdefault('avg_capacity_reduction', 0)
        result.setdefault('facility_standard_pct', 0)
        result.setdefault('safety_coverage_pct', 0)
    return result

def _calc_pt(conn, prefix: str, logs: list) -> dict:
    result = {}
    try:
        tbl_route = _tbl(prefix, "pt_route")
        tbl_stop  = _tbl(prefix, "pt_stop")
        tbl_link  = _tbl(prefix, "pt_link_result")
        _, rr = _query(conn, f"SELECT COUNT(*) FROM {tbl_route}")
        _, sr = _query(conn, f"SELECT COUNT(*) FROM {tbl_stop}")
        result['route_count'] = int(rr[0][0]) if rr else 0
        result['stop_count']  = int(sr[0][0]) if sr else 0
        _, lr = _query(conn, f"""
            SELECT link_type,
                   COUNT(*) AS cnt,
                   ROUND(AVG(flow)::numeric,1) AS avg_flow,
                   ROUND(MAX(flow)::numeric,1) AS max_flow,
                   ROUND(SUM(flow)::numeric,1) AS total_flow
            FROM {tbl_link}
            WHERE flow IS NOT NULL
            GROUP BY link_type ORDER BY link_type
        """)
        link_data = []
        for r in lr:
            link_data.append({'type': str(r[0]), 'count': int(r[1]),
                               'avg_flow': float(r[2] or 0),
                               'max_flow': float(r[3] or 0),
                               'total_flow': float(r[4] or 0)})
        result['link_data'] = link_data
        enroute = next((d for d in link_data if d['type'] == 'ENROUTE'), {'avg_flow': 0, 'total_flow': 0})
        result['enroute_avg'] = enroute['avg_flow']
        result['enroute_total'] = enroute['total_flow']
        logs.append(f"pt: routes={result['route_count']}, ENROUTE avg={result['enroute_avg']}")
    except Exception as e:
        logs.append(f"pt calc failed: {e}")
        result.setdefault('route_count', 0)
        result.setdefault('stop_count', 0)
        result.setdefault('link_data', [])
        result.setdefault('enroute_avg', 0)
        result.setdefault('enroute_total', 0)
    return result


# ── 效益量化 ──────────────────────────────────────────────────────────────────
# 参考文献（方法依据）：
# [1] HCM 2010. Highway Capacity Manual. TRB.
# [2] 发改委. 建设项目经济评价方法与参数（第三版）, 2006.
# [3] Ceder, A. (2007). Public Transit Planning and Operation. Elsevier.
# [4] 交通运输部. 城市步行和自行车交通系统规划设计导则, 2013.
# [5] IPCC. Emission Factor for Passenger Car: 0.18 kg CO2/km (average).

VOT_YUAN_PER_H  = 35.0    # 出行时间价值（元/小时），参考[2]城市居民
FUEL_YUAN_PER_KM = 0.65   # 燃油成本（元/km），参考[2]综合单价
CO2_KG_PER_KM   = 0.18   # 乘用车CO2排放因子（kg/km），参考[5]
WALK_CO2_SAVED   = 0.12   # 步行替代小汽车节省CO2（kg/km），参考[5]
MET_WALK_PER_KM  = 3.5    # 步行健康活动潜力MET·min/km，参考[4]
PT_BOARDINGS_VOT = 25.0   # 公交乘客时间价值（元/小时），参考[2]


def _benefit_motor(new_m: dict, base_m: dict | None) -> dict:
    """机动车改造效益量化（BPR延误模型 + VOT货币化）"""
    b = {}
    new_vmt = new_m.get('total_vmt', 0)
    new_vht = new_m.get('total_vht', 0)
    new_cong = new_m.get('congested_count', 0)
    b['new_vmt'] = new_vmt
    b['new_vht'] = new_vht
    b['new_cong'] = new_cong
    if base_m:
        base_vmt = base_m.get('total_vmt', 0)
        base_vht = base_m.get('total_vht', 0)
        base_cong = base_m.get('congested_count', 0)
        delta_vht = base_vht - new_vht          # 节省的总行程时间（车辆·小时）
        delta_vmt = new_vmt - base_vmt          # VMT变化（正=增加）
        delta_cong = base_cong - new_cong        # 拥堵路段减少数
        time_benefit_yuan = delta_vht * VOT_YUAN_PER_H
        fuel_benefit_yuan = -delta_vmt * FUEL_YUAN_PER_KM if delta_vmt < 0 else 0
        co2_saved_kg = -delta_vmt * CO2_KG_PER_KM if delta_vmt < 0 else 0
        b.update({
            'base_vmt': base_vmt, 'base_vht': base_vht, 'base_cong': base_cong,
            'delta_vht': round(delta_vht, 2),
            'delta_vmt': round(delta_vmt, 1),
            'delta_cong': delta_cong,
            'time_benefit_yuan': round(time_benefit_yuan, 0),
            'fuel_benefit_yuan': round(fuel_benefit_yuan, 0),
            'co2_saved_kg': round(co2_saved_kg, 1),
            'has_compare': True,
        })
    else:
        b['has_compare'] = False
    return b

def _benefit_slow(new_s: dict, base_s: dict | None) -> dict:
    """慢行改造效益量化（碳减排 + 健康活动潜力）"""
    b = {}
    new_pmt = new_s.get('total_pmt', 0)
    new_avg = new_s.get('avg_flow', 0)
    b['new_pmt'] = new_pmt
    b['new_avg'] = new_avg
    if base_s:
        base_pmt = base_s.get('total_pmt', 0)
        base_avg = base_s.get('avg_flow', 0)
        delta_pmt = new_pmt - base_pmt
        co2_saved_kg = delta_pmt * WALK_CO2_SAVED
        health_met = delta_pmt * MET_WALK_PER_KM
        delta_avg_flow = new_avg - base_avg
        delta_avg_vc = new_s.get('avg_vc', 0) - base_s.get('avg_vc', 0)
        delta_max_vc = new_s.get('max_vc', 0) - base_s.get('max_vc', 0)
        delta_high_vc = new_s.get('high_vc_count', 0) - base_s.get('high_vc_count', 0)
        delta_pressure = new_s.get('pressure_count', 0) - base_s.get('pressure_count', 0)
        delta_avg_cap_adj = new_s.get('avg_capacity_slow_adj', 0) - base_s.get('avg_capacity_slow_adj', 0)
        vc_change_rate = (delta_avg_vc / base_s.get('avg_vc', 0) * 100) if base_s.get('avg_vc', 0) else 0
        delta_facility_standard_pct = new_s.get('facility_standard_pct', 0) - base_s.get('facility_standard_pct', 0)
        delta_safety_coverage_pct = new_s.get('safety_coverage_pct', 0) - base_s.get('safety_coverage_pct', 0)
        b.update({
            'base_pmt': base_pmt, 'base_avg': base_avg,
            'base_avg_vc': base_s.get('avg_vc', 0),
            'base_max_vc': base_s.get('max_vc', 0),
            'base_high_vc_count': base_s.get('high_vc_count', 0),
            'base_pressure_count': base_s.get('pressure_count', 0),
            'base_avg_capacity_slow_adj': base_s.get('avg_capacity_slow_adj', 0),
            'delta_pmt': round(delta_pmt, 1),
            'delta_avg_flow': round(delta_avg_flow, 1),
            'delta_avg_vc': round(delta_avg_vc, 4),
            'delta_max_vc': round(delta_max_vc, 4),
            'delta_high_vc_count': int(delta_high_vc),
            'delta_pressure_count': int(delta_pressure),
            'delta_avg_capacity_slow_adj': round(delta_avg_cap_adj, 1),
            'vc_change_rate': round(vc_change_rate, 1),
            'base_facility_standard_pct': base_s.get('facility_standard_pct', 0),
            'base_safety_coverage_pct': base_s.get('safety_coverage_pct', 0),
            'delta_facility_standard_pct': round(delta_facility_standard_pct, 2),
            'delta_safety_coverage_pct': round(delta_safety_coverage_pct, 2),
            'co2_saved_kg': round(co2_saved_kg, 1),
            'health_met': round(health_met, 0),
            'has_compare': True,
        })
    else:
        b['has_compare'] = False
    return b

def _benefit_pt(new_p: dict, base_p: dict | None) -> dict:
    """公交线路效益量化（客运量 + 时间节省）"""
    b = {}
    new_enroute = new_p.get('enroute_total', 0)
    new_avg = new_p.get('enroute_avg', 0)
    b['new_enroute'] = new_enroute
    b['new_avg'] = new_avg
    if base_p:
        base_enroute = base_p.get('enroute_total', 0)
        base_avg = base_p.get('enroute_avg', 0)
        delta_enroute = new_enroute - base_enroute
        delta_avg = new_avg - base_avg
        time_benefit = delta_enroute * PT_BOARDINGS_VOT / 60  # 以分钟候车时间节省估算，需具体数据
        b.update({
            'base_enroute': base_enroute, 'base_avg': base_avg,
            'delta_enroute': round(delta_enroute, 1),
            'delta_avg': round(delta_avg, 1),
            'time_benefit_yuan': round(time_benefit, 0),
            'has_compare': True,
        })
    else:
        b['has_compare'] = False
    return b


# ── HTML 生成 ─────────────────────────────────────────────────────────────────
_HTML_TPL = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
{leaflet_head}
<style>
*{{box-sizing:border-box;margin:0;padding:0}}
body{{font-family:"Times New Roman","宋体",serif;background:#eef2f7;color:#1a1a2e;font-size:14px;line-height:1.6}}
/* ── 页眉 ── */
.hdr{{background:linear-gradient(120deg,#0d2137 0%,#1a4a7a 60%,#2471a3 100%);
      color:#fff;padding:24px 48px 20px}}
.hdr h1{{font-size:20px;font-weight:700;letter-spacing:.5px;margin-bottom:4px}}
.hdr .meta{{font-size:11.5px;opacity:.75;display:flex;flex-wrap:wrap;gap:6px 16px}}
.hdr .meta span{{white-space:nowrap}}
/* ── 导航 ── */
.nav{{position:sticky;top:0;z-index:100;display:flex;gap:4px;padding:8px 48px;
      background:#fff;border-bottom:2px solid #d0dce8;flex-wrap:wrap}}
.nav a{{padding:4px 14px;border-radius:3px;text-decoration:none;color:#1a4a7a;
        font-size:12.5px;border:1px solid #b8cfe6;transition:all .15s}}
.nav a:hover,.nav a.act{{background:#1a4a7a;color:#fff;border-color:#1a4a7a}}
/* ── 主内容 ── */
.wrap{{max-width:1180px;margin:20px auto;padding:0 24px}}
.section{{background:#fff;border-radius:6px;box-shadow:0 1px 4px rgba(0,0,0,.08);
          padding:22px 28px;margin-bottom:20px}}
.sec-title{{font-size:15px;font-weight:700;color:#0d2137;
            border-left:4px solid #1a6ebd;padding-left:10px;margin-bottom:14px}}
/* ── KPI 卡片 ── */
.kpi-row{{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px}}
.kpi{{flex:1;min-width:130px;background:linear-gradient(135deg,#f4f8fd,#e8f1fb);
      border:1px solid #c8ddf0;border-radius:6px;padding:12px 16px;text-align:center}}
.kpi .val{{font-size:22px;font-weight:700;color:#0d2137;line-height:1.2}}
.kpi .lbl{{font-size:11px;color:#6b7e95;margin-top:3px}}
.kpi.hi .val{{color:#c0392b}}
.kpi.ok .val{{color:#1e8449}}
.kpi.warn .val{{color:#b7770d}}
/* ── 2 列网格 ── */
.g2{{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:14px}}
.g3{{display:grid;grid-template-columns:1fr 1fr 1fr;gap:14px;margin-bottom:14px}}
/* ── 图表卡片 ── */
.card{{background:#fafcff;border:1px solid #dce8f5;border-radius:6px;padding:14px 16px}}
.card h3{{font-size:12.5px;color:#1a4a7a;margin-bottom:8px;font-weight:600;
          border-bottom:1px solid #e0eaf5;padding-bottom:6px}}
/* ── 描述块 ── */
.desc{{background:#eef6ff;border-left:4px solid #1a6ebd;padding:10px 14px;border-radius:4px;
       font-size:13px;line-height:1.7;margin-bottom:14px}}
.desc.green{{background:#edfbf3;border-color:#1e8449}}
.desc.orange{{background:#fff8ee;border-color:#ca6f1e}}
/* ── 效益表 ── */
.bene{{background:#f0faf5;border:1px solid #a9dfbf;border-radius:6px;padding:14px 18px;margin-bottom:14px}}
.bene h4{{font-size:13px;color:#1a5e35;font-weight:700;margin-bottom:10px}}
.bene table{{width:100%;border-collapse:collapse;font-size:12.5px}}
.bene thead tr{{background:#a9dfbf}}
.bene th{{padding:6px 10px;text-align:left;font-weight:600;color:#1a5e35}}
.bene td{{padding:5px 10px;border-bottom:1px solid #d5f0e2}}
.bene td.hi{{color:#c0392b;font-weight:700}}
.bene td.ok{{color:#1e8449;font-weight:700}}
/* ── 参考 ── */
.ref-list{{font-size:12px;color:#666;line-height:2}}
@media(max-width:820px){{.g2,.g3{{grid-template-columns:1fr}}}}
{map_css}
</style>
</head>
<body>
<div class="hdr">
  <h1>{title}</h1>
  <div class="meta">
    <span>项目名 {project_name}</span>
    <span>项目 ID {project_id}</span>
    <span>用户 {user_id}</span>
    <span>方案 {case_id}</span>
    {base_case_label}
    <span>分析维度 {type_label}</span>
    <span>生成时间：{gen_time}</span>
    <br>{data_sources}
  </div>
</div>
<div class="nav">
  <a href="#s_main" class="act">核心指标</a>
  <a href="#s_bene">效益量化</a>
  <a href="#s_detail">详细分析</a>
  <a href="#s_maps">空间分布</a>
  <a href="#s_ref">参考文献</a>
</div>
<div class="wrap">
{body}
{map_section}
<div id="s_ref" class="section">
  <div class="sec-title">参考文献</div>
  <div class="ref-list">{references}</div>
</div>
</div>
<script>
const __ECHARTS_PLACEHOLDER__ = 1;
</script>
<script>
var _D = {data_json};
document.addEventListener('DOMContentLoaded', function(){{
  {chart_js}
}});
</script>
{map_js}
</body></html>
"""

# ─ motor HTML ─────────────────────────────────────────────────────────────────
def _motor_html(new_m: dict, bene: dict, has_compare: bool) -> tuple[str, str]:
    vmt_delta = bene.get('delta_vmt', 0)
    vht_delta = bene.get('delta_vht', 0)
    cong_delta = bene.get('delta_cong', 0)

    desc = (f"方案网络总VMT（车辆行驶里程）<strong>{_fmt(new_m['total_vmt'],1)}</strong> 万pcu·km，"
            f"总VHT（行程时间）<strong>{_fmt(new_m['total_vht'],2)}</strong> 万pcu·h，"
            f"过饱和路段 <strong>{new_m['congested_count']}</strong> 条。")
    if has_compare:
        arrow_v = "↓" if vht_delta >= 0 else "↑"
        arrow_c = "↓" if cong_delta >= 0 else "↑"
        desc += (f"相较于现状方案，网络行程时间 <strong>{arrow_v}{abs(vht_delta):.2f}</strong> 万pcu·h，"
                 f"拥堵路段 <strong>{arrow_c}{abs(cong_delta)}</strong> 条，"
                 f"时间效益约 <strong>{bene.get('time_benefit_yuan',0):,.0f}</strong> 元/日。")

    kpi = ""
    for item in [
        ("VMT", f"{_fmt(new_m['total_vmt'],1)} 万pcu·km", ""),
        ("VHT", f"{_fmt(new_m['total_vht'],2)} 万pcu·h", ""),
        ("过饱和路段", str(new_m['congested_count']), "hi" if new_m['congested_count'] > 0 else "ok"),
        ("ΔVHT", (f"{'-' if vht_delta>=0 else '+'}{abs(vht_delta):.2f} 万h") if has_compare else "—", "ok" if vht_delta > 0 else ""),
    ]:
        kpi += f'<div class="kpi {item[2]}"><div class="val">{item[1]}</div><div class="lbl">{item[0]}</div></div>\n'

    bene_html = ""
    if has_compare:
        bene_html = f"""
<div class="bene"><h4>效益量化摘要（参考《建设项目经济评价方法与参数》第三版）</h4>
<table>
<tr><th>指标</th><th>现状</th><th>改造后</th><th>变化量</th><th>货币化效益（元/日）</th></tr>
<tr><td>总VMT（万pcu·km）</td><td>{_fmt(bene['base_vmt'],1)}</td><td>{_fmt(bene['new_vmt'],1)}</td>
    <td class="{'ok' if bene['delta_vmt']<=0 else 'hi'}">{bene['delta_vmt']:+.1f}</td>
    <td class="ok">{bene.get('fuel_benefit_yuan',0):+,.0f}</td></tr>
<tr><td>总VHT（万pcu·h）</td><td>{_fmt(bene['base_vht'],2)}</td><td>{_fmt(bene['new_vht'],2)}</td>
    <td class="{'ok' if bene['delta_vht']>=0 else 'hi'}">{bene['delta_vht']:+.2f}</td>
    <td class="ok">{bene.get('time_benefit_yuan',0):+,.0f}</td></tr>
<tr><td>过饱和路段数</td><td>{bene['base_cong']}</td><td>{bene['new_cong']}</td>
    <td class="{'ok' if bene['delta_cong']>=0 else 'hi'}">{bene['delta_cong']:+d}</td><td>—</td></tr>
<tr><td>CO₂减排潜力（kg/日）</td><td>—</td><td>—</td><td>—</td>
    <td class="ok">{bene.get('co2_saved_kg',0):+.1f} kg</td></tr>
</table>
<p class="ref">时间价值取值 {VOT_YUAN_PER_H} 元/h（城市居民），燃油单价 {FUEL_YUAN_PER_KM} 元/km，CO₂排放因子 {CO2_KG_PER_KM} kg/km（参考IPCC 2006）。</p>
</div>"""

    compare_chart = ""
    if has_compare:
        compare_chart = '<div class="card"><h3>???? vs ???? ? ?????</h3><div id="c_slow_core" style="width:100%;height:280px"></div></div>'
    charts_html = f"""
{compare_chart}
<div class="g2">
  <div class="card"><h3>各等级路段 VMT 构成（万pcu·km）</h3>
    <div id="c_vmt_bar" style="width:100%;height:260px"></div></div>
  <div class="card"><h3>各等级路段 VHT 构成（万pcu·h）</h3>
    <div id="c_vht_bar" style="width:100%;height:260px"></div></div>
</div>
<div class="g2">
  <div class="card"><h3>V/C 分布（路段数）</h3>
    <div id="c_vc_dist" style="width:100%;height:240px"></div></div>
  <div class="card"><h3>各等级平均 V/C 对比</h3>
    <div id="c_vc_type" style="width:100%;height:240px"></div></div>
</div>"""
    if has_compare:
        charts_html += """
<div class="g2">
  <div class="card"><h3>改造前后 VMT 对比（万pcu·km）</h3>
    <div id="c_cmp_vmt" style="width:100%;height:240px"></div></div>
  <div class="card"><h3>改造前后 VHT 对比（万pcu·h）</h3>
    <div id="c_cmp_vht" style="width:100%;height:240px"></div></div>
</div>"""

    glossary_rows = "".join(
        f"<tr><td><strong>{g['name']}</strong></td><td>{g['meaning']}</td>"
        f"<td style='font-size:11px'>{g['formula']}</td><td style='font-size:11px;color:#666'>{g['reference']}</td>"
        f"<td>{g['unit']}</td></tr>"
        for g in CBA_MOTOR_GLOSSARY
    )
    glossary_html = f"""
<div class="card" style="margin-bottom:16px">
  <h3>指标含义与参考</h3>
  <table class="dtable">
    <tr><th>指标</th><th>作用</th><th>计算方法</th><th>合理参考</th><th>单位</th></tr>
    {glossary_rows}
  </table>
</div>"""

    body = f"""
<div id="s_main" class="section">
  <div class="sec-title">机动车道路改造 — 核心指标</div>
  {glossary_html}
  <div class="desc" id="desc_motor">{desc}</div>
  <div class="kpi-row">{kpi}</div>
  {bene_html}
</div>
<div id="s_detail" class="section">
  <div class="sec-title">详细分析</div>
  {charts_html}
</div>"""
    return body

def _motor_js(new_m: dict, bene: dict, has_compare: bool) -> str:
    MC = "#2980b9,#27ae60,#e67e22,#8e44ad,#c0392b"
    td = new_m.get('type_data', [])
    labels  = json.dumps([d['label'] for d in td])
    vmts    = json.dumps([d['vmt']   for d in td])
    vhts    = json.dumps([d['vht']   for d in td])
    avg_vcs = json.dumps([d['avg_vc'] for d in td])
    vc_dist = json.dumps(new_m.get('vc_dist', [0,0,0,0]))
    td_json = json.dumps(td, ensure_ascii=False)

    def _color_map_bar(data_js, colors_csv, toFixed='1'):
        cs = ["'" + c + "'" for c in colors_csv.split(',')]
        cs_js = ','.join(cs)
        return (data_js + ".map(function(v,i){"
                "var c=[" + cs_js + "];"
                "return{value:v,itemStyle:{color:c[i%c.length]}}})")

    G = "{left:56,right:16,top:32,bottom:12,containLabel:true}"
    js = (
        "\n  (()=>{"
        "\n    var _td=" + td_json + ";"
        "\n    var _vc=" + vc_dist + ";"
        "\n    var _ec=function(id){return echarts.init(document.getElementById(id))};"
        "\n    _ec('c_vmt_bar').setOption({"
        "\n      tooltip:{trigger:'axis',formatter:function(p){return p[0].name+'<br/>VMT: '+p[0].value.toFixed(2)+' 万pcu·km'}},"
        "\n      grid:" + G + ","
        "\n      xAxis:{type:'category',data:" + labels + ",axisLabel:{fontSize:12}},"
        "\n      yAxis:{type:'value',name:'万pcu·km',nameTextStyle:{fontSize:12}},"
        "\n      series:[{type:'bar',barMaxWidth:52,"
        "\n        data:" + _color_map_bar(vmts, MC) + ","
        "\n        label:{show:true,position:'top',fontSize:11,formatter:function(p){return p.value.toFixed(1)}}}]"
        "\n    });"
        "\n    _ec('c_vht_bar').setOption({"
        "\n      tooltip:{trigger:'axis',formatter:function(p){return p[0].name+'<br/>VHT: '+p[0].value.toFixed(3)+' 万pcu·h'}},"
        "\n      grid:" + G + ","
        "\n      xAxis:{type:'category',data:" + labels + ",axisLabel:{fontSize:12}},"
        "\n      yAxis:{type:'value',name:'万pcu·h',nameTextStyle:{fontSize:12}},"
        "\n      series:[{type:'bar',barMaxWidth:52,"
        "\n        data:" + _color_map_bar(vhts, MC) + ","
        "\n        label:{show:true,position:'top',fontSize:11,formatter:function(p){return p.value.toFixed(3)}}}]"
        "\n    });"
        "\n    _ec('c_vc_dist').setOption({"
        "\n      tooltip:{trigger:'item',formatter:'{b}: {c} 条 ({d}%)'},"
        "\n      legend:{orient:'vertical',right:4,top:'center',itemWidth:10,textStyle:{fontSize:11}},"
        "\n      series:[{type:'pie',radius:['36%','64%'],center:['38%','50%'],"
        "\n        data:["
        "\n          {name:'畅通 V/C<0.6',value:_vc[0],itemStyle:{color:'#1e8449'}},"
        "\n          {name:'基本畅通 0.6-0.8',value:_vc[1],itemStyle:{color:'#d4ac0d'}},"
        "\n          {name:'轻度拥堵 0.8-1.0',value:_vc[2],itemStyle:{color:'#ca6f1e'}},"
        "\n          {name:'过饱和 >=1.0',value:_vc[3],itemStyle:{color:'#c0392b'}},"
        "\n        ],label:{formatter:'{d}%',fontSize:11},labelLine:{length:8,length2:6}}]"
        "\n    });"
        "\n    _ec('c_vc_type').setOption({"
        "\n      tooltip:{trigger:'axis'},"
        "\n      grid:" + G + ","
        "\n      xAxis:{type:'category',data:" + labels + ",axisLabel:{fontSize:12}},"
        "\n      yAxis:{type:'value',name:'平均V/C',nameTextStyle:{fontSize:12},max:1.5,"
        "\n        splitLine:{lineStyle:{type:'dashed'}}},"
        "\n      series:[{type:'bar',barMaxWidth:52,"
        "\n        data:" + avg_vcs + ".map(function(v){"
        "var c=['#1e8449','#d4ac0d','#ca6f1e','#c0392b'];"
        "return{value:v,itemStyle:{color:v<0.6?c[0]:v<0.8?c[1]:v<1.0?c[2]:c[3]}}}),"
        "\n        label:{show:true,position:'top',fontSize:11,formatter:function(p){return p.value.toFixed(3)}},"
        "\n        markLine:{silent:true,data:[{yAxis:1.0,lineStyle:{color:'#c0392b',type:'dashed'}}]}}]"
        "\n    });"
    )
    if has_compare:
        base_td = bene.get('_base_type_data', [])
        base_vmts = json.dumps([d['vmt'] for d in base_td])
        base_vhts = json.dumps([d['vht'] for d in base_td])
        GC = "{left:56,right:16,top:32,bottom:36,containLabel:true}"
        js += (
            "\n    _ec('c_cmp_vmt').setOption({"
            "\n      tooltip:{trigger:'axis'},"
            "\n      legend:{data:['现状','改造后'],bottom:2,textStyle:{fontSize:11}},"
            "\n      grid:" + GC + ","
            "\n      xAxis:{type:'category',data:" + labels + ",axisLabel:{fontSize:12}},"
            "\n      yAxis:{type:'value',name:'万pcu·km',nameTextStyle:{fontSize:12}},"
            "\n      series:["
            "\n        {name:'现状',type:'bar',barGap:'8%',barMaxWidth:32,"
            "\n          data:" + base_vmts + ".map(function(v){return{value:v,itemStyle:{color:'#aab7c4'}}}),"
            "\n          label:{show:true,position:'top',fontSize:10}},"
            "\n        {name:'改造后',type:'bar',barMaxWidth:32,"
            "\n          data:" + vmts + ".map(function(v){return{value:v,itemStyle:{color:'#1a6ebd'}}}),"
            "\n          label:{show:true,position:'top',fontSize:10}},"
            "\n      ]"
            "\n    });"
            "\n    _ec('c_cmp_vht').setOption({"
            "\n      tooltip:{trigger:'axis'},"
            "\n      legend:{data:['现状','改造后'],bottom:2,textStyle:{fontSize:11}},"
            "\n      grid:" + GC + ","
            "\n      xAxis:{type:'category',data:" + labels + ",axisLabel:{fontSize:12}},"
            "\n      yAxis:{type:'value',name:'万pcu·h',nameTextStyle:{fontSize:12}},"
            "\n      series:["
            "\n        {name:'现状',type:'bar',barGap:'8%',barMaxWidth:32,"
            "\n          data:" + base_vhts + ".map(function(v){return{value:v,itemStyle:{color:'#aab7c4'}}}),"
            "\n          label:{show:true,position:'top',fontSize:10}},"
            "\n        {name:'改造后',type:'bar',barMaxWidth:32,"
            "\n          data:" + vhts + ".map(function(v){return{value:v,itemStyle:{color:'#1e8449'}}}),"
            "\n          label:{show:true,position:'top',fontSize:10}},"
            "\n      ]"
            "\n    });"
        )
    js += "\n  })();"
    return js

# ─ slow HTML ──────────────────────────────────────────────────────────────────


def _slow_js(new_s: dict, bene: dict, has_compare: bool) -> str:
    SC = "#27ae60,#2ecc71,#82e0aa,#abebc6"
    SC_list = ','.join("'" + c + "'" for c in SC.split(','))
    td = new_s.get('type_data', [])
    labels    = json.dumps([d['label']    for d in td])
    avg_flows = json.dumps([d['avg_flow'] for d in td])
    max_flows = json.dumps([d['max_flow'] for d in td])
    pie_data  = json.dumps([{'name': d['label'], 'value': d['pmt']} for d in td], ensure_ascii=False)
    td_json   = json.dumps(td, ensure_ascii=False)
    top_links = new_s.get('top_links', [])

    def _cm(data_js, colors_csv, extra=''):
        cs_js = ','.join("'" + c + "'" for c in colors_csv.split(','))
        return (data_js + ".map(function(v,i){var c=[" + cs_js + "];"
                "return{value:v,itemStyle:{color:c[i%c.length]" + extra + "}}})")

    js = (
        "\n  (()=>{"
        "\n    var _td=" + td_json + ";"
        "\n    var _ec=function(id){return echarts.init(document.getElementById(id))};"
        "\n    _ec('c_slow_pmt').setOption({"
        "\n      tooltip:{trigger:'item',formatter:'{b}: {c} 万人·km ({d}%)'},"
        "\n      legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},"
        "\n      series:[{type:'pie',radius:['32%','62%'],center:['50%','46%'],"
        "\n        data:" + pie_data + ".map(function(d,i){"
        "var c=[" + ','.join("'" + c + "'" for c in SC.split(',')) + "];"
        "return Object.assign({},d,{itemStyle:{color:c[i%c.length]}})}),"
        "\n        label:{formatter:'{b}\\n{d}%',fontSize:10}}]"
        "\n    });"
        "\n    _ec('c_slow_cmp').setOption({"
        "\n      tooltip:{trigger:'axis'},"
        "\n      legend:{data:['平均流量','峰值流量'],bottom:2,textStyle:{fontSize:11}},"
        "\n      grid:{left:56,right:16,top:32,bottom:40,containLabel:true},"
        "\n      xAxis:{type:'category',data:" + labels + ",axisLabel:{fontSize:12}},"
        "\n      yAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:12}},"
        "\n      series:["
        "\n        {name:'平均流量',type:'bar',barGap:'10%',barMaxWidth:36,"
        "\n          data:" + _cm(avg_flows, SC) + ","
        "\n          label:{show:true,position:'top',fontSize:11}},"
        "\n        {name:'峰值流量',type:'bar',barMaxWidth:36,"
        "\n          data:" + _cm(max_flows, SC, ",opacity:.45") + ","
        "\n          label:{show:true,position:'top',fontSize:11}},"
        "\n      ]"
        "\n    });"
    )

    if has_compare:
        core_labels = json.dumps(["总 PMT", "平均流量", "峰值流量", "平均 V/C", "高负荷路段数", "平均影响后通行能力"])
        base_vals = json.dumps([
            bene.get('base_pmt', 0),
            bene.get('base_avg', 0),
            bene.get('base_max_flow', bene.get('base_max', 0)),
            bene.get('base_avg_vc', 0),
            bene.get('base_high_vc_count', 0),
            bene.get('base_avg_capacity_slow_adj', 0),
        ], ensure_ascii=False)
        new_vals = json.dumps([
            bene.get('new_pmt', new_s.get('total_pmt', 0)),
            bene.get('new_avg', new_s.get('avg_flow', 0)),
            new_s.get('max_flow', 0),
            new_s.get('avg_vc', 0),
            new_s.get('high_vc_count', 0),
            new_s.get('avg_capacity_slow_adj', 0),
        ], ensure_ascii=False)
        js += (
            "\n    _ec('c_slow_core').setOption({"
            "\n      tooltip:{trigger:'axis'},"
            "\n      legend:{data:['基础方案','普通方案'],top:4},"
            "\n      grid:{left:56,right:24,top:40,bottom:56,containLabel:true},"
            "\n      xAxis:{type:'category',data:" + core_labels + ",axisLabel:{interval:0,rotate:18,fontSize:11}},"
            "\n      yAxis:{type:'value',axisLabel:{fontSize:11}},"
            "\n      series:["
            "\n        {name:'基础方案',type:'bar',barMaxWidth:34,data:" + base_vals + ",itemStyle:{color:'#95a5a6'}},"
            "\n        {name:'普通方案',type:'bar',barMaxWidth:34,data:" + new_vals + ",itemStyle:{color:'#2e6da4'}}"
            "\n      ]"
            "\n    });"
        )

    if top_links:
        tl_ids   = json.dumps(list(reversed(["路段" + str(l['link_id']) for l in top_links])))
        tl_flows = json.dumps(list(reversed([l['flow'] for l in top_links])))
        js += (
            "\n    _ec('c_slow_top').setOption({"
            "\n      tooltip:{trigger:'axis'},"
            "\n      grid:{left:12,right:56,top:8,bottom:8,containLabel:true},"
            "\n      xAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:11}},"
            "\n      yAxis:{type:'category',data:" + tl_ids + ",axisLabel:{fontSize:11}},"
            "\n      series:[{type:'bar',barMaxWidth:20,"
            "\n        data:" + tl_flows + ".map(function(v,i){"
            "return{value:v,itemStyle:{color:'hsl('+(150-i*8)+',55%,'+(48-i*2)+'%)'}}}),"
            "\n        label:{show:true,position:'right',fontSize:11}}]"
            "\n    });"
        )
    else:
        js += ("\n    document.getElementById('c_slow_top').innerHTML="
               "'<div style=\"height:240px;display:flex;align-items:center;"
               "justify-content:center;color:#aaa\">暂无路段级数据</div>';")
    js += "\n  })();"
    return js

# ─ pt HTML ────────────────────────────────────────────────────────────────────
def _pt_html(new_p: dict, bene: dict, has_compare: bool) -> str:
    desc = (f"方案公交网络共 <strong>{new_p['route_count']}</strong> 条线路，"
            f"<strong>{new_p['stop_count']}</strong> 个站点，"
            f"ENROUTE段总客流 <strong>{_fmt(new_p['enroute_total'],1)}</strong> 万人次/h，"
            f"平均 <strong>{_fmt(new_p['enroute_avg'],1)}</strong> 人次/h。")
    if has_compare:
        d_enr = bene.get('delta_enroute', 0)
        desc += (f"相较现状，ENROUTE总客流变化 <strong>{d_enr:+.1f}</strong> 万人次/h，"
                 f"平均流量变化 <strong>{bene.get('delta_avg',0):+.1f}</strong> 人次/h。")
    kpi = ""
    for item in [
        ("线路数", str(new_p['route_count']), ""),
        ("站点数", str(new_p['stop_count']), ""),
        ("ENROUTE总客流", f"{_fmt(new_p['enroute_total'],1)} 万次/h", ""),
        ("ΔENROUTE", (f"{bene.get('delta_enroute',0):+.1f} 万次/h") if has_compare else "—", "ok" if bene.get('delta_enroute',0)>0 else ""),
    ]:
        kpi += f'<div class="kpi {item[2]}"><div class="val">{item[1]}</div><div class="lbl">{item[0]}</div></div>\n'

    bene_html = ""
    if has_compare:
        bene_html = f"""
<div class="bene"><h4>效益量化摘要（参考 Ceder 2007《公共交通规划与运营》）</h4>
<table>
<tr><th>指标</th><th>现状</th><th>改造后</th><th>变化量</th><th>效益</th></tr>
<tr><td>ENROUTE总客流（万次/h）</td>
    <td>{_fmt(bene['base_enroute'],1)}</td><td>{_fmt(bene['new_enroute'],1)}</td>
    <td class="ok">{bene['delta_enroute']:+.1f}</td><td>—</td></tr>
<tr><td>ENROUTE平均流量（次/h）</td>
    <td>{_fmt(bene['base_avg'],1)}</td><td>{_fmt(bene['new_avg'],1)}</td>
    <td class="{'ok' if bene['delta_avg']>0 else 'hi'}">{bene['delta_avg']:+.1f}</td>
    <td class="ok">{bene.get('time_benefit_yuan',0):+,.0f} 元/日</td></tr>
</table>
<p class="ref">乘客时间价值取 {PT_BOARDINGS_VOT} 元/h（公交乘客），参考《建设项目经济评价方法与参数》第三版。</p>
</div>"""

    compare_chart = ""
    if has_compare:
        compare_chart = '<div class="card"><h3>???? vs ???? ? ?????</h3><div id="c_slow_core" style="width:100%;height:280px"></div></div>'
    charts_html = f"""
{compare_chart}
<div class="g2">
  <div class="card"><h3>各链路类型流量构成（人次/h）</h3>
    <div id="c_pt_pie" style="width:100%;height:260px"></div></div>
  <div class="card"><h3>平均流量 vs 峰值流量对比</h3>
    <div id="c_pt_cmp" style="width:100%;height:260px"></div></div>
</div>"""

    return f"""
<div id="s_main" class="section">
  <div class="sec-title">公交线路改造 — 核心指标</div>
  <div class="desc orange">{desc}</div>
  <div class="kpi-row">{kpi}</div>
  {bene_html}
</div>
<div id="s_detail" class="section">
  <div class="sec-title">详细分析</div>
  {charts_html}
</div>"""

def _pt_js(new_p: dict, bene: dict, has_compare: bool) -> str:
    PC = "#e67e22,#d35400,#f39c12,#f8c471"
    PC_list = ','.join("'" + c + "'" for c in PC.split(','))
    ld = new_p.get('link_data', [])
    types       = json.dumps([d['type']       for d in ld])
    avg_flows   = json.dumps([d['avg_flow']   for d in ld])
    max_flows   = json.dumps([d['max_flow']   for d in ld])
    pie_data    = json.dumps([{'name': d['type'], 'value': d['total_flow']} for d in ld], ensure_ascii=False)
    ld_json     = json.dumps(ld, ensure_ascii=False)

    def _cm(data_js, colors_csv, extra=''):
        cs_js = ','.join("'" + c + "'" for c in colors_csv.split(','))
        return (data_js + ".map(function(v,i){var c=[" + cs_js + "];"
                "return{value:v,itemStyle:{color:c[i%c.length]" + extra + "}}})")

    js = (
        "\n  (()=>{"
        "\n    var _ld=" + ld_json + ";"
        "\n    var _ec=function(id){return echarts.init(document.getElementById(id))};"
        "\n    _ec('c_pt_pie').setOption({"
        "\n      tooltip:{trigger:'item',formatter:'{b}: {c} ({d}%)'},"
        "\n      legend:{bottom:4,itemWidth:10,textStyle:{fontSize:11}},"
        "\n      series:[{type:'pie',radius:['32%','62%'],center:['50%','46%'],"
        "\n        data:" + pie_data + ".map(function(d,i){"
        "var c=[" + ','.join("'" + c + "'" for c in PC.split(',')) + "];"
        "return Object.assign({},d,{itemStyle:{color:c[i%c.length]}})}),"
        "\n        label:{formatter:'{b}\\n{d}%',fontSize:10}}]"
        "\n    });"
        "\n    _ec('c_pt_cmp').setOption({"
        "\n      tooltip:{trigger:'axis'},"
        "\n      legend:{data:['平均流量','峰值流量'],bottom:2,textStyle:{fontSize:11}},"
        "\n      grid:{left:56,right:16,top:32,bottom:40,containLabel:true},"
        "\n      xAxis:{type:'category',data:" + types + ",axisLabel:{fontSize:12}},"
        "\n      yAxis:{type:'value',name:'人次/h',nameTextStyle:{fontSize:12}},"
        "\n      series:["
        "\n        {name:'平均流量',type:'bar',barGap:'10%',barMaxWidth:40,"
        "\n          data:" + _cm(avg_flows, PC) + ","
        "\n          label:{show:true,position:'top',fontSize:11,fontWeight:600}},"
        "\n        {name:'峰值流量',type:'bar',barMaxWidth:40,"
        "\n          data:" + _cm(max_flows, PC, ",opacity:.4") + ","
        "\n          label:{show:true,position:'top',fontSize:11}},"
        "\n      ]"
        "\n    });"
        "\n  })();"
    )
    return js

# ─ 参考文献 ───────────────────────────────────────────────────────────────────
_REFS = {
    'motor': """[1] Transportation Research Board. Highway Capacity Manual (HCM). 6th Edition. TRB, 2016.<br>
[2] 国家发展和改革委员会. 建设项目经济评价方法与参数（第三版）. 北京：中国计划出版社, 2006.<br>
[3] BPR延误模型: Bureau of Public Roads. Traffic Assignment Manual. US Dept. of Commerce, 1964.<br>
[4] IPCC. 2006 IPCC Guidelines for National Greenhouse Gas Inventories. Volume 2, Chapter 3.""",
    'slow': """[1] 中华人民共和国住房和城乡建设部. 城市步行和自行车交通系统规划设计导则. 北京: 中国建筑工业出版社, 2013.<br>
[2] WHO. Global action plan on physical activity 2018-2030. Geneva: WHO, 2018.<br>
[3] IPCC. 2006 IPCC Guidelines for National Greenhouse Gas Inventories. Volume 2, Chapter 3.<br>
[4] Woodcock J, et al. Public health benefits of strategies to reduce greenhouse-gas emissions: urban land transport. Lancet, 2009, 374(9705): 1930-1943.""",
    'pt': """[1] Ceder A. Public Transit Planning and Operation: Theory, Modeling and Practice. Oxford: Elsevier, 2007.<br>
[2] 国家发展和改革委员会. 建设项目经济评价方法与参数（第三版）. 北京：中国计划出版社, 2006.<br>
[3] Vuchic V R. Urban Transit: Operations, Planning and Economics. Hoboken: Wiley, 2005.<br>
[4] 中华人民共和国交通运输部. 城市公共交通分类标准（CJJ/T 114—2007）. 北京, 2007.""",
}

# ── 主流程 ────────────────────────────────────────────────────────────────────

def main():
    import argparse, datetime
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--request-file",  dest="req", default="")
    ap.add_argument("--response-file", dest="resp", default="")
    args, _ = ap.parse_known_args()
    in_path  = args.req  or os.environ.get("TNA_INPUT_FILE", "")
    out_path = args.resp or os.environ.get("TNA_OUTPUT_FILE", "")
    if not in_path or not out_path:
        sys.exit(1)

    kv = _read_kv(in_path)
    pid  = int(kv.get("project_id", 0))
    uid  = int(kv.get("user_id", 0))
    cid  = int(kv.get("case_id", 0))
    if pid <= 0 or uid <= 0:
        _fail(out_path, "project_id and user_id must be positive integers")
        return

    p2 = parse_report_param2(kv.get("param2"))
    project_label = project_display_name(p2, pid)

    rpt_type = p2.get("type", "motor").lower()
    base_raw = p2.get("base_case_id")
    cba_stem = f"cost_benefit_report_case{cid}"
    if base_raw is not None and str(base_raw).strip() != "":
        try:
            _bc = int(base_raw)
            if _bc != cid:
                cba_stem = f"cost_benefit_report_case{cid}_vs_case{_bc}"
        except (TypeError, ValueError):
            pass
    output_path = resolve_report_output_path(
        p2,
        report_stem=cba_stem,
        fallback_path=f"/tmp/tna_cba_report_{pid}_{uid}_{cid}.html",
    )

    base_cid = None
    if base_raw is not None and str(base_raw).strip() != "":
        try:
            base_cid = int(base_raw)
        except (TypeError, ValueError):
            _fail(out_path, f"param2.base_case_id 须为整数，当前: {base_raw!r}")
        if base_cid < 0:
            _fail(out_path, "param2.base_case_id 无效（方案 ID 须 ≥ 0）")

    logs: list[str] = []
    # 仅当传入 base_case_id 且与 case_id 不同时做现状对比；相同或未传 → 单方案
    if base_cid is not None and base_cid != cid:
        has_compare = True
        logs.append(f"对比模式：改造 case{cid} vs 现状 case{base_cid}")
    else:
        has_compare = False
        if base_cid is not None and base_cid == cid:
            logs.append(f"base_case_id 与 case_id 均为 {cid}，按单方案分析（不对比）")
        else:
            logs.append(f"未传 base_case_id，单方案 case{cid} 成本效益分析")

    if rpt_type not in ("motor", "slow", "pt"):
        _fail(out_path, f"unsupported type '{rpt_type}', must be motor/slow/pt")
        return

    db_conf_path = os.environ.get("TNA_DB_CONF", "/opt/algorithms/db.conf")
    cfg = _load_db_conf(db_conf_path)

    try:
        conn = _connect(cfg)
    except Exception as e:
        _fail(out_path, f"DB connection failed: {e}")
        return

    new_prefix  = _prefix(pid, uid, cid)
    base_prefix = _prefix(pid, uid, base_cid) if has_compare else None

    try:
        if rpt_type == "motor":
            new_data  = _calc_motor(conn, new_prefix, logs)
            base_data = _calc_motor(conn, base_prefix, logs) if has_compare else None
            bene = _benefit_motor(new_data, base_data)
            if has_compare:
                bene['_base_type_data'] = base_data.get('type_data', [])
            body = _motor_html(new_data, bene, has_compare)
            chart_js = _motor_js(new_data, bene, has_compare)
            type_label = f"{type_cn('motor')}道路改造"
            title = f"{project_label} — 机动车道路改造成本效益分析报告"
        elif rpt_type == "slow":
            new_data  = _calc_slow(conn, new_prefix, logs)
            base_data = _calc_slow(conn, base_prefix, logs) if has_compare else None
            bene = _benefit_slow(new_data, base_data)
            body = _slow_html(new_data, bene, has_compare)
            chart_js = _slow_js(new_data, bene, has_compare)
            type_label = f"{type_cn('slow')}道路改造"
            title = f"{project_label} — 慢行交通改造成本效益分析报告"
        else:
            new_data  = _calc_pt(conn, new_prefix, logs)
            base_data = _calc_pt(conn, base_prefix, logs) if has_compare else None
            bene = _benefit_pt(new_data, base_data)
            body = _pt_html(new_data, bene, has_compare)
            chart_js = _pt_js(new_data, bene, has_compare)
            type_label = f"{type_cn('pt')}线路改造"
            title = f"{project_label} — 公共交通改造成本效益分析报告"
    except Exception as e:
        logs.append(traceback.format_exc())
        _fail(out_path, f"calc failed: {e}")
        return

    map_render = render_map_sections({})
    if rpt_type in ("motor", "slow"):
        try:
            if rpt_type == "slow" and has_compare and base_cid is not None:
                base_bundle, base_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [base_cid], logs, type_filter="slow"
                )
                new_bundle, new_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [cid], logs, type_filter="slow"
                )
                compare_layers = [
                    {
                        "key": "base",
                        "label": f"\u57fa\u7840\u65b9\u6848 case{base_cid}",
                        "case_id": base_cid,
                        "bundle": base_bundle,
                        "pipeline": base_meta.get("pipeline") or {},
                    },
                    {
                        "key": "scheme",
                        "label": f"\u666e\u901a\u65b9\u6848 case{cid}",
                        "case_id": cid,
                        "bundle": new_bundle,
                        "pipeline": new_meta.get("pipeline") or {},
                    },
                ]
                map_render = render_map_sections(
                    new_bundle,
                    pipeline=new_meta.get("pipeline") or {},
                    compare_layers=compare_layers,
                    section_title=f"\u7a7a\u95f4\u5206\u5e03\u5bf9\u6bd4\uff1a\u6162\u884c\u8def\u7f51\u6d41\u91cf\u4e0e OD \u671f\u671b\u7ebf\uff08case{base_cid} vs case{cid}\uff09",
                )
            else:
                fallbacks = [3]
                if has_compare and base_cid is not None and int(base_cid) not in fallbacks:
                    fallbacks.insert(0, int(base_cid))
                map_type_filter = "slow" if rpt_type == "slow" else "all"
                map_bundle, map_meta = collect_map_bundle_for_cases(
                    conn, pid, uid, [cid] + fallbacks, logs, type_filter=map_type_filter
                )
                map_cid = map_meta.get("map_case_id", cid)
                map_note = map_meta.get("map_note") or ""
                pipeline = map_meta.get("pipeline") or {}
                map_render = render_map_sections(map_bundle, pipeline=pipeline)
                if rpt_type == "slow":
                    title_line = f"?? Case{cid} ??????? OD ???"
                else:
                    title_line = f"?? Case{cid} ????? OD ???"
                if map_note:
                    title_line += f"?{map_note}?"
                elif int(map_cid) != int(cid):
                    title_line = f"?? Case{map_cid} ????? OD ???????? case{cid}?"
                map_render["html"] = map_render["html"].replace(
                    "?????????? OD ???",
                    title_line,
                )
        except Exception as exc:
            logs.append(f"??????: {exc}")

    try:
        conn.close()
    except Exception:
        pass

    base_case_label = f"| 现状方案ID: {base_cid}" if has_compare else ""
    gen_time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    data_src = data_sources_line(tables_for_cba(rpt_type))
    data_json = json.dumps({}, ensure_ascii=False)
    map_js_block = f"<script>{map_render.get('js') or ''}</script>" if map_render.get("js") else ""

    html = _HTML_TPL.format(
        title=title,
        project_name=project_label,
        project_id=pid, user_id=uid, case_id=cid,
        base_case_label=base_case_label,
        type_label=type_label,
        gen_time=gen_time,
        data_sources=data_src,
        body=body,
        data_json=data_json,
        chart_js=chart_js,
        references=_REFS.get(rpt_type, ""),
        leaflet_head=map_render.get("leaflet_head") or "",
        map_css=map_render.get("css") or "",
        map_section=map_render.get("html") or "",
        map_js=map_js_block,
    )

    # 内联 ECharts（本地缓存优先，否则 CDN）
    echarts_js = _load_echarts_script()
    if echarts_js:
        html = html.replace("const __ECHARTS_PLACEHOLDER__ = 1;", echarts_js)
    else:
        html = html.replace(
            "const __ECHARTS_PLACEHOLDER__ = 1;",
            "",
        )
        html = html.replace(
            "</head>",
            '<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>\n</head>',
            1,
        )

    Path(output_path).write_text(html, encoding="utf-8")
    logs.append(f"report written: {output_path}")
    _success(out_path, output_path, has_compare=has_compare, cid=cid, base_cid=base_cid, logs=logs)



# ---- override slow cost-benefit renderer: added 2026-07-07 ----

def _slow_html(new_s: dict, bene: dict, has_compare: bool) -> str:
    def fmt(v, nd=1):
        try:
            return _fmt(v, nd)
        except Exception:
            return "-"

    def cls(v, lower_is_better=False):
        try:
            x = float(v or 0)
        except Exception:
            x = 0.0
        if abs(x) < 1e-9:
            return ""
        if lower_is_better:
            return "ok" if x < 0 else "hi"
        return "ok" if x > 0 else "hi"

    base_avg_vc = bene.get("base_avg_vc", 0)
    new_avg_vc = new_s.get("avg_vc", 0)
    delta_vc = bene.get("delta_avg_vc", new_avg_vc - base_avg_vc)
    delta_pmt = bene.get("delta_pmt", 0)
    delta_high = bene.get("delta_high_vc_count", 0)
    delta_cap = bene.get("delta_avg_capacity_slow_adj", 0)
    co2 = bene.get("co2_saved_kg", 0)
    health = bene.get("health_met", 0)
    vc_rate = bene.get("vc_change_rate", 0)

    desc = (
        f"方案慢行网络总 PMT <strong>{fmt(new_s.get('total_pmt', 0), 1)}</strong> 万人·km，"
        f"全网平均流量 <strong>{fmt(new_s.get('avg_flow', 0), 1)}</strong> 人次/h。"
    )
    if has_compare:
        desc += (
            f"相较基础方案，慢行使用量变化 <strong>{delta_pmt:+.1f}</strong> 万人·km，"
            f"按参考因子估算 CO₂ 减排 <strong>{co2:+.1f}</strong> kg/日，"
            f"健康效益 <strong>{health:+.0f}</strong> MET·min。"
        )

    items = [
        ("总 PMT", f"{fmt(new_s.get('total_pmt', 0), 1)} 万人·km", ""),
        ("平均流量", f"{fmt(new_s.get('avg_flow', 0), 1)} 人次/h", ""),
        ("峰值流量", f"{fmt(new_s.get('max_flow', 0), 1)} 人次/h", "hi"),
        ("ΔPMT", f"{delta_pmt:+.1f} 万人·km" if has_compare else "—", cls(delta_pmt, True) if has_compare else ""),
        ("V/C 变化率", f"{vc_rate:+.1f}%" if has_compare else "—", cls(vc_rate, True) if has_compare else ("ok" if new_avg_vc < 0.8 else "hi")),
    ]
    kpi = "".join(
        [f'<div class="kpi {c}"><div class="val">{v}</div><div class="lbl">{n}</div></div>' for n, v, c in items]
    )

    bene_html = ""
    if has_compare:
        bene_html = f"""
<div id="s_bene" class="bene"><h4>慢行出行效益量化摘要（参考《城市步行和自行车交通系统规划设计导则》2013）</h4><p class="ref">本表对比基础方案 case41 与普通方案 case42 的慢行出行效益变化。</p>
<table>
<tr><th>指标</th><th>基础方案</th><th>普通方案</th><th>变化量</th><th>效益</th></tr>
<tr><td>总 PMT（万人·km）</td><td>{fmt(bene.get("base_pmt", 0), 1)}</td><td>{fmt(bene.get("new_pmt", 0), 1)}</td>
    <td class="{cls(delta_pmt, True)}">{delta_pmt:+.1f}</td><td>—</td></tr>
<tr><td>CO₂减排（kg/日）</td><td>—</td><td>—</td><td>—</td><td class="{cls(co2, False)}">{co2:+.1f} kg</td></tr>
<tr><td>健康效益（MET·min）</td><td>—</td><td>—</td><td>—</td><td class="{cls(health, False)}">{health:+.0f}</td></tr>
</table>
<p class="ref">CO₂减排因子取 0.12 kg/km（替代小汽车出行），健康效益取 3.5 MET·min/km（步行），参考 IPCC、WHO 及《城市步行和自行车交通系统规划设计导则》2013。</p>
</div>
<div class="bene"><h4>慢行扩容效果与规划达标补充表</h4><p class="ref">本表用于补充说明普通方案相对基础方案的慢行运行压力、供给能力和规划达标水平变化。</p>
<table>
<tr><th>指标</th><th>基础方案</th><th>普通方案</th><th>变化量</th><th>说明</th></tr>
<tr><td>平均 V/C</td><td>{fmt(base_avg_vc, 3)}</td><td>{fmt(new_avg_vc, 3)}</td>
    <td class="{cls(delta_vc, True)}">{delta_vc:+.3f}</td><td>小于 0 表示运行压力下降</td></tr>
<tr><td>高负荷路段数（V/C≥1）</td><td>{bene.get("base_high_vc_count", 0)}</td><td>{new_s.get("high_vc_count", 0)}</td>
    <td class="{cls(delta_high, True)}">{delta_high:+d}</td><td>小于 0 表示高负荷路段减少</td></tr>
<tr><td>平均影响后通行能力（人次/h）</td><td>{fmt(bene.get("base_avg_capacity_slow_adj", 0), 1)}</td><td>{fmt(new_s.get("avg_capacity_slow_adj", 0), 1)}</td>
    <td class="{cls(delta_cap, False)}">{delta_cap:+.1f}</td><td>大于 0 表示供给能力提升</td></tr>
<tr><td>慢行设施达标长度比例</td><td>{fmt(bene.get("base_facility_standard_pct", 0), 2)}%</td><td>{fmt(new_s.get("facility_standard_pct", 0), 2)}%</td>
    <td class="{cls(bene.get('delta_facility_standard_pct', 0), False)}">{bene.get("delta_facility_standard_pct", 0):+.2f}%</td><td>按需求宽度达标的慢行设施长度占比</td></tr>
<tr><td>慢行安全改善覆盖率</td><td>{fmt(bene.get("base_safety_coverage_pct", 0), 2)}%</td><td>{fmt(new_s.get("safety_coverage_pct", 0), 2)}%</td>
    <td class="{cls(bene.get('delta_safety_coverage_pct', 0), False)}">{bene.get("delta_safety_coverage_pct", 0):+.2f}%</td><td>按物理隔离或安全净距达标路段长度占比</td></tr>
</table>
</div>"""

    compare_chart = (
        '<div class="card"><h3>基础方案 vs 普通方案：核心指标对比</h3>'
        '<div id="c_slow_core" style="width:100%;height:280px"></div></div>'
        if has_compare else ""
    )
    charts = (
        compare_chart
        + '<div class="g2"><div class="card"><h3>各类型慢行道 PMT 构成（普通方案 case42，万人·km）</h3>'
        + '<div id="c_slow_pmt" style="width:100%;height:260px"></div></div>'
        + '<div class="card"><h3>平均流量与峰值流量（普通方案 case42，人次/h）</h3>'
        + '<div id="c_slow_cmp" style="width:100%;height:260px"></div></div></div>'
        + '<div class="card"><h3>高流量路段 TOP10（普通方案 case42，人次/h）</h3>'
        + '<div id="c_slow_top" style="width:100%;height:240px"></div></div>'
    )

    return (
        '<div id="s_main" class="section"><div class="sec-title">慢行道路改造 — 核心指标</div>'
        f'<div class="desc green">{desc}</div><div class="kpi-row">{kpi}</div>{bene_html}</div>'
        f'<div id="s_detail" class="section"><div class="sec-title">详细分析</div><div class="desc">说明：核心指标图为基础方案 case41 与普通方案 case42 的对比；其余 PMT 构成、流量和 TOP10 图为普通方案 case42 的单方案结果。</div>{charts}</div>'
    )

# ---- end final combined slow CBA html override ----

if __name__ == "__main__":
    main()
