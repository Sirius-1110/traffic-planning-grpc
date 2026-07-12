"""报告/UI 数据表中文名与数据来源文案（各 bridge 共用）。"""
from __future__ import annotations

TABLE_CN: dict[str, str] = {
    "road_way": "机动车路段表",
    "road_point": "路网节点表",
    "greedy_link_flow_results": "机动车分配流量表",
    "slow_road_way": "慢行路段表",
    "slow_greedy_link_flow_results": "慢行分配流量表",
    "slow_road_community": "慢行交通小区表",
    "slow_other_od": "慢行OD矩阵表",
    "pt_route": "公交线路表",
    "pt_stop": "公交站点表",
    "pt_link_result": "公交分配链路表",
    "other_bus_route": "公交线路属性表",
    "road_community": "交通小区表",
    "other_od": "OD矩阵表",
    "diagnosis_indicator_result_rows": "诊断指标结果表",
}

COLUMN_CN: dict[str, str] = {
    "volume": "流量",
    "v_c": "饱和度",
    "flow": "流量",
    "trip_mile": "线路里程",
}

TYPE_CN: dict[str, str] = {
    "motor": "机动车",
    "slow": "慢行",
    "pt": "公共交通",
    "all": "全方式",
}

SCHEME_COMPARE_TABLES: dict[str, list[str]] = {
    "motor": ["road_way"],
    "slow": ["slow_road_way", "slow_greedy_link_flow_results"],
    "pt": ["pt_route", "pt_stop", "pt_link_result"],
    "all": [
        "road_way",
        "slow_road_way",
        "slow_greedy_link_flow_results",
        "pt_route",
        "pt_stop",
        "pt_link_result",
    ],
}

CBA_TABLES: dict[str, list[str]] = {
    "motor": ["road_way"],
    "slow": ["slow_road_way", "slow_greedy_link_flow_results"],
    "pt": ["pt_route", "pt_stop", "pt_link_result"],
}

DIAGNOSIS_TABLES: dict[str, list[str]] = {
    "motor": ["diagnosis_indicator_result_rows", "road_way", "greedy_link_flow_results"],
    "slow": [
        "diagnosis_indicator_result_rows",
        "slow_road_way",
        "slow_greedy_link_flow_results",
        "slow_other_od",
    ],
    "pt": [
        "diagnosis_indicator_result_rows",
        "pt_route",
        "pt_stop",
        "pt_link_result",
        "other_bus_route",
    ],
    "all": [
        "diagnosis_indicator_result_rows",
        "road_way",
        "greedy_link_flow_results",
        "slow_road_way",
        "slow_greedy_link_flow_results",
        "pt_route",
        "pt_link_result",
    ],
}

BASE_REPORT_TABLES: list[str] = [
    "road_way",
    "slow_road_way",
    "slow_greedy_link_flow_results",
    "pt_route",
    "pt_link_result",
    "other_bus_route",
    "road_community",
    "diagnosis_indicator_result_rows",
]


def table_cn(suffix: str) -> str:
    return TABLE_CN.get(suffix, suffix)


def field_cn(table_suffix: str, column: str) -> str:
    col = COLUMN_CN.get(column, column)
    return f"{table_cn(table_suffix)}·{col}"


def type_cn(value: str) -> str:
    return TYPE_CN.get(str(value).lower(), value)


def tables_cn_join(suffixes: list[str]) -> str:
    return "、".join(f"「{table_cn(s)}」" for s in suffixes)


def data_sources_line(suffixes: list[str]) -> str:
    return f"数据来源：{tables_cn_join(suffixes)}"


def table_missing_warning(suffix: str, detail: str = "") -> str:
    msg = f"未找到「{table_cn(suffix)}」"
    if detail:
        msg += f"（{detail}）"
    return msg


def tables_missing_warning(suffixes: list[str], detail: str = "") -> str:
    names = "、".join(f"「{table_cn(s)}」" for s in suffixes)
    msg = f"未找到 {names}"
    if detail:
        msg += f"（{detail}）"
    return msg


def humanize_db_message(msg: str) -> str:
    """将 PG 报错中的英文表名片段替换为中文（用户可见日志）。"""
    out = str(msg)
    for suffix, cn in sorted(TABLE_CN.items(), key=lambda x: -len(x[0])):
        out = out.replace(suffix, cn)
    return out


def tables_for_scheme_compare(cmp_type: str) -> list[str]:
    return list(SCHEME_COMPARE_TABLES.get(str(cmp_type).lower(), SCHEME_COMPARE_TABLES["all"]))


def tables_for_cba(rpt_type: str) -> list[str]:
    return list(CBA_TABLES.get(str(rpt_type).lower(), CBA_TABLES["motor"]))


def tables_for_diagnosis(type_filter: str) -> list[str]:
    return list(DIAGNOSIS_TABLES.get(str(type_filter).lower(), DIAGNOSIS_TABLES["all"]))
