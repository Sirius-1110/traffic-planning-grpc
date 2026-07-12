"""AI 智能诊断 — 常用诊断指标目录（按方案类型 / scope 分组）。"""
from __future__ import annotations

from typing import Any

# 从 diagnosis_* RPC 已计算指标中摘取的常用子集（软件第二项下拉）
COMMON_INDICATORS: dict[str, list[dict[str, str]]] = {
    "motor": [
        {"code": "road_macro_density", "name": "路网密度", "level": "宏观"},
        {"code": "road_macro_tpi", "name": "交通绩效指数 TPI", "level": "宏观"},
        {"code": "road_macro_congestion_ratio", "name": "拥堵里程比例", "level": "宏观"},
        {"code": "road_macro_average_speed", "name": "平均行驶速度", "level": "宏观"},
        {"code": "road_macro_tti", "name": "行程时间指数 TTI", "level": "宏观"},
        {"code": "road_macro_dti", "name": "延误时间指数 DTI", "level": "宏观"},
        {"code": "road_macro_co2", "name": "道路交通碳排放", "level": "宏观"},
        {"code": "road_micro_v_c", "name": "路段饱和度 V/C", "level": "中微观"},
        {"code": "road_micro_flow", "name": "路段流量", "level": "中微观"},
        {"code": "road_micro_speed_kmh", "name": "路段运行车速", "level": "中微观"},
        {"code": "road_micro_tti", "name": "路段 TTI", "level": "中微观"},
    ],
    "slow": [
        {"code": "slow_macro_walk_density", "name": "步行网络密度", "level": "宏观"},
        {"code": "slow_macro_bike_density", "name": "非机动车网络密度", "level": "宏观"},
        {"code": "slow_macro_walk_continuity", "name": "步行网络连续性", "level": "宏观"},
        {"code": "slow_macro_bike_continuity", "name": "非机动车连续性", "level": "宏观"},
        {"code": "slow_macro_area_percap", "name": "人均慢行面积", "level": "宏观"},
        {"code": "slow_meso_walk_density", "name": "片区步行密度", "level": "中观"},
        {"code": "slow_meso_bike_density", "name": "片区骑行密度", "level": "中观"},
        {"code": "slow_micro_vc", "name": "慢行路段饱和度", "level": "微观"},
        {"code": "slow_micro_topn_vc", "name": "慢行 TopN 饱和路段", "level": "微观"},
    ],
    "pt": [
        {"code": "pt_macro_bus_density", "name": "公交线网密度", "level": "宏观"},
        {"code": "pt_macro_pt_pop_coverage_500m", "name": "公交 500m 人口覆盖率", "level": "宏观"},
        {"code": "pt_macro_city_bus_area_coverage", "name": "公交区域覆盖率", "level": "宏观"},
        {"code": "pt_macro_infeasible_ratio", "name": "暂不可服务需求比例", "level": "宏观"},
        {"code": "pt_macro_avg_path_cost", "name": "平均出行时间成本", "level": "宏观"},
        {"code": "pt_macro_avg_wait_cost", "name": "平均等待时间", "level": "宏观"},
        {"code": "pt_macro_wait_cost_share", "name": "等待时间占比", "level": "宏观"},
        {"code": "pt_macro_max_stop_link_vc", "name": "最高站间饱和度", "level": "宏观"},
        {"code": "pt_macro_high_vc_stop_link_ratio", "name": "高负荷站间占比", "level": "宏观"},
        {"code": "pt_macro_top_link_flow", "name": "最高断面客流", "level": "宏观"},
        {"code": "pt_macro_hyperpath_count", "name": "公交出行路径数量", "level": "宏观"},
        {"code": "pt_meso_total_routes", "name": "线路条数", "level": "中观"},
        {"code": "pt_meso_avg_mileage", "name": "平均线路里程", "level": "中观"},
        {"code": "pt_meso_avg_stations", "name": "平均站点数", "level": "中观"},
        {"code": "pt_meso_max_mileage", "name": "最长线路里程", "level": "中观"},
        {"code": "pt_meso_avg_stop_spacing", "name": "平均站间距", "level": "中观"},
        {"code": "pt_meso_avg_frequency", "name": "平均发车频率", "level": "中观"},
        {"code": "pt_meso_active_routes", "name": "有客流线路数", "level": "中观"},
    ],
}


def list_common_indicators(scope: str) -> list[dict[str, str]]:
    from .scope_utils import normalize_scope

    s = normalize_scope(scope)
    if s == "all":
        out: list[dict[str, str]] = []
        for mode in ("motor", "slow", "pt"):
            for item in COMMON_INDICATORS[mode]:
                row = dict(item)
                row["mode"] = mode
                out.append(row)
        return out
    return list(COMMON_INDICATORS.get(s, []))


def indicator_method_label(code: str) -> str:
    """指标 code → 面向用户的中文诊断方法名（建议文案禁止写 RPC/接口名）。"""
    for mode, items in COMMON_INDICATORS.items():
        for item in items:
            if item["code"] == code:
                name = item["name"]
                level = item.get("level", "宏观")
                mode_cn = {"motor": "机动车", "slow": "慢行", "pt": "公交"}.get(mode, "")
                return f"{mode_cn}{level}「{name}」诊断"
    if code.startswith("road_macro"):
        return "机动车宏观诊断"
    if code.startswith("slow_"):
        return "慢行宏观/中微观诊断"
    if code.startswith("pt_"):
        return "公交宏观/中微观诊断"
    return "对应交通方式宏观诊断"


def resolve_indicator_codes(
    prompt_blocks: dict[str, Any] | None,
    scope: str,
) -> list[str] | None:
    """
    解析软件第二项所选指标。
    未传或空列表 → None（加载该 scope 下全部已计算指标，最多 diagnosis_limit 条）。
    """
    pb = prompt_blocks or {}
    raw = pb.get("indicator_codes") or pb.get("selected_indicators") or pb.get("indicators")
    if raw is None:
        return None
    if isinstance(raw, str):
        raw = [x.strip() for x in raw.replace("，", ",").split(",") if x.strip()]
    if not isinstance(raw, (list, tuple)):
        return None
    codes = [str(x).strip() for x in raw if str(x).strip()]
    if not codes:
        return None
    # 校验：仅保留目录中该 scope 允许的 code（未知 code 仍保留，便于扩展）
    allowed = {i["code"] for i in list_common_indicators(scope)}
    return [c for c in codes if c in allowed] or codes
