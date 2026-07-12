"""诊断/报告指标：中文含义、计算公式、参考范围、单位说明。"""
from __future__ import annotations

from typing import Any

# module: road | slow | pt
INDICATOR_META: dict[str, dict[str, str]] = {
    "road_macro_density": {
        "label": "路网密度",
        "formula": "机动车路网里程(type=1) ÷ 研究区TAZ面积",
        "reference": "常见城市 2~8 km/km²；>50 需核对面积单位",
        "unit": "km/km²",
    },
    "road_macro_tpi": {
        "label": "交通绩效指数 TPI",
        "formula": "按道路等级判定低速路段里程占比 φ，TPI=min(10, φ/0.30×10)，全网按VKT加权",
        "reference": "0~10 分；≤3 较好，≥7 较差",
        "unit": "分",
    },
    "road_macro_congestion_ratio": {
        "label": "拥堵里程比例",
        "formula": "满足 V/C>0.8 或 实际速度÷自由流速度<0.5 的路段里程 ÷ 有流量路段总里程 × 100%",
        "reference": "大城市高峰 10%~30%；与 DTI、速度比、V/C 分档口径一致",
        "unit": "%",
    },
    "road_macro_average_speed": {
        "label": "平均行程速度",
        "formula": "全网 Σ(流量×路段长度) ÷ Σ(流量×行程时间)",
        "reference": "城区主干 25~45 km/h；自由流可达 50+ km/h",
        "unit": "km/h",
    },
    "road_macro_tti": {
        "label": "行程时间指数 TTI",
        "formula": "Σ(流量×实际行程时间) ÷ Σ(流量×自由流时间)",
        "reference": "1.0=自由流；1.2~1.5 轻度拥堵；>2 严重",
        "unit": "",
    },
    "road_macro_dti": {
        "label": "延误时间指数 DTI",
        "formula": "max(0, (VHT−VHT_free)/VHT_free) × 100%",
        "reference": "0%=无延误；>20% 延误明显",
        "unit": "%",
    },
    "road_macro_speed_relation": {
        "label": "速度比（实际/自由流）",
        "formula": "流量加权平均 min(1, 实际速度/自由流速度) × 100%",
        "reference": "80%~100% 运行较好；<60% 拥堵严重",
        "unit": "%",
    },
    "road_macro_capacity_distribution": {
        "label": "通行能力分布（饱和度分档）",
        "formula": "按 V/C 分档统计各档路段里程占比：畅通<0.6、基本畅通0.6~0.8、拥挤0.8~1.0、严重≥1.0",
        "reference": "四档合计应≈100%；严重拥堵档宜 <15%",
        "unit": "%（分档）",
    },
    "road_micro_flow": {
        "label": "路段流量",
        "formula": "指定路段分配流量 flow",
        "reference": "依道路等级；次干道常见 500~3000 pcu/h",
        "unit": "veh/h",
    },
    "road_micro_speed_kmh": {
        "label": "路段运行速度",
        "formula": "路段长度 ÷ 实际行程时间",
        "reference": "与城市等级、时段相关",
        "unit": "km/h",
    },
    "road_micro_v_c": {
        "label": "路段 V/C",
        "formula": "路段流量 ÷ 通行能力",
        "reference": "<0.8 较畅通；≥1.0 过饱和",
        "unit": "",
    },
    "road_micro_tti": {
        "label": "路段 TTI",
        "formula": "路段实际行程时间 ÷ 自由流行程时间",
        "reference": "≈1 为自由流",
        "unit": "",
    },
    "road_micro_dti": {
        "label": "路段 DTI",
        "formula": "路段延误时间比 × 100%",
        "reference": "0% 为无延误",
        "unit": "%",
    },
    "road_micro_source_tracing": {
        "label": "路段溯源流量",
        "formula": "OD 溯源至该路段的流量汇总",
        "reference": "应 ≤ 路段总流量",
        "unit": "veh/h",
    },
    "slow_macro_walk_density": {
        "label": "步行网络密度",
        "formula": "步行道里程 ÷ 研究区面积",
        "reference": "常见 2~15 km/km²",
        "unit": "km/km²",
    },
    "slow_macro_bike_density": {
        "label": "骑行网络密度",
        "formula": "非机动车道里程 ÷ 研究区面积",
        "reference": "常见 1~10 km/km²",
        "unit": "km/km²",
    },
    "slow_macro_walk_continuity": {
        "label": "步行网络连续度",
        "formula": "步行连通路段总长度（非占比）",
        "reference": "越长表示连通性越好；看绝对值与路网规模",
        "unit": "km",
    },
    "slow_macro_bike_continuity": {
        "label": "骑行网络连续度",
        "formula": "骑行连通路段总长度",
        "reference": "同上",
        "unit": "km",
    },
    "slow_macro_area_percap": {
        "label": "人均慢行空间",
        "formula": "慢行用地面积 ÷ 常住人口",
        "reference": "常见 2~10 m²/人",
        "unit": "m²/人",
    },
    "slow_macro_vc_distribution": {
        "label": "慢行 V/C 分档分布",
        "formula": "按慢行路段 V/C 分档统计里程占比",
        "reference": "分档占比合计应≈100%",
        "unit": "%（分档）",
    },
    "slow_macro_capacity_impact": {
        "label": "慢行通行能力影响程度",
        "formula": "基于原始通行能力与慢行影响后通行能力，统计受影响路段占比、平均/最大通行能力下降",
        "reference": "受影响占比越高、下降幅度越大，说明非机动车/慢行影响对路网容量约束越明显",
        "unit": "% / 人次/h",
    },
    "slow_meso_walk_density": {
        "label": "慢行中观-步行网络密度",
        "formula": "局部区域步行道里程 ÷ 区域面积",
        "reference": "宏观密度的空间分解",
        "unit": "km/km²",
    },
    "slow_meso_bike_density": {
        "label": "慢行中观-骑行网络密度",
        "formula": "局部区域非机动车道里程 ÷ 区域面积",
        "reference": "同上",
        "unit": "km/km²",
    },
    "slow_meso_walk_continuity": {
        "label": "慢行中观-步行连续度",
        "formula": "局部区域步行连通长度或连通比例",
        "reference": "看分区对比",
        "unit": "% 或 km",
    },
    "slow_meso_bike_continuity": {
        "label": "慢行中观-骑行连续度",
        "formula": "局部区域骑行连通长度或比例",
        "reference": "看分区对比",
        "unit": "% 或 km",
    },
    "slow_meso_area_percap": {
        "label": "慢行中观-人均慢行空间",
        "formula": "分区慢行用地 ÷ 分区人口",
        "reference": "2~10 m²/人",
        "unit": "m²/人",
    },
    "slow_micro_vc": {
        "label": "慢行微观 V/C",
        "formula": "慢行路段流量 ÷ 通行能力",
        "reference": "<0.8 较宽松",
        "unit": "",
    },
    "slow_micro_topn_vc": {
        "label": "慢行微观 TOP-N 高 V/C",
        "formula": "V/C 最高的前 N 条慢行路段",
        "reference": "用于识别瓶颈",
        "unit": "",
    },
    "slow_micro_capacity_impact_topn": {
        "label": "慢行微观 TOP-N 容量影响路段",
        "formula": "按通行能力下降值排序，结合分配流量计算影响后 V/C",
        "reference": "用于识别容量下降明显且运行压力较高的优先治理路段",
        "unit": "",
    },
    "pt_macro_bus_density": {
        "label": "公交线路密度",
        "formula": "公交线网总里程 ÷ 研究区面积",
        "reference": "常见 0.5~5 km/km²；异常大数需核对 trip_mile/area 单位",
        "unit": "km/km²",
    },
    "pt_macro_pt_pop_coverage_500m": {
        "label": "公交 500m 人口覆盖率",
        "formula": "站点500m缓冲区内人口+岗位 ÷ 总人口+总岗位 × 100%",
        "reference": "60%~90% 为较好水平",
        "unit": "%",
    },
    "pt_macro_city_bus_area_coverage": {
        "label": "公交站点用地覆盖率",
        "formula": "站点服务覆盖面积 ÷ 研究区面积 × 100%",
        "reference": "看 300m/500m 缓冲；宜 30%~70%",
        "unit": "%",
    },
    "pt_macro_rail_pop_coverage_800m": {
        "label": "轨道 800m 人口覆盖率",
        "formula": "轨道站点800m内人口+岗位覆盖率",
        "reference": "轨道城市常见 20%~50%",
        "unit": "%",
    },
    "pt_meso_total_routes": {
        "label": "公交线路条数",
        "formula": "bus_route 表线路计数",
        "reference": "视城市规模",
        "unit": "条",
    },
    "pt_meso_avg_mileage": {
        "label": "线路平均里程",
        "formula": "AVG(trip_mile)",
        "reference": "常见 10~25 km/条",
        "unit": "km",
    },
    "pt_meso_avg_stations": {
        "label": "线路平均站点数",
        "formula": "AVG(站点数)",
        "reference": "常见 15~30 站/条",
        "unit": "个/条",
    },
    "pt_meso_max_mileage": {
        "label": "线路最大里程",
        "formula": "MAX(trip_mile)",
        "reference": "识别超长线路",
        "unit": "km",
    },
    "pt_meso_max_stations": {
        "label": "线路最大站点数",
        "formula": "MAX(站点数)",
        "reference": "—",
        "unit": "个",
    },
}

CBA_MOTOR_GLOSSARY: list[dict[str, str]] = [
    {
        "name": "VMT（车辆行驶里程）",
        "meaning": "全网车辆-公里累计，反映出行总强度与路网负荷",
        "formula": "Σ(路段流量 × 路段长度)",
        "reference": "与研究区规模正相关；改造前后宜同口径对比",
        "unit": "万 pcu·km",
    },
    {
        "name": "VHT（车辆行程时间）",
        "meaning": "全网在车时间累计，反映拥堵与可达性成本",
        "formula": "Σ(路段流量 × 路段行程时间)",
        "reference": "下降表示出行时间节约；是成本效益核心量",
        "unit": "万 pcu·h",
    },
    {
        "name": "过饱和路段数",
        "meaning": "V/C≥1.0 的路段条数，反映拥堵瓶颈规模",
        "formula": "COUNT(路段 WHERE v_c≥1)",
        "reference": "越少越好；改造应力争下降",
        "unit": "条",
    },
    {
        "name": "ΔVHT / 时间效益",
        "meaning": "改造相对现状节省（或增加）的在车时间货币化",
        "formula": "ΔVHT × 时间价值(VOT)",
        "reference": "参考《建设项目经济评价方法与参数》；正效益为节约时间",
        "unit": "元/日",
    },
]


def get_indicator_meta(code: str) -> dict[str, str]:
    return INDICATOR_META.get(code, {"label": code, "formula": "", "reference": "", "unit": ""})


# 雷达图维度：good/bad 为参考区间端点，direction 表示「越高越好」或「越低越好」
RADAR_DIMENSIONS: list[dict[str, Any]] = [
    {"code": "road_macro_tpi", "name": "交通绩效TPI", "direction": "lower", "good": 3.0, "bad": 8.0},
    {"code": "road_macro_average_speed", "name": "运行速度", "direction": "higher", "good": 25.0, "bad": 45.0},
    {"code": "road_macro_tti", "name": "行程畅通度", "direction": "lower", "good": 1.2, "bad": 2.5},
    {"code": "road_macro_dti", "name": "延误控制", "direction": "lower", "good": 20.0, "bad": 100.0},
    {"code": "road_macro_speed_relation", "name": "速度保持率", "direction": "higher", "good": 65.0, "bad": 90.0},
    {"code": "road_macro_congestion_ratio", "name": "拥堵里程控制", "direction": "lower", "good": 5.0, "bad": 25.0},
]


def norm_score_by_range(
    value: float | None,
    *,
    good: float,
    bad: float,
    direction: str = "higher",
) -> float:
    """将指标映射到 0–100，100 表示参考意义上「越好」。"""
    if value is None:
        return 0.0
    if good == bad:
        return 50.0
    if direction == "lower":
        if value <= good:
            return 100.0
        if value >= bad:
            return 0.0
        return max(0.0, min(100.0, (bad - value) / (bad - good) * 100.0))
    if value >= bad:
        return 100.0
    if value <= good:
        return 0.0
    return max(0.0, min(100.0, (value - good) / (bad - good) * 100.0))


def assess_indicator_level(code: str, num: float | None) -> tuple[str, str]:
    """按参考范围给出评价标签与色调（good|warn|bad|neutral）。"""
    if num is None:
        return "—", "neutral"

    rules: dict[str, tuple[str, str]] = {
        "road_macro_tpi": (
            ("优", "good") if num <= 3 else
            ("中", "warn") if num <= 7 else
            ("差", "bad")
        ),
        "road_macro_congestion_ratio": (
            ("优", "good") if num < 5 else
            ("良", "good") if num < 10 else
            ("中", "warn") if num < 25 else
            ("差", "bad")
        ),
        "road_macro_tti": (
            ("优", "good") if num <= 1.2 else
            ("良", "good") if num <= 1.5 else
            ("中", "warn") if num <= 2.0 else
            ("差", "bad")
        ),
        "road_macro_dti": (
            ("优", "good") if num <= 20 else
            ("良", "good") if num <= 50 else
            ("差", "bad")
        ),
        "road_macro_speed_relation": (
            ("优", "good") if num >= 80 else
            ("良", "good") if num >= 60 else
            ("差", "bad")
        ),
        "road_macro_average_speed": (
            ("优", "good") if num >= 35 else
            ("良", "good") if num >= 25 else
            ("差", "bad")
        ),
        "road_macro_density": (
            ("良", "good") if 2.0 <= num <= 8.0 else
            ("中", "warn")
        ),
        "pt_macro_pt_pop_coverage_500m": (
            ("优", "good") if num >= 80 else
            ("良", "good") if num >= 60 else
            ("中", "warn") if num >= 40 else
            ("差", "bad")
        ),
    }
    return rules.get(code, ("—", "neutral"))


def format_distribution_value(code: str, rows: list[dict]) -> tuple[str, str]:
    """多档分布类指标格式化为「档名 值%」拼接（报告卡片区已弃用，仅作后备）。"""
    import math
    from diagnosis_value_utils import preferred_macro_rows, row_item

    dist_codes = {
        "road_macro_capacity_distribution",
        "slow_macro_vc_distribution",
    }
    if code not in dist_codes:
        return "", ""
    parts = []
    for r in preferred_macro_rows(rows, code):
        num = r.get("result_value_num")
        if num is None:
            continue
        try:
            v = float(num)
        except (TypeError, ValueError):
            continue
        if math.isnan(v):
            continue
        name = row_item(r) or r.get("result_key") or ""
        if name in ("value", "overall"):
            continue
        parts.append(f"{name} {v:.1f}%")
    if not parts:
        return "—", ""
    return " / ".join(parts[:6]), "%（分档）"


def enrich_indicator_card(code: str, card: dict[str, Any], rows: list[dict]) -> dict[str, Any]:
    meta = get_indicator_meta(code)
    card = dict(card)
    card["label"] = meta.get("label") or card.get("label", code)
    card["formula"] = meta.get("formula", "")
    card["reference"] = meta.get("reference", "")
    dist_val, dist_unit = format_distribution_value(code, rows)
    if dist_val:
        card["value"] = dist_val
        card["unit"] = dist_unit
    elif meta.get("unit") and not card.get("unit"):
        card["unit"] = meta["unit"]
    # 连续度误标为 % 的修正展示
    if code in ("slow_macro_walk_continuity", "slow_macro_bike_continuity") and card.get("unit") == "%":
        card["unit"] = "km"
    if code == "pt_macro_city_bus_area_coverage" and (card.get("unit") or "").find("km") >= 0:
        card["unit"] = "%"
    card["footnote"] = f"计算：{card['formula']}" if card.get("formula") else ""
    if card.get("reference"):
        card["footnote"] = (card.get("footnote") or "") + f"　参考：{card['reference']}"
    return card
