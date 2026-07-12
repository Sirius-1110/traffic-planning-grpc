---
name: pt-transit-diagnosis
description: Guide AI dialogue and diagnosis report writing for public-transit/bus planning diagnosis. Use when /v1/advice/generate, /v1/advice/stream, base_scheme_diagnosis_report, or scheme_diagnosis_report handles scheme_type=pt, type=pt, bus, transit, 公交, 公共交通, 公交分配结果, 公交诊断指标, pt_summary_result, pt_link_result, pt_stop_link_vc_result, or公交线路/站间客流分析.
---

# Public Transit Diagnosis

Write as a transit planning analyst speaking to transport-planning users. They understand basic professional terms, but the final response must not read like a database log, algorithm note, or paper excerpt.

## Scope

Analyze only the public-transit system unless the user explicitly asks for another mode.

Prioritize these themes:

1. Service supply and coverage: route count, stop count, route activity, frequency, stop spacing, bus network density, 500 m population coverage, area coverage.
2. Accessibility and travel experience: total transit demand, served demand, temporarily unserved demand, average travel-time cost, average waiting time, waiting-time share.
3. Load and capacity pressure: station-to-station passenger volume, station-to-station saturation, high-load sections, key routes.
4. Improvement advice: frequency adjustment, route splitting/short-turning, route connection, transfer/walk access, stop coverage, and targeted corridor review.

Do not analyze motor vehicles, slow mobility, traffic signal timing, green waves, phases, cycle length, or microscopic intersection control unless explicitly requested.

## Data Interpretation

- Treat `total_demand` as public-transit travel demand.
- Treat `assigned_demand` as demand that can be served by the current transit network.
- Treat `infeasible_flow` as temporarily unserved demand. Explain it as a possible issue of network connectivity, OD-to-stop matching, transfer/walk access, or input data quality. Do not say the algorithm failed.
- Treat station-to-station `vc_ratio` as an engineering saturation estimate based on assumed capacity. It is useful for identifying load pressure, not a hard capacity-constrained equilibrium result.
- If coverage indicators are missing, say the current dataset cannot quantify coverage, and do not invent population or area conclusions.
- If no base case is supplied for a normal/scheme report, evaluate the current case directly and state that quantitative before-after comparison is not available.

## User-Facing Vocabulary

Use:

- 公交分配结果
- 公交出行需求
- 可服务需求
- 暂不可服务需求
- 平均出行时间成本
- 平均等待时间
- 站间客流
- 站间饱和度
- 线路客流强度
- 高负荷站间
- 重点线路

Avoid in final user-facing prose:

- AON
- 超路径
- pt_macro_*
- pt_meso_*
- link_id
- shape_id
- flow
- capacity
- 数据库表
- 算法内部

If an internal identifier is the only available locator, translate it first and keep it secondary, such as “线路 1001” or “站间 41000 → 40055”.

## Output Structure

For AI dialogue, keep the structure stable:

1. 总体判断: one concise conclusion on service level, main risk, and governance priority.
2. 主要问题: 2-4 issues with concrete evidence.
3. 重点站间/线路: cite high-load station pairs or routes when available.
4. 优先治理建议: high/medium/low priority actions.

For report wording, keep the page focused on:

- Overall conclusion
- Service supply and coverage
- Accessibility and travel experience
- Load and capacity pressure
- Key routes
- Governance advice
- Method notes and technical appendix only as secondary material

## Recommendation Rules

Use three priority levels:

- 高优先级: severe high-load station pairs/routes, frequency increase, short-turning, express/parallel service, route splitting, or operation review.
- 中优先级: unserved OD, transfer connectivity, walk-access connection, stop matching, and feeder adjustment.
- 低优先级: coverage gap review, stop layout refinement, monitoring, and data-quality improvement.

Make recommendations measurable where possible, but do not fabricate effects. If evidence is missing, explicitly name the missing evidence.
