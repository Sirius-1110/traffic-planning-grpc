"""诊断指标取值、层级过滤与合理性校验 — 各报告桥接共用。"""
from __future__ import annotations

from typing import Any

# 各宏观指标优先 result_key（context 一般为 GLOBAL）
PRIMARY_RESULT_KEY: dict[str, list[str]] = {
    "road_macro_density": ["overall", "全网统归"],
    "road_macro_tpi": ["overall", "tpi"],
    "road_macro_congestion_ratio": ["overall", "congestion"],
    "road_macro_average_speed": ["overall", "speed"],
    "road_macro_tti": ["overall", "tti"],
    "road_macro_dti": ["overall", "dti"],
    "slow_macro_walk_density": ["overall"],
    "slow_macro_bike_density": ["overall"],
    "slow_macro_walk_continuity": ["overall"],
    "slow_macro_bike_continuity": ["overall"],
    "slow_macro_area_percap": ["overall"],
    "slow_macro_vc_distribution": ["overall"],
    "slow_macro_capacity_impact": ["overall"],
    "pt_macro_bus_density": ["overall"],
    "pt_macro_pt_pop_coverage_500m": ["500m", "overall"],
    "pt_macro_city_bus_area_coverage": ["500m", "300m", "overall"],
    "pt_macro_rail_pop_coverage_800m": ["800m", "overall"],
}

# 同一 result_key 多行时，指定 result_item（如覆盖率取 rate，密度取 density）
PRIMARY_RESULT_ITEM: dict[str, list[str]] = {
    "road_macro_density": ["density", "value"],
    "road_macro_tpi": ["value", "tpi"],
    "road_macro_congestion_ratio": ["value", "congestion"],
    "road_macro_average_speed": ["value", "speed"],
    "road_macro_tti": ["value", "tti"],
    "road_macro_dti": ["value", "dti"],
    "slow_macro_walk_density": ["density"],
    "slow_macro_bike_density": ["density"],
    "slow_macro_walk_continuity": ["total_length_km"],
    "slow_macro_bike_continuity": ["total_length_km"],
    "slow_macro_area_percap": ["area_per_capita_m2"],
    "slow_macro_vc_distribution": ["overall"],
    "slow_macro_capacity_impact": ["affected_ratio_pct", "affected_links"],
    "pt_macro_pt_pop_coverage_500m": ["rate"],
    "pt_macro_rail_pop_coverage_800m": ["rate"],
    "pt_macro_bus_density": ["density"],
    "pt_macro_city_bus_area_coverage": ["rate"],
}

MAX_PLAUSIBLE_SPEED_KMH = 180.0
MAX_PLAUSIBLE_DENSITY_KM2 = 500.0

TYPE_PREFIX = {"motor": "road_", "slow": "slow_", "pt": "pt_"}

# 入库辅助行，不得作为报告展示值
_SKIP_RESULT_ITEMS: dict[str, frozenset[str]] = {
    "pt_macro_pt_pop_coverage_500m": frozenset({"total_job", "total_pop", "covered_pop", "job_count"}),
    "pt_macro_rail_pop_coverage_800m": frozenset({"total_pop", "covered_pop"}),
    "pt_macro_city_bus_area_coverage": frozenset({"total_area", "covered_area"}),
    "slow_macro_bike_density": frozenset({"region_area_km2", "area_km2", "region_area"}),
    "slow_macro_walk_density": frozenset({"region_area_km2", "area_km2", "region_area"}),
}


def indicator_tier(code: str) -> str:
    c = (code or "").strip()
    if "_macro_" in c:
        return "macro"
    if any(x in c for x in ("_micro_", "_meso_", "road_micro", "slow_micro", "pt_meso")):
        return "meso_micro"
    return "other"


def row_item(r: dict[str, Any]) -> str:
    return (r.get("result_item") or r.get("result_key") or "").strip()


def global_rows(rows: list[dict]) -> list[dict]:
    g = [r for r in rows if (r.get("context_key") or "").upper() == "GLOBAL"]
    return g if g else list(rows)


def is_plausible_value(code: str, num: float | None, unit: str = "") -> bool:
    if num is None:
        return True
    try:
        v = float(num)
    except (TypeError, ValueError):
        return False
    if code in (
        "road_macro_average_speed",
        "road_micro_speed_kmh",
    ):
        return 0 <= v <= MAX_PLAUSIBLE_SPEED_KMH
    if code in (
        "road_macro_density",
        "slow_macro_walk_density",
        "slow_macro_bike_density",
    ):
        return 0 <= v <= MAX_PLAUSIBLE_DENSITY_KM2
    u = (unit or "").strip()
    if code in ("pt_macro_pt_pop_coverage_500m", "pt_macro_rail_pop_coverage_800m", "pt_macro_city_bus_area_coverage"):
        if "%" in u or row_item({"result_item": "rate"}) == "rate":
            return 0 <= v <= 100.0
    if code == "slow_macro_area_percap" and v < 0:
        return False
    return True


def pick_primary_value(code: str, rows: list[dict], *, label: str | None = None) -> dict[str, Any]:
    """提取单指标代表值；不合理数值视为缺失。"""
    item_prefs = PRIMARY_RESULT_ITEM.get(code) or []
    prefs = PRIMARY_RESULT_KEY.get(code, ["overall"])
    lbl = label or code

    def pack(r: dict, key: str, num: float) -> dict[str, Any]:
        unit = (r.get("result_unit") or "").strip()
        if not is_plausible_value(code, num, unit):
            return {"code": code, "label": lbl, "key": key, "num": None, "unit": unit, "text": ""}
        return {
            "code": code,
            "label": lbl,
            "key": key,
            "num": num,
            "unit": unit,
            "text": r.get("result_value_text") or "",
        }

    for item in item_prefs:
        for r in global_rows(rows):
            if row_item(r) == item and r.get("result_value_num") is not None:
                return pack(r, item, float(r["result_value_num"]))
    for rk in prefs:
        for r in global_rows(rows):
            if (r.get("result_key") or "") == rk and r.get("result_value_num") is not None:
                if item_prefs and row_item(r) not in item_prefs:
                    continue
                return pack(r, rk, float(r["result_value_num"]))
    for r in global_rows(rows):
        if r.get("result_value_num") is not None:
            u = (r.get("result_unit") or "").strip()
            if "%" in u:
                return pack(r, r.get("result_key") or "", float(r["result_value_num"]))
    for r in global_rows(rows):
        if r.get("result_value_num") is not None:
            return pack(r, r.get("result_key") or "", float(r["result_value_num"]))
    skip_text = frozenset({"meets_standard", "overall"})
    txt = next(
        (
            r.get("result_value_text")
            for r in global_rows(rows)
            if r.get("result_value_text") and row_item(r) not in skip_text
        ),
        "",
    )
    return {"code": code, "label": lbl, "key": "", "num": None, "unit": "", "text": txt}


def filter_rows_for_report(
    rows: list[dict],
    *,
    scope: str = "macro",
    type_filter: str = "all",
) -> list[dict]:
    """报告默认仅宏观；仅 scope=meso_micro 或 all 时含中微观。"""
    scope = (scope or "macro").strip().lower()
    out: list[dict] = []
    for r in rows:
        code = (r.get("indicator_code") or "").strip()
        if not code:
            continue
        tier = indicator_tier(code)
        if scope == "macro" and tier != "macro":
            continue
        if scope == "meso_micro" and tier != "meso_micro":
            continue
        if type_filter != "all":
            pref = TYPE_PREFIX.get(type_filter, "")
            if pref and not code.startswith(pref):
                continue
        if row_item(r) in _SKIP_RESULT_ITEMS.get(code, frozenset()):
            continue
        num = r.get("result_value_num")
        if num is not None and not is_plausible_value(
            code, float(num), (r.get("result_unit") or "")
        ):
            continue
        out.append(r)
    return out


def group_by_indicator(rows: list[dict]) -> dict[str, list[dict]]:
    groups: dict[str, list[dict]] = {}
    for r in rows:
        code = r.get("indicator_code") or ""
        groups.setdefault(code, []).append(r)
    return groups


def macro_summary_rows(
    groups: dict[str, list[dict]],
    *,
    labels: dict[str, str] | None = None,
) -> list[dict]:
    """每个宏观指标一行代表值，供基础数据分析/明细表。"""
    labels = labels or {}
    out: list[dict] = []
    for code in sorted(groups.keys()):
        if indicator_tier(code) != "macro":
            continue
        p = pick_primary_value(code, groups[code], label=labels.get(code, code))
        if p["num"] is None and not p.get("text"):
            continue
        out.append(
            {
                "code": code,
                "ctx": "GLOBAL",
                "key": p["key"],
                "item": p["key"],
                "num": p["num"],
                "text": p.get("text") or "",
                "unit": p.get("unit") or "",
                "tier": "macro",
            }
        )
    return out


def format_display_num(num: float | None, unit: str = "", *, digits: int = 4) -> str:
    if num is None:
        return "—"
    av = abs(num)
    if av >= 1e6:
        s = f"{num:.3e}"
    elif av >= 100:
        s = f"{num:.1f}"
    else:
        s = f"{num:.{digits}f}"
    return f"{s}{(' ' + unit) if unit else ''}"
