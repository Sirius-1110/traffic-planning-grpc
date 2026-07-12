"""Table registry used by the AI diagnosis data collector."""
from __future__ import annotations

TABLE_SPECS: list[dict[str, str]] = [
    {"suffix": "road_way", "category": "motor", "label": "机动车路网"},
    {"suffix": "other_od", "category": "motor", "label": "机动车 OD"},
    {"suffix": "other_observation", "category": "motor", "label": "路段观测流量"},
    {"suffix": "greedy_iteration_history", "category": "motor", "label": "机动车迭代历史"},
    {"suffix": "greedy_link_flow_results", "category": "motor", "label": "机动车路段流量结果"},
    {"suffix": "greedy_path_results", "category": "motor", "label": "机动车路径结果"},
    {"suffix": "greedy_summary_statistics", "category": "motor", "label": "机动车统计摘要"},
    {"suffix": "iteration_record", "category": "motor", "label": "迭代记录"},
    {"suffix": "link_flow_results", "category": "motor", "label": "链路流量"},
    {"suffix": "od_estimation_results", "category": "motor", "label": "OD 反推结果"},
    {"suffix": "slow_road_way", "category": "slow", "label": "慢行路网"},
    {"suffix": "slow_other_od", "category": "slow", "label": "慢行 OD"},
    {"suffix": "slow_greedy_iteration_history", "category": "slow", "label": "慢行迭代历史"},
    {"suffix": "slow_greedy_link_flow_results", "category": "slow", "label": "慢行路段流量"},
    {"suffix": "slow_greedy_path_results", "category": "slow", "label": "慢行路径结果"},
    {"suffix": "slow_greedy_summary_statistics", "category": "slow", "label": "慢行统计摘要"},
    {"suffix": "other_taz_socioeconomic", "category": "trip", "label": "TAZ 社会经济"},
    {"suffix": "other_trip_production_attraction", "category": "trip", "label": "产生吸引量"},
    {"suffix": "other_trip_distribution_hist", "category": "trip", "label": "分布历史"},
    {"suffix": "other_bus_route", "category": "pt", "label": "历史公交线路属性"},
    {"suffix": "city_bus_stop_table", "category": "pt", "label": "历史城市公交站点"},
    {"suffix": "pt_station_table", "category": "pt", "label": "历史公交场站"},
    {"suffix": "rail_station_table", "category": "pt", "label": "历史轨道站点"},
    {"suffix": "pt_route", "category": "pt", "label": "公交线路"},
    {"suffix": "bus_point", "category": "pt", "label": "公交站点"},
    {"suffix": "pt_transit", "category": "pt", "label": "公交站间运行"},
    {"suffix": "pt_shape", "category": "pt", "label": "公交线路序列"},
    {"suffix": "pt_trip", "category": "pt", "label": "公交 OD 需求"},
    {"suffix": "pt_walk", "category": "pt", "label": "步行换乘"},
    {"suffix": "bus_way", "category": "pt", "label": "公交道路渲染表"},
    {"suffix": "pt_link_result", "category": "pt", "label": "公交链路结果"},
    {"suffix": "pt_path_result", "category": "pt", "label": "公交路径结果"},
    {"suffix": "pt_iter_result", "category": "pt", "label": "公交迭代结果"},
    {"suffix": "pt_summary_result", "category": "pt", "label": "公交汇总结果"},
    {"suffix": "pt_stop_link_vc_result", "category": "pt", "label": "公交站间饱和度结果"},
    {"suffix": "diagnosis_indicator_result_rows", "category": "diagnosis", "label": "诊断指标结果"},
    {"suffix": "road_community", "category": "network", "label": "路网社区"},
    {"suffix": "road_point", "category": "network", "label": "路网节点"},
]

SCOPE_CATEGORIES = {
    "motor": {"motor"},
    "slow": {"slow"},
    "pt": {"pt"},
    "trip": {"trip"},
    "all": {"motor", "slow", "pt", "trip", "diagnosis", "network"},
}


def suffixes_for_scope(scope: str) -> list[dict[str, str]]:
    cats = SCOPE_CATEGORIES.get(scope, SCOPE_CATEGORIES["all"])
    return [s for s in TABLE_SPECS if s["category"] in cats]
