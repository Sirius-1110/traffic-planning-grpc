#pragma once

#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/sync_stream.h>
#include "tna_service.pb.h"

namespace tna_local_bridge {

struct CentroidConnectorEnvConfig {
    std::string mode = "single";  // single | multi_osm
    int max_connectors_per_zone = 5;
    std::string table_prefix_mode;  // empty=tool; scheme_observation=方案前缀; tool_observation=工具前缀+other_observation 默认参数
};

CentroidConnectorEnvConfig parse_centroid_config_from_param2(
    const std::string& param2, const char* default_mode = "single");

/** scheme_observation（tool_estimation_case）：multi_osm，每区至少 2 条不同道路连杆 */
CentroidConnectorEnvConfig parse_scheme_observation_centroid_config(const std::string& param2);

/** tool_observation（tool_estimation 默认）：同 case 算法，工具前缀 project{p}_user{u}_ */
CentroidConnectorEnvConfig parse_tool_observation_centroid_config(const std::string& param2);

grpc::Status run_tool_estimation_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_tool_estimation_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const CentroidConnectorEnvConfig& centroid_cfg);
grpc::Status run_tool_estimation_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);
grpc::Status run_tool_estimation_stream(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer,
    const CentroidConnectorEnvConfig& centroid_cfg);

CentroidConnectorEnvConfig parse_od_estimate_local_centroid_config(const std::string& param2);

CentroidConnectorEnvConfig parse_tool_od_estimation_local_centroid_config(const std::string& param2);

grpc::Status run_od_estimate_local_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_od_estimate_local_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const CentroidConnectorEnvConfig& centroid_cfg);
grpc::Status run_od_estimate_local_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);
grpc::Status run_od_estimate_local_stream(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer,
    const CentroidConnectorEnvConfig& centroid_cfg);

grpc::Status run_tool_od_estimation_local_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_tool_od_estimation_local_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const CentroidConnectorEnvConfig& centroid_cfg);
grpc::Status run_tool_od_estimation_local_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);
grpc::Status run_tool_od_estimation_local_stream(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer,
    const CentroidConnectorEnvConfig& centroid_cfg);

grpc::Status run_trip_sync(const std::string& method, const func::ParamsData* request, func::ResultData* response);
grpc::Status run_trip_stream(const std::string& method, const std::string& start_title, const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_centroid_connector_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_centroid_connector_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

/** 无 type=10 时建连杆；已有则跳过（除非 force_rebuild）。供反推/分配入口串行调用。 */
grpc::Status ensure_centroid_connectors_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const std::string& table_prefix_mode,
    const std::string& connector_mode,
    int max_connectors_per_zone);

grpc::Status run_transit_assignment_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_transit_assignment_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);
grpc::Status run_transit_assignment_new_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_transit_assignment_new_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_diagnosis_sync(const std::string& module, const func::ParamsData* request, func::ResultData* response);
grpc::Status run_diagnosis_stream(const std::string& module, const std::string& start_title, const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

// 分阶段流式辅助：progress 映射到 [start_percent, end_percent]，不写最终 done 帧，结果写入 result_out。
// 供 review_* 方案评审方法串联调用。
grpc::Status run_transit_assignment_stream_ranged(const func::ParamsData* request,
                                                   grpc::ServerWriter<func::ProgressData>* writer,
                                                   int start_percent, int end_percent,
                                                   func::ResultData* result_out);

grpc::Status run_diagnosis_stream_ranged(const std::string& module,
                                          const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer,
                                          int start_percent, int end_percent,
                                          func::ResultData* result_out);

grpc::Status run_report_sync(int task, const func::ParamsData* request, func::ResultData* response);
grpc::Status run_report_stream(int task, const std::string& start_title, const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_base_report_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_base_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_cost_benefit_report_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_cost_benefit_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_scheme_compare_report_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_scheme_compare_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_base_scheme_diagnosis_report_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_base_scheme_diagnosis_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

grpc::Status run_scheme_diagnosis_report_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_scheme_diagnosis_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);


grpc::Status run_od_trace_sync(const func::ParamsData* request, func::ResultData* response);
grpc::Status run_od_trace_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer);

}  // namespace tna_local_bridge
