#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include <cstdlib>
#include <unistd.h>
#include <limits.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "tna_service.grpc.pb.h"
#include "local_bridge.h"

#ifdef _WIN32
#include "../../../Include/postgresql/libpq-fe.h"
#else
#include <libpq-fe.h>
#endif

// TNA library umbrella header (includes TNM_Net.h, TNM_Algorithm.h, etc.)
#if __has_include("stdafx.h")
#  include "stdafx.h"
#else
#  include "header/stdafx.h"
#endif

#include <sstream>

// Accumulated warnings from TNM
static std::string g_warnings;

struct CentroidConnectorEnvConfig {
    std::string mode = "single";  // single | multi_osm
    int max_connectors_per_zone = 5;
};

static int extract_json_int_field(const std::string& s, const std::string& key, int def)
{
    const std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return def;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    size_t q = p;
    while (q < s.size() && (s[q] == '-' || (s[q] >= '0' && s[q] <= '9'))) ++q;
    if (q == p) return def;
    return std::atoi(s.substr(p, q - p).c_str());
}

static double extract_json_double_field(const std::string& s, const std::string& key, double def)
{
    const std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return def;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    char* end = nullptr;
    double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return def;
    return v;
}

struct MotorAssignmentParams {
    double conv = 0.001;
    int max_iter = 20;
    bool preserve_centroid_links = true;
    bool skip_centroid_prebuild = true;
};

static bool json_field_is_true(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":true";
    return json.find(needle) != std::string::npos;
}

static bool json_field_is_false(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":false";
    return json.find(needle) != std::string::npos;
}

static bool auto_centroid_prebuild_enabled(const std::string& param2)
{
    if (json_field_is_false(param2, "auto_centroid_prebuild")) return false;
    if (json_field_is_true(param2, "auto_centroid_prebuild")) return true;
    return true;
}

static MotorAssignmentParams parse_motor_assignment_params(const std::string& param2)
{
    MotorAssignmentParams p;
    if (param2.empty()) return p;
    std::string json = param2;
    const size_t ap = json.find("\"algorithm_params\"");
    if (ap != std::string::npos) json = json.substr(ap);
    double conv = extract_json_double_field(json, "conv", p.conv);
    if (conv > 0.0) p.conv = conv;
    int max_iter = extract_json_int_field(json, "max_iter", p.max_iter);
    if (max_iter > 0) p.max_iter = max_iter;
    if (json_field_is_true(param2, "preserve_centroid_links")
        || param2.find("preserve_od_estimation_centroid") != std::string::npos) {
        p.preserve_centroid_links = true;
    }
    if (json_field_is_false(param2, "preserve_centroid_links")) {
        p.preserve_centroid_links = false;
    }
    if (json_field_is_true(param2, "skip_centroid_prebuild")) {
        p.skip_centroid_prebuild = true;
    }
    if (json_field_is_false(param2, "skip_centroid_prebuild")) {
        p.skip_centroid_prebuild = false;
    }
    return p;
}

static void apply_skip_centroid_env(bool skip)
{
#ifdef _WIN32
    _putenv_s("TNA_SKIP_CENTROID_PREBUILD", skip ? "1" : "0");
#else
    setenv("TNA_SKIP_CENTROID_PREBUILD", skip ? "1" : "0", 1);
#endif
}

static void apply_preserve_centroid_env(bool preserve)
{
#ifdef _WIN32
    _putenv_s("TNA_PRESERVE_CENTROID_LINKS", preserve ? "1" : "0");
#else
    setenv("TNA_PRESERVE_CENTROID_LINKS", preserve ? "1" : "0", 1);
#endif
}

static CentroidConnectorEnvConfig parse_centroid_config_from_param2(
    const std::string& param2, const char* default_mode)
{
    CentroidConnectorEnvConfig cfg;
    cfg.mode = default_mode ? std::string(default_mode) : "single";
    cfg.max_connectors_per_zone = extract_json_int_field(param2, "max_connectors_per_zone", 5);
    if (cfg.max_connectors_per_zone < 1) cfg.max_connectors_per_zone = 1;
    if (cfg.max_connectors_per_zone > 20) cfg.max_connectors_per_zone = 20;
    return cfg;
}

static void apply_centroid_connector_env(const CentroidConnectorEnvConfig& cfg)
{
#ifdef _WIN32
    _putenv_s("TNA_CENTROID_CONNECTOR_MODE", cfg.mode.c_str());
    _putenv_s("TNA_CENTROID_MAX_CONNECTORS", std::to_string(cfg.max_connectors_per_zone).c_str());
#else
    setenv("TNA_CENTROID_CONNECTOR_MODE", cfg.mode.c_str(), 1);
    setenv("TNA_CENTROID_MAX_CONNECTORS", std::to_string(cfg.max_connectors_per_zone).c_str(), 1);
#endif
}

static void reset_centroid_connector_env()
{
#ifdef _WIN32
    _putenv_s("TNA_CENTROID_CONNECTOR_MODE", "single");
    _putenv_s("TNA_CENTROID_MAX_CONNECTORS", "5");
#else
    setenv("TNA_CENTROID_CONNECTOR_MODE", "single", 1);
    setenv("TNA_CENTROID_MAX_CONNECTORS", "5", 1);
#endif
}

static std::string extract_error_message_from_detail(const std::string& detail)
{
    if (detail.empty()) return detail;
    const std::string key = "\"message\":\"";
    size_t pos = detail.find(key);
    if (pos == std::string::npos) return detail;
    pos += key.size();
    std::string out;
    for (size_t i = pos; i < detail.size(); ++i) {
        char c = detail[i];
        if (c == '"' && (i == pos || detail[i - 1] != '\\')) break;
        if (c == '\\' && i + 1 < detail.size()) {
            char n = detail[i + 1];
            if (n == 'n') { out.push_back('\n'); ++i; continue; }
            if (n == 't') { out.push_back('\t'); ++i; continue; }
            if (n == '"') { out.push_back('"'); ++i; continue; }
            if (n == '\\') { out.push_back('\\'); ++i; continue; }
        }
        out.push_back(c);
    }
    return out.empty() ? detail : out;
}

static std::string resolve_od_mode_attribute()
{
    const char* mode = std::getenv("TNA_OD_MODE");
    if (mode == nullptr || *mode == '\0') return "unknown";
    return std::string(mode);
}

static bool motor_assignment_failed(TAP_Greedy_dijk& solver, std::string& message_out)
{
    TERMFLAGS tf = solver.Solve();
    if (tf != ErrorTerm) {
        return false;
    }
    message_out.clear();
    if (solver.network) {
        message_out = extract_error_message_from_detail(solver.network->GetLastErrorDetail());
    }
    if (message_out.empty()) {
        message_out = "交通分配求解失败：存在不可达 OD 对";
    }
    return true;
}

// 全局数据库连接串（启动时从配置文件加载）
static std::string g_db_conn_str;
thread_local grpc::ServerWriter<func::ProgressData>* g_base_network_progress_writer = nullptr;
thread_local int g_base_network_last_percent = -1;
thread_local int g_base_network_last_iter = -1;
thread_local int g_base_network_phase_start = 0;
thread_local int g_base_network_phase_end   = 100;

static int map_solver_percent_to_stream_percent(int solver_percent)
{
    if (solver_percent < 0) solver_percent = 0;
    if (solver_percent > 98) solver_percent = 98;
    return 10 + (solver_percent * 85) / 98;
}

static int remap_phase_percent(int raw_pct)
{
    int s = g_base_network_phase_start;
    int e = g_base_network_phase_end;
    if (raw_pct < 0) raw_pct = 0;
    if (raw_pct > 100) raw_pct = 100;
    return s + raw_pct * (e - s) / 100;
}

static void reset_base_network_stream_progress_bridge()
{
    g_base_network_progress_writer = nullptr;
    g_base_network_last_percent = -1;
    g_base_network_last_iter = -1;
    // phase_start/end intentionally NOT reset here: they remain valid for
    // remap_phase_percent() calls after Solve() in run_base_network_stream_impl.
}

static void bind_base_network_stream_progress_bridge(grpc::ServerWriter<func::ProgressData>* writer,
                                                      int phase_start = 0, int phase_end = 100)
{
    g_base_network_progress_writer = writer;
    g_base_network_last_percent = -1;
    g_base_network_last_iter = -1;
    g_base_network_phase_start = phase_start;
    g_base_network_phase_end   = phase_end;
}

static void emit_base_network_stream_progress(int iter, double precision, int solver_percent)
{
    if (g_base_network_progress_writer == nullptr) {
        return;
    }

    int percent = map_solver_percent_to_stream_percent(solver_percent);
    if (percent < 11) percent = 11;
    if (percent > 95) percent = 95;
    if (percent < g_base_network_last_percent) percent = g_base_network_last_percent;
    if (percent == g_base_network_last_percent && iter == g_base_network_last_iter) {
        return;
    }

    func::ProgressData frame;
    frame.set_code(0);
    frame.set_message("Running");
    frame.mutable_data()->set_percent(remap_phase_percent(percent));
    frame.mutable_data()->set_done(0);

    std::ostringstream title;
    title << "正在迭代求解";
    if (iter > 0) {
        title << "，第 " << iter << " 次";
    }
    title << "，当前收敛指标 " << precision;
    frame.mutable_data()->set_title(title.str());

    g_base_network_progress_writer->Write(frame);
    g_base_network_last_percent = percent;  // store raw (pre-remap) for dedup
    g_base_network_last_iter = iter;
}

// 读取数据库配置文件，返回 libpq 连接串
// 部署结构：算法根目录/db.conf；算法根目录/base_motor_network/tna_grpc_server
// 查找顺序：TNA_DB_CONF 环境变量 → 可执行文件上级目录(算法根目录)/db.conf → 可执行文件同目录/db.conf
static std::string load_db_config()
{
    // 候选路径列表
    std::vector<std::string> candidates;

    const char* env_conf = std::getenv("TNA_DB_CONF");
    if (env_conf && *env_conf)
        candidates.push_back(env_conf);

    // 可执行文件所在目录
    char exe_path[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        std::string exe_dir(exe_path, len);
        auto slash = exe_dir.rfind('/');
        if (slash != std::string::npos)
            exe_dir = exe_dir.substr(0, slash);

        // 上级目录（算法根目录）放 db.conf
        auto parent_slash = exe_dir.rfind('/');
        if (parent_slash != std::string::npos)
            candidates.push_back(exe_dir.substr(0, parent_slash) + "/db.conf");

        // 同目录兜底（开发/调试时直接放可执行文件旁边）
        candidates.push_back(exe_dir + "/db.conf");
    }

    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::string conn, line;
        while (std::getline(f, line)) {
            // 跳过空行和注释
            if (line.empty() || line[0] == '#') continue;
            if (!conn.empty()) conn += ' ';
            conn += line;
        }
        if (!conn.empty()) {
            std::cout << "[gRPC] Loaded db config from: " << path << std::endl;
            return conn;
        }
    }

    std::cerr << "[gRPC][WARN] No db.conf found. Tried:";
    for (const auto& p : candidates) std::cerr << ' ' << p;
    std::cerr << std::endl;
    return "";
}

static std::string resolve_request_db_conn_str(const std::string& param1)
{
    if (!param1.empty()) {
        std::string conn = param1;
        for (char& ch : conn) {
            if (ch == ';' || ch == '\n' || ch == '\r') ch = ' ';
        }
        return conn;
    }
    return g_db_conn_str;
}

// 根据 project_id + user_id + case_id 推导输入/输出表名。
// 默认 schema/role 为 user_project；如需覆盖，可设置环境变量 TNA_DB_SCHEMA。
// case_id <= 0 视为未提供，此时前缀退化为 {project_id}_{user_id}_。
static std::string resolve_db_schema()
{
    const char* schema_env = std::getenv("TNA_DB_SCHEMA");
    return (schema_env && *schema_env) ? std::string(schema_env) : std::string("user_project");
}

struct SchemaTableName {
    std::string schema;
    std::string table;
};

static SchemaTableName split_schema_table_name(const std::string& qualified_name)
{
    SchemaTableName result;
    size_t dot = qualified_name.find('.');
    if (dot == std::string::npos) {
        result.schema = resolve_db_schema();
        result.table = qualified_name;
    } else {
        result.schema = qualified_name.substr(0, dot);
        result.table = qualified_name.substr(dot + 1);
    }
    return result;
}

static bool postgres_table_exists(PGconn* conn, const std::string& qualified_name)
{
    if (conn == nullptr || qualified_name.empty()) {
        return false;
    }

    SchemaTableName st = split_schema_table_name(qualified_name);
    const char* values[2] = { st.schema.c_str(), st.table.c_str() };
    int lengths[2] = {0, 0};
    int formats[2] = {0, 0};
    PGresult* res = PQexecParams(
        conn,
        "SELECT 1 FROM information_schema.tables WHERE table_schema = $1 AND table_name = $2 LIMIT 1",
        2,
        nullptr,
        values,
        lengths,
        formats,
        0);

    bool exists = (res != nullptr && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (res != nullptr) {
        PQclear(res);
    }
    return exists;
}

static bool validate_input_table_pair(const std::string& db_conn_str,
                                      const std::string& network_table,
                                      const std::string& od_table,
                                      std::string& error_message)
{
    error_message.clear();
    PGconn* conn = PQconnectdb(db_conn_str.c_str());
    if (conn == nullptr) {
        error_message = "无法连接数据库，输入表校验失败。";
        return false;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        error_message = std::string("数据库连接失败：") + PQerrorMessage(conn);
        PQfinish(conn);
        return false;
    }

    bool network_exists = postgres_table_exists(conn, network_table);
    bool od_exists = postgres_table_exists(conn, od_table);
    PQfinish(conn);

    if (network_exists && od_exists) {
        return true;
    }

    error_message = "缺少必需的输入表。路网表=" + network_table
        + (network_exists ? "（存在）" : "（不存在）")
        + "，OD表=" + od_table
        + (od_exists ? "（存在）" : "（不存在）");
    return false;
}

static std::string build_table_prefix(int32_t project_id, int32_t user_id, int32_t case_id)
{
    // case_id 为方案 ID；0 也拼入前缀 → project{p}_user{u}_case0_
    return "project" + std::to_string(project_id) + "_user" + std::to_string(user_id)
           + "_case" + std::to_string(case_id) + "_";
}

// 根据 project_id + user_id + case_id 与场景 mode_suffix 派生输入/输出表名。
//   mode_suffix == ""      → 机动车（base_motor_network），前缀 = "{P}_{U}_{C}_"
//   mode_suffix == "slow_" → 慢行（base_slow_network），前缀 = "{P}_{U}_{C}_slow_"
// 算法 C++ 主体不感知 mode_suffix，只接收最终拼好的表名与 PG_SCENARIO_PREFIX。
static void derive_table_names(
    int32_t project_id, int32_t user_id, int32_t case_id,
    const std::string& mode_suffix,
    std::string& networkTable, std::string& odTable, std::string& scenarioPrefix)
{
    scenarioPrefix = build_table_prefix(project_id, user_id, case_id) + mode_suffix;

    std::string schema = resolve_db_schema();
    networkTable = schema + "." + scenarioPrefix + "road_way";
    odTable      = schema + "." + scenarioPrefix + "other_od";
}

static void append_multiline_entries(const std::string& text, google::protobuf::RepeatedPtrField<std::string>* out)
{
    std::string current;
    for (char ch : text) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!current.empty()) *out->Add() = current;
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) *out->Add() = current;
}

static void fill_base_network_result_payload(int iter,
                                             const std::string& stage_label,
                                             const std::string& networkTable,
                                             const std::string& odTable,
                                             const std::string& warnings,
                                             func::ResultData* response)
{
    response->clear_data();
    response->clear_summary();
    (*response->mutable_data()->mutable_tables()->mutable_items())["network_table"] = networkTable;
    (*response->mutable_data()->mutable_tables()->mutable_items())["od_table"] = odTable;
    (*response->mutable_data()->mutable_counts()->mutable_items())["iter"] = iter;
    response->mutable_summary()->set_stage(stage_label);
    (*response->mutable_summary()->mutable_attributes())["od_mode"] = resolve_od_mode_attribute();
    append_multiline_entries(warnings, response->mutable_summary()->mutable_logs());
}

// 同步求解公共逻辑：被 base_motor_network / base_slow_network 共用，
// 唯一区别是入参 mode_suffix（""=机动，"slow_"=慢行）与 stage_label。
static grpc::Status run_base_network_sync(const std::string& mode_suffix,
                                          const std::string& stage_label,
                                          const func::ParamsData* request,
                                          func::ResultData* response,
                                          const CentroidConnectorEnvConfig* centroid_cfg = nullptr)
{
    g_warnings.clear();

    std::string dbConnStr = resolve_request_db_conn_str(request->param1());
    if (dbConnStr.empty()) {
        response->set_code(-1);
        response->set_message("服务器未加载数据库配置，请检查 db.conf。");
        response->clear_data();
        response->clear_summary();
        return grpc::Status::OK;
    }
    if (request->project_id() <= 0 || request->user_id() <= 0) {
        response->set_code(-1);
        response->set_message("project_id 与 user_id 必须为正整数。");
        response->clear_data();
        response->clear_summary();
        return grpc::Status::OK;
    }

    std::string networkTable, odTable, scenarioPrefix;
    derive_table_names(request->project_id(), request->user_id(), request->case_id(),
                       mode_suffix,
                       networkTable, odTable, scenarioPrefix);

    std::string missingInputMessage;
    if (!validate_input_table_pair(dbConnStr, networkTable, odTable, missingInputMessage)) {
        response->set_code(-2);
        response->set_message(missingInputMessage);
        response->clear_data();
        response->clear_summary();
        return grpc::Status::OK;
    }

    std::cout << "[gRPC] " << stage_label
              << ": project=" << request->project_id()
              << " user=" << request->user_id()
              << " case=" << request->case_id()
              << " schema=" << resolve_db_schema()
              << " net=" << networkTable << " od=" << odTable << std::endl;

    const bool is_slow_network = (mode_suffix == "slow_");
    if ((mode_suffix.empty() || is_slow_network) && auto_centroid_prebuild_enabled(request->param2())) {
        CentroidConnectorEnvConfig pre_cfg =
            centroid_cfg ? *centroid_cfg
                         : parse_centroid_config_from_param2(request->param2(), "multi_osm");
        func::ResultData pre;
        const char* prebuild_table_prefix = is_slow_network ? "scheme_slow" : "scheme";
        grpc::Status pre_st = tna_local_bridge::ensure_centroid_connectors_sync(
            request, &pre, prebuild_table_prefix, pre_cfg.mode, pre_cfg.max_connectors_per_zone);
        if (!pre_st.ok()) return pre_st;
        if (pre.code() != 1) {
            response->CopyFrom(pre);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] " << stage_label << ": centroid prebuild ok ("
                  << prebuild_table_prefix << ", mode=" << pre_cfg.mode << ")" << std::endl;
    }

    try {
        MotorAssignmentParams assign_params = parse_motor_assignment_params(request->param2());
        TAP_Greedy_dijk solver;
        solver.SetLPF(BPRLK);
        solver.SetConv(assign_params.conv);
        solver.SetMaxIter(assign_params.max_iter);
        std::cout << "[gRPC] assignment params: conv=" << assign_params.conv
                  << " max_iter=" << assign_params.max_iter
                  << " preserve_centroid=" << (assign_params.preserve_centroid_links ? "1" : "0")
                  << " skip_centroid_prebuild=" << (assign_params.skip_centroid_prebuild ? "1" : "0") << std::endl;
#ifdef _WIN32
        _putenv_s("PG_SCENARIO_PREFIX", scenarioPrefix.c_str());
        _putenv_s("TNA_OD_MODE", "");
#else
        setenv("PG_SCENARIO_PREFIX", scenarioPrefix.c_str(), 1);
        setenv("TNA_OD_MODE", "", 1);
#endif
        apply_preserve_centroid_env(assign_params.preserve_centroid_links);
        apply_skip_centroid_env(assign_params.skip_centroid_prebuild);
        if (centroid_cfg) {
            apply_centroid_connector_env(*centroid_cfg);
        } else {
            reset_centroid_connector_env();
        }
        int buildRet = solver.Build("", "", NETPOSTGRESQL, dbConnStr, networkTable, odTable);
        if (buildRet != 0) {
            std::string lastErrDetail;
            if (solver.network) {
                lastErrDetail = solver.network->GetLastErrorDetail();
            }
            std::string msg = extract_error_message_from_detail(lastErrDetail);
            if (msg.empty()) {
                msg = "构网失败，错误码 " + std::to_string(buildRet);
            }
            response->set_code(-2);
            response->set_message(msg);
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }

        std::string solve_msg;
        if (motor_assignment_failed(solver, solve_msg)) {
            response->set_code(-2);
            response->set_message(solve_msg);
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        solver.WritePG(dbConnStr, networkTable, scenarioPrefix);

        std::string warnings = g_warnings;
        response->set_code(1);
        response->set_message(warnings.empty() ? "成功" : ("成功\n" + warnings));
        fill_base_network_result_payload(solver.curIter, stage_label, networkTable, odTable, warnings, response);
        if (centroid_cfg) {
            (*response->mutable_summary()->mutable_attributes())["centroid_connector_mode"] = centroid_cfg->mode;
            (*response->mutable_summary()->mutable_attributes())["max_connectors_per_zone"] =
                std::to_string(centroid_cfg->max_connectors_per_zone);
        }

    } catch (const std::exception& e) {
        response->set_code(-99);
        response->set_message(std::string("Exception: ") + e.what());
        response->clear_data();
        response->clear_summary();
    } catch (...) {
        response->set_code(-99);
        response->set_message("Unknown exception");
        response->clear_data();
        response->clear_summary();
    }

    return grpc::Status::OK;
}

// 流式求解核心实现。
// phase_start/phase_end：将全程进度 0-100 映射到此子区间。
// emit_done=true：成功时写 done=1 完成帧（独立调用场景）；
// emit_done=false：成功时写 percent=phase_end 的进度帧（评审串联场景），结果写入 result_out。
// 错误时无论 emit_done 取何值均写 done=1 错误帧，函数返回 false。
static bool run_base_network_stream_impl(const std::string& mode_suffix,
                                         const std::string& stage_label,
                                         const func::ParamsData* request,
                                         grpc::ServerWriter<func::ProgressData>* writer,
                                         int phase_start, int phase_end,
                                         bool emit_done,
                                         func::ResultData* result_out,
                                         const CentroidConnectorEnvConfig* centroid_cfg = nullptr)
{
    g_warnings.clear();
    // Set phase range immediately so all remap_phase_percent() calls in this
    // function (including frames before bind_base_network_stream_progress_bridge)
    // use the correct sub-range.
    g_base_network_phase_start = phase_start;
    g_base_network_phase_end   = phase_end;

    std::string dbConnStr = resolve_request_db_conn_str(request->param1());
    if (dbConnStr.empty()) {
        func::ProgressData frame;
        frame.set_code(-1);
        frame.set_message("Server db config not loaded. Check db.conf.");
        frame.mutable_data()->set_done(1);
        writer->Write(frame);
        return false;
    }
    if (request->project_id() <= 0 || request->user_id() <= 0) {
        func::ProgressData frame;
        frame.set_code(-1);
        frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
        frame.mutable_data()->set_done(1);
        writer->Write(frame);
        return false;
    }

    std::string networkTable, odTable, scenarioPrefix;
    derive_table_names(request->project_id(), request->user_id(), request->case_id(),
                       mode_suffix,
                       networkTable, odTable, scenarioPrefix);

    std::string missingInputMessage;
    if (!validate_input_table_pair(dbConnStr, networkTable, odTable, missingInputMessage)) {
        func::ProgressData frame;
        frame.set_code(-2);
        frame.set_message(missingInputMessage);
        frame.mutable_data()->set_done(1);
        writer->Write(frame);
        return false;
    }

    std::cout << "[gRPC] " << stage_label << "_stream"
              << ": project=" << request->project_id()
              << " user=" << request->user_id()
              << " case=" << request->case_id()
              << " net=" << networkTable << " od=" << odTable << std::endl;

    const bool is_slow_network = (mode_suffix == "slow_");
    if ((mode_suffix.empty() || is_slow_network) && auto_centroid_prebuild_enabled(request->param2())) {
        CentroidConnectorEnvConfig pre_cfg =
            centroid_cfg ? *centroid_cfg
                         : parse_centroid_config_from_param2(request->param2(), "multi_osm");
        func::ResultData pre;
        const char* prebuild_table_prefix = is_slow_network ? "scheme_slow" : "scheme";
        grpc::Status pre_st = tna_local_bridge::ensure_centroid_connectors_sync(
            request, &pre, prebuild_table_prefix, pre_cfg.mode, pre_cfg.max_connectors_per_zone);
        if (!pre_st.ok()) {
            func::ProgressData frame;
            frame.set_code(-99);
            frame.set_message(pre_st.error_message());
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return false;
        }
        if (pre.code() != 1) {
            func::ProgressData frame;
            frame.set_code(pre.code());
            frame.set_message(pre.message());
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return false;
        }
    }

    {
        func::ProgressData frame;
        frame.set_code(0);
        frame.set_message("Starting");
        frame.mutable_data()->set_percent(remap_phase_percent(0));
        frame.mutable_data()->set_done(0);
        frame.mutable_data()->set_title("正在加载路网数据...");
        writer->Write(frame);
    }

    try {
        MotorAssignmentParams assign_params = parse_motor_assignment_params(request->param2());
        TAP_Greedy_dijk solver;
        solver.SetLPF(BPRLK);
        solver.SetConv(assign_params.conv);
        solver.SetMaxIter(assign_params.max_iter);
        std::cout << "[gRPC] assignment params: conv=" << assign_params.conv
                  << " max_iter=" << assign_params.max_iter
                  << " preserve_centroid=" << (assign_params.preserve_centroid_links ? "1" : "0")
                  << " skip_centroid_prebuild=" << (assign_params.skip_centroid_prebuild ? "1" : "0") << std::endl;
#ifdef _WIN32
        _putenv_s("PG_SCENARIO_PREFIX", scenarioPrefix.c_str());
        _putenv_s("TNA_OD_MODE", "");
#else
        setenv("PG_SCENARIO_PREFIX", scenarioPrefix.c_str(), 1);
        setenv("TNA_OD_MODE", "", 1);
#endif
        apply_preserve_centroid_env(assign_params.preserve_centroid_links);
        apply_skip_centroid_env(assign_params.skip_centroid_prebuild);
        if (centroid_cfg) {
            apply_centroid_connector_env(*centroid_cfg);
        } else {
            reset_centroid_connector_env();
        }
        int buildRet = solver.Build("", "", NETPOSTGRESQL, dbConnStr, networkTable, odTable);
        if (buildRet != 0) {
            func::ProgressData frame;
            std::string lastErrDetail;
            if (solver.network) {
                lastErrDetail = solver.network->GetLastErrorDetail();
            }
            std::string msg = extract_error_message_from_detail(lastErrDetail);
            if (msg.empty()) {
                msg = "构网失败，错误码 " + std::to_string(buildRet);
            }
            frame.set_code(-2);
            frame.set_message(msg);
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return false;
        }

        {
            func::ProgressData frame;
            frame.set_code(0);
            frame.set_message("Running");
            frame.mutable_data()->set_percent(remap_phase_percent(10));
            frame.mutable_data()->set_done(0);
            frame.mutable_data()->set_title("路网加载完成，正在迭代求解...");
            writer->Write(frame);
        }

        bind_base_network_stream_progress_bridge(writer, phase_start, phase_end);
        solver.SetProgressCallback(emit_base_network_stream_progress);
        std::string solve_msg;
        const bool solve_failed = motor_assignment_failed(solver, solve_msg);
        solver.SetProgressCallback(nullptr);
        reset_base_network_stream_progress_bridge();

        if (solve_failed) {
            func::ProgressData frame;
            frame.set_code(-2);
            frame.set_message(solve_msg);
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return false;
        }

        {
            func::ProgressData frame;
            frame.set_code(0);
            frame.set_message("Writing");
            frame.mutable_data()->set_percent(remap_phase_percent(98));
            frame.mutable_data()->set_done(0);
            frame.mutable_data()->set_title("迭代求解完成，正在写入结果...");
            writer->Write(frame);
        }

        solver.WritePG(dbConnStr, networkTable, scenarioPrefix);

        std::string warnings = g_warnings;
        func::ResultData local_result;
        func::ResultData& out = result_out ? *result_out : local_result;
        out.set_code(1);
        out.set_message(warnings.empty() ? "成功" : ("成功\n" + warnings));
        fill_base_network_result_payload(solver.curIter, stage_label, networkTable, odTable, warnings, &out);
        if (centroid_cfg) {
            (*out.mutable_summary()->mutable_attributes())["centroid_connector_mode"] = centroid_cfg->mode;
            (*out.mutable_summary()->mutable_attributes())["max_connectors_per_zone"] =
                std::to_string(centroid_cfg->max_connectors_per_zone);
        }

        if (emit_done) {
            func::ProgressData frame;
            frame.set_code(1);
            frame.set_message(out.message());
            frame.mutable_data()->set_percent(remap_phase_percent(100));
            frame.mutable_data()->set_done(1);
            frame.mutable_data()->set_title("交通分配完成，共迭代 " + std::to_string(solver.curIter) + " 次");
            frame.mutable_result()->CopyFrom(out);
            writer->Write(frame);
        } else {
            func::ProgressData frame;
            frame.set_code(0);
            frame.set_message("Phase1 done");
            frame.mutable_data()->set_percent(remap_phase_percent(100));
            frame.mutable_data()->set_done(0);
            frame.mutable_data()->set_title("交通分配完成，共迭代 " + std::to_string(solver.curIter) + " 次");
            writer->Write(frame);
        }
        return true;

    } catch (const std::exception& e) {
        reset_base_network_stream_progress_bridge();
        func::ProgressData frame;
        frame.set_code(-99);
        frame.set_message(std::string("Exception: ") + e.what());
        frame.mutable_data()->set_done(1);
        writer->Write(frame);
        return false;
    } catch (...) {
        reset_base_network_stream_progress_bridge();
        func::ProgressData frame;
        frame.set_code(-99);
        frame.set_message("Unknown exception");
        frame.mutable_data()->set_done(1);
        writer->Write(frame);
        return false;
    }
}

// 独立调用的流式接口（原有行为不变）
static grpc::Status run_base_network_stream(const std::string& mode_suffix,
                                            const std::string& stage_label,
                                            const func::ParamsData* request,
                                            grpc::ServerWriter<func::ProgressData>* writer,
                                            const CentroidConnectorEnvConfig* centroid_cfg = nullptr)
{
    run_base_network_stream_impl(mode_suffix, stage_label, request, writer, 0, 100, true, nullptr, centroid_cfg);
    return grpc::Status::OK;
}

static std::string quote_pg_identifier(const std::string& value)
{
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out += "\"";
    return out;
}

static std::string qualified_pg_table(const std::string& schema, const std::string& table)
{
    return quote_pg_identifier(schema) + "." + quote_pg_identifier(table);
}

static std::string quote_pg_literal(const std::string& value)
{
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

static grpc::Status run_slow_capacity_adjustment_sync(
    const func::ParamsData* request, func::ResultData* response)
{
    response->clear_data();
    response->clear_summary();
    response->mutable_summary()->set_stage("slow_capacity_adjustment");

    if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
        response->set_code(-1);
        response->set_message(
            "project_id/user_id must be > 0 and case_id must be >= 0.");
        return grpc::Status::OK;
    }

    const std::string db_conn_str = resolve_request_db_conn_str(request->param1());
    if (db_conn_str.empty()) {
        response->set_code(-1);
        response->set_message("Server database configuration is unavailable.");
        return grpc::Status::OK;
    }

    const std::string schema = resolve_db_schema();
    const std::string prefix =
        build_table_prefix(request->project_id(), request->user_id(), request->case_id())
        + "slow_";
    const std::string road_name = prefix + "road_way";
    const std::string impact_name = prefix + "impact";
    const std::string road_table = schema + "." + road_name;
    const std::string impact_table = schema + "." + impact_name;

    PGconn* conn = PQconnectdb(db_conn_str.c_str());
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
        response->set_code(-2);
        response->set_message(
            std::string("Database connection failed: ")
            + (conn ? PQerrorMessage(conn) : "null connection"));
        if (conn) PQfinish(conn);
        return grpc::Status::OK;
    }

    if (!postgres_table_exists(conn, road_table)
        || !postgres_table_exists(conn, impact_table)) {
        response->set_code(-2);
        response->set_message(
            "Required input table is missing. road_table=" + road_table
            + ", impact_table=" + impact_table);
        PQfinish(conn);
        return grpc::Status::OK;
    }

    const std::string qr = qualified_pg_table(schema, road_name);
    const std::string qi = qualified_pg_table(schema, impact_name);
    std::ostringstream sql;
    sql
        << "BEGIN;"
        << "ALTER TABLE " << qr
        << " ADD COLUMN IF NOT EXISTS capacity_slow_adj double precision;"
        << "WITH joined AS ("
        << " SELECT r.link_id,r.capacity,r.speedlimit,r.type,r.lane_num,"
        << " COALESCE(i.lane_width,3.5)::double precision AS sc_lane_width,"
        << " COALESCE(i.separation_type,1)::integer AS sc_separation_type,"
        << " COALESCE(i.bike_lane_width,0.0)::double precision AS sc_bike_lane_width,"
        << " COALESCE(i.buffer_width,0.0)::double precision AS sc_buffer_width,"
        << " COALESCE(i.req_bike_width,1.5)::double precision AS sc_req_bike_width,"
        << " COALESCE(i.lat_safety_clear,0.5)::double precision AS sc_lat_safety_clear,"
        << " COALESCE(i.enc_ratio,0.0)::double precision AS sc_enc_ratio,"
        << " GREATEST(COALESCE(i.aff_lane_num,1),0)::integer AS sc_aff_lane_num"
        << " FROM " << qr << " r"
        << " LEFT JOIN (SELECT DISTINCT ON (link_id) * FROM " << qi
        << " ORDER BY link_id,id) i USING (link_id)"
        << "), lane_base AS ("
        << " SELECT joined.*,"
        << " CASE WHEN COALESCE(lane_num,0)>0 THEN lane_num::integer"
        << " ELSE GREATEST(1,ROUND(COALESCE(capacity,0)/"
        << "   CASE WHEN COALESCE(type,4) IN (1,2) OR COALESCE(speedlimit,30)>=80"
        << "        THEN CASE WHEN COALESCE(speedlimit,30)>=80 THEN 2100 ELSE 1800 END"
        << "        WHEN COALESCE(speedlimit,30)>=60 THEN 1800"
        << "        WHEN COALESCE(speedlimit,30)>=50 THEN 1700"
        << "        WHEN COALESCE(speedlimit,30)>=40 THEN 1650"
        << "        WHEN COALESCE(speedlimit,30)>=30 THEN 1600 ELSE 1400 END"
        << " )::integer) END AS sc_effective_lane_num"
        << " FROM joined"
        << "), width_base AS ("
        << " SELECT lane_base.*,"
        << " CASE WHEN sc_separation_type=2 THEN 0.0 ELSE"
        << " sc_enc_ratio*GREATEST(0.0,sc_req_bike_width+sc_lat_safety_clear"
        << " -sc_bike_lane_width-sc_buffer_width) END AS sc_encroached_width"
        << " FROM lane_base"
        << "), factor_base AS ("
        << " SELECT width_base.*,"
        << " GREATEST(2.75,sc_lane_width-sc_encroached_width) AS sc_effective_width,"
        << " CASE WHEN sc_separation_type=2 THEN 1.0 ELSE"
        << " LEAST(1.05,GREATEST(0.90,"
        << " 1.0+(GREATEST(2.75,sc_lane_width-sc_encroached_width)-3.6)/9.0))"
        << " END AS sc_width_factor"
        << " FROM width_base"
        << "), calculated AS ("
        << " SELECT factor_base.*,"
        << " ROUND((CASE WHEN COALESCE(capacity,0)>=999999 THEN capacity"
        << " WHEN COALESCE(capacity,0)<=0 THEN 0.0 ELSE"
        << " capacity*(1.0-(LEAST(sc_aff_lane_num,sc_effective_lane_num)::double precision"
        << " /NULLIF(sc_effective_lane_num,0))*(1.0-sc_width_factor)) END)::numeric,2)"
        << " ::double precision AS capacity_slow_adj"
        << " FROM factor_base"
        << ") UPDATE " << qr << " AS road"
        << " SET capacity_slow_adj=calculated.capacity_slow_adj"
        << " FROM calculated WHERE road.link_id=calculated.link_id;"
        << "COMMIT;";

    PGresult* exec_res = PQexec(conn, sql.str().c_str());
    if (exec_res == nullptr || PQresultStatus(exec_res) != PGRES_COMMAND_OK) {
        const std::string error = PQerrorMessage(conn);
        if (exec_res) PQclear(exec_res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        response->set_code(-2);
        response->set_message("Slow capacity calculation failed: " + error);
        return grpc::Status::OK;
    }
    PQclear(exec_res);

    const std::string metrics_sql =
        "SELECT COUNT(*),"
        " COUNT(*) FILTER (WHERE"
        " ROUND((COALESCE(capacity,0)-COALESCE(capacity_slow_adj,0))::numeric,2)>0),"
        " COALESCE(AVG(ROUND((COALESCE(capacity,0)-COALESCE(capacity_slow_adj,0))"
        " ::numeric,2)),0),"
        " COALESCE(MAX(ROUND((COALESCE(capacity,0)-COALESCE(capacity_slow_adj,0))"
        " ::numeric,2)),0),"
        " COALESCE(AVG(CASE WHEN COALESCE(capacity,0)>0 THEN"
        " (capacity-capacity_slow_adj)/capacity ELSE 0.0 END),0)"
        " FROM " + qr;
    PGresult* metrics = PQexec(conn, metrics_sql.c_str());
    if (metrics == nullptr || PQresultStatus(metrics) != PGRES_TUPLES_OK
        || PQntuples(metrics) != 1) {
        const std::string error = PQerrorMessage(conn);
        if (metrics) PQclear(metrics);
        PQfinish(conn);
        response->set_code(-2);
        response->set_message("Result metrics query failed: " + error);
        return grpc::Status::OK;
    }

    const int64_t rows = std::strtoll(PQgetvalue(metrics, 0, 0), nullptr, 10);
    const int64_t affected = std::strtoll(PQgetvalue(metrics, 0, 1), nullptr, 10);
    const double avg_reduction = std::strtod(PQgetvalue(metrics, 0, 2), nullptr);
    const double max_reduction = std::strtod(PQgetvalue(metrics, 0, 3), nullptr);
    const double avg_reduction_pct = std::strtod(PQgetvalue(metrics, 0, 4), nullptr);
    PQclear(metrics);
    PQfinish(conn);

    response->set_code(1);
    response->set_message(
        "Slow traffic capacity adjustment completed and backfilled to road_table.capacity_slow_adj.");
    (*response->mutable_data()->mutable_tables()->mutable_items())["road_table"] = road_table;
    (*response->mutable_data()->mutable_tables()->mutable_items())["impact_table"] = impact_table;
    (*response->mutable_data()->mutable_tables()->mutable_items())["output_table"] = road_table;
    (*response->mutable_data()->mutable_counts()->mutable_items())["rows"] = rows;
    (*response->mutable_data()->mutable_counts()->mutable_items())["affected_rows"] = affected;
    (*response->mutable_data()->mutable_metrics()->mutable_items())["average_reduction"] =
        avg_reduction;
    (*response->mutable_data()->mutable_metrics()->mutable_items())["maximum_reduction"] =
        max_reduction;
    (*response->mutable_data()->mutable_metrics()->mutable_items())["average_reduction_pct"] =
        avg_reduction_pct;
    (*response->mutable_summary()->mutable_attributes())["formula_version"] =
        "slow_capacity_v2_backfill";
    (*response->mutable_summary()->mutable_attributes())["output_column"] =
        "capacity_slow_adj";
    return grpc::Status::OK;
}

static grpc::Status run_slow_capacity_adjustment_stream_impl(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer)
{
    func::ProgressData start;
    start.set_code(0);
    start.set_message("Running");
    start.mutable_data()->set_percent(10);
    start.mutable_data()->set_done(0);
    start.mutable_data()->set_title("Calculating slow traffic capacity adjustment");
    writer->Write(start);

    func::ResultData result;
    run_slow_capacity_adjustment_sync(request, &result);
    func::ProgressData done;
    done.set_code(result.code());
    done.set_message(result.message());
    done.mutable_data()->set_percent(100);
    done.mutable_data()->set_done(1);
    done.mutable_data()->set_title(
        result.code() == 1 ? "Completed" : "Failed");
    done.mutable_result()->CopyFrom(result);
    writer->Write(done);
    return grpc::Status::OK;
}
static bool execute_slow_capacity_assignment_sql(
    const func::ParamsData* request,
    const std::string& sql,
    std::string& error)
{
    const std::string db_conn_str = resolve_request_db_conn_str(request->param1());
    PGconn* conn = PQconnectdb(db_conn_str.c_str());
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
        error = conn ? PQerrorMessage(conn) : "null database connection";
        if (conn) PQfinish(conn);
        return false;
    }
    PGresult* result = PQexec(conn, sql.c_str());
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok) error = PQerrorMessage(conn);
    if (result) PQclear(result);
    PQfinish(conn);
    return ok;
}

static std::string slow_assignment_road_table_sql(const func::ParamsData* request)
{
    const std::string prefix =
        build_table_prefix(request->project_id(), request->user_id(), request->case_id())
        + "slow_";
    return qualified_pg_table(resolve_db_schema(), prefix + "road_way");
}

static bool recover_slow_assignment_capacity(
    const func::ParamsData* request, std::string& error)
{
    const std::string road = slow_assignment_road_table_sql(request);
    const std::string sql =
        "DO $recover$ BEGIN "
        "IF EXISTS (SELECT 1 FROM information_schema.columns "
        "WHERE table_schema=" + quote_pg_literal(resolve_db_schema()) +
        " AND table_name=" + quote_pg_literal(
            build_table_prefix(
                request->project_id(), request->user_id(), request->case_id())
            + "slow_road_way") +
        " AND column_name='capacity_slow_assignment_backup') THEN "
        "UPDATE " + road +
        " SET capacity=capacity_slow_assignment_backup "
        "WHERE capacity_slow_assignment_backup IS NOT NULL; "
        "ALTER TABLE " + road +
        " DROP COLUMN capacity_slow_assignment_backup; "
        "END IF; END $recover$;";
    return execute_slow_capacity_assignment_sql(request, sql, error);
}

static bool activate_slow_adjusted_capacity(
    const func::ParamsData* request, std::string& error)
{
    const std::string road = slow_assignment_road_table_sql(request);
    const std::string sql =
        "BEGIN;"
        "ALTER TABLE " + road +
        " ADD COLUMN capacity_slow_assignment_backup double precision;"
        "UPDATE " + road +
        " SET capacity_slow_assignment_backup=capacity,"
        " capacity=COALESCE(capacity_slow_adj,capacity);"
        "COMMIT;";
    return execute_slow_capacity_assignment_sql(request, sql, error);
}

static void merge_slow_capacity_assignment_result(
    const func::ResultData& capacity_result,
    const func::ResultData& assignment_result,
    func::ResultData* response)
{
    response->CopyFrom(assignment_result);
    response->mutable_summary()->set_stage("slow_capacity_assignment");
    (*response->mutable_summary()->mutable_attributes())["capacity_source"] =
        "capacity_slow_adj";
    (*response->mutable_summary()->mutable_attributes())["capacity_restore"] =
        "original_capacity_restored";
    (*response->mutable_summary()->mutable_attributes())["execution_order"] =
        "slow_capacity_adjustment->base_slow_network";

    const auto& capacity_counts = capacity_result.data().counts().items();
    const auto rows = capacity_counts.find("rows");
    if (rows != capacity_counts.end()) {
        (*response->mutable_data()->mutable_counts()->mutable_items())["capacity_rows"] =
            rows->second;
    }
    const auto affected = capacity_counts.find("affected_rows");
    if (affected != capacity_counts.end()) {
        (*response->mutable_data()->mutable_counts()->mutable_items())[
            "capacity_affected_rows"] = affected->second;
    }
    const auto& capacity_metrics = capacity_result.data().metrics().items();
    for (const auto& item : capacity_metrics) {
        (*response->mutable_data()->mutable_metrics()->mutable_items())[
            "capacity_" + item.first] = item.second;
    }
    const auto& capacity_tables = capacity_result.data().tables().items();
    const auto impact = capacity_tables.find("impact_table");
    if (impact != capacity_tables.end()) {
        (*response->mutable_data()->mutable_tables()->mutable_items())["impact_table"] =
            impact->second;
    }
}

static grpc::Status run_slow_capacity_assignment_sync(
    const func::ParamsData* request, func::ResultData* response)
{
    response->clear_data();
    response->clear_summary();
    response->mutable_summary()->set_stage("slow_capacity_assignment");
    if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
        response->set_code(-1);
        response->set_message(
            "project_id/user_id must be > 0 and case_id must be >= 0.");
        return grpc::Status::OK;
    }

    std::string db_error;
    if (!recover_slow_assignment_capacity(request, db_error)) {
        response->set_code(-2);
        response->set_message("Failed to recover original capacity: " + db_error);
        return grpc::Status::OK;
    }

    func::ResultData capacity_result;
    run_slow_capacity_adjustment_sync(request, &capacity_result);
    if (capacity_result.code() != 1) {
        response->CopyFrom(capacity_result);
        response->mutable_summary()->set_stage("slow_capacity_assignment");
        return grpc::Status::OK;
    }

    if (!activate_slow_adjusted_capacity(request, db_error)) {
        response->set_code(-2);
        response->set_message("Failed to activate capacity_slow_adj: " + db_error);
        response->mutable_summary()->set_stage("slow_capacity_assignment");
        return grpc::Status::OK;
    }

    func::ResultData assignment_result;
    run_base_network_sync(
        /*mode_suffix=*/"slow_", "slow_capacity_assignment", request, &assignment_result);

    std::string restore_error;
    const bool restored = recover_slow_assignment_capacity(request, restore_error);
    if (!restored) {
        response->CopyFrom(assignment_result);
        response->set_code(-2);
        response->set_message(
            "Assignment finished, but original capacity restoration failed: "
            + restore_error);
        response->mutable_summary()->set_stage("slow_capacity_assignment");
        (*response->mutable_summary()->mutable_attributes())["capacity_restore"] =
            "failed";
        return grpc::Status::OK;
    }

    merge_slow_capacity_assignment_result(
        capacity_result, assignment_result, response);
    if (response->code() == 1) {
        response->set_message(
            "Slow capacity adjustment and assignment completed successfully.");
    }
    return grpc::Status::OK;
}

static void write_slow_capacity_assignment_frame(
    grpc::ServerWriter<func::ProgressData>* writer,
    int code,
    int percent,
    int done,
    const std::string& title,
    const std::string& message,
    const func::ResultData* result = nullptr)
{
    func::ProgressData frame;
    frame.set_code(code);
    frame.set_message(message);
    frame.mutable_data()->set_percent(percent);
    frame.mutable_data()->set_done(done);
    frame.mutable_data()->set_title(title);
    if (result) frame.mutable_result()->CopyFrom(*result);
    writer->Write(frame);
}

static grpc::Status run_slow_capacity_assignment_stream_impl(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer)
{
    if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
        func::ResultData failed;
        failed.set_code(-1);
        failed.set_message(
            "project_id/user_id must be > 0 and case_id must be >= 0.");
        failed.mutable_summary()->set_stage("slow_capacity_assignment");
        write_slow_capacity_assignment_frame(
            writer, -1, 100, 1, "Failed", failed.message(), &failed);
        return grpc::Status::OK;
    }

    write_slow_capacity_assignment_frame(
        writer, 0, 5, 0, "Preparing",
        "Recovering original capacity if a previous run was interrupted.");

    std::string db_error;
    if (!recover_slow_assignment_capacity(request, db_error)) {
        func::ResultData failed;
        failed.set_code(-2);
        failed.set_message("Failed to recover original capacity: " + db_error);
        failed.mutable_summary()->set_stage("slow_capacity_assignment");
        write_slow_capacity_assignment_frame(
            writer, -2, 100, 1, "Failed", failed.message(), &failed);
        return grpc::Status::OK;
    }

    write_slow_capacity_assignment_frame(
        writer, 0, 15, 0, "Calculating capacity impact",
        "Running slow_capacity_adjustment.");
    func::ResultData capacity_result;
    run_slow_capacity_adjustment_sync(request, &capacity_result);
    if (capacity_result.code() != 1) {
        write_slow_capacity_assignment_frame(
            writer, capacity_result.code(), 100, 1, "Failed",
            capacity_result.message(), &capacity_result);
        return grpc::Status::OK;
    }

    write_slow_capacity_assignment_frame(
        writer, 0, 40, 0, "Applying adjusted capacity",
        "Temporarily replacing capacity with capacity_slow_adj.");
    if (!activate_slow_adjusted_capacity(request, db_error)) {
        func::ResultData failed;
        failed.set_code(-2);
        failed.set_message("Failed to activate capacity_slow_adj: " + db_error);
        failed.mutable_summary()->set_stage("slow_capacity_assignment");
        write_slow_capacity_assignment_frame(
            writer, -2, 100, 1, "Failed", failed.message(), &failed);
        return grpc::Status::OK;
    }

    write_slow_capacity_assignment_frame(
        writer, 0, 55, 0, "Running adjusted assignment",
        "Running base_slow_network with adjusted capacity.");
    func::ResultData assignment_result;
    run_base_network_sync(
        /*mode_suffix=*/"slow_", "slow_capacity_assignment", request, &assignment_result);

    write_slow_capacity_assignment_frame(
        writer, 0, 90, 0, "Restoring capacity",
        "Restoring the original slow_road_way.capacity values.");
    std::string restore_error;
    if (!recover_slow_assignment_capacity(request, restore_error)) {
        assignment_result.set_code(-2);
        assignment_result.set_message(
            "Assignment finished, but original capacity restoration failed: "
            + restore_error);
        assignment_result.mutable_summary()->set_stage("slow_capacity_assignment");
        (*assignment_result.mutable_summary()->mutable_attributes())["capacity_restore"] =
            "failed";
        write_slow_capacity_assignment_frame(
            writer, -2, 100, 1, "Failed",
            assignment_result.message(), &assignment_result);
        return grpc::Status::OK;
    }

    func::ResultData combined;
    merge_slow_capacity_assignment_result(
        capacity_result, assignment_result, &combined);
    if (combined.code() == 1) {
        combined.set_message(
            "Slow capacity adjustment and assignment completed successfully.");
    }
    write_slow_capacity_assignment_frame(
        writer, combined.code(), 100, 1,
        combined.code() == 1 ? "Completed" : "Failed",
        combined.message(), &combined);
    return grpc::Status::OK;
}
// ── 方案评审辅助：合并两阶段结果（中文 message / 统一 code）────────────────────
static std::string review_zh_title(const std::string& review_label)
{
    if (review_label == "review_road") return "机动车方案评审";
    if (review_label == "review_slow") return "慢行方案评审";
    if (review_label == "review_pt") return "公交方案评审";
    return review_label;
}

static std::string review_assign_phase_zh(const std::string& review_label)
{
    if (review_label == "review_road") return "机动车交通分配";
    if (review_label == "review_slow") return "慢行交通分配";
    if (review_label == "review_pt") return "公交交通分配";
    return "交通分配";
}

static int merge_review_code(const func::ResultData& assign_result, const func::ResultData& diag_result)
{
    if (assign_result.code() < 0) return assign_result.code();
    if (diag_result.code() == -99 || diag_result.code() == -1) return diag_result.code();
    if (diag_result.code() == 2) return 2;
    return 1;
}

static std::string merge_review_message(const std::string& review_label,
                                        const func::ResultData& assign_result,
                                        const func::ResultData& diag_result)
{
    const std::string title = review_zh_title(review_label);
    const std::string assign_phase = review_assign_phase_zh(review_label);
    if (assign_result.code() < 0) {
        return title + "失败：" + assign_phase + "阶段未完成。原因：" + assign_result.message();
    }
    if (diag_result.code() == -99 || diag_result.code() == -1) {
        return title + "失败：" + assign_phase + "已完成（" + assign_result.message()
               + "），但宏观诊断阶段失败。原因：" + diag_result.message();
    }
    if (diag_result.code() == 2) {
        return title + "部分完成：" + assign_phase + "成功（" + assign_result.message()
               + "）；" + diag_result.message();
    }
    return title + "完成：" + assign_phase + "成功（" + assign_result.message()
           + "）；" + diag_result.message();
}

static std::string review_phase1_fail_message(const std::string& review_label,
                                              const func::ResultData& assign_result)
{
    return review_zh_title(review_label) + "失败：" + review_assign_phase_zh(review_label)
           + "阶段未完成。原因：" + assign_result.message();
}

static void write_review_done_frame(const std::string& review_label,
                                    const func::ResultData& combined,
                                    grpc::ServerWriter<func::ProgressData>* writer,
                                    const std::string& title_suffix = "完成")
{
    func::ProgressData done_frame;
    done_frame.set_code(combined.code());
    done_frame.set_message(combined.message());
    done_frame.mutable_data()->set_percent(100);
    done_frame.mutable_data()->set_done(1);
    done_frame.mutable_data()->set_title(review_zh_title(review_label) + title_suffix);
    done_frame.mutable_result()->CopyFrom(combined);
    writer->Write(done_frame);
}

static void merge_review_result(const std::string& review_label,
                                 const func::ResultData& assign_result,
                                 const func::ResultData& diag_result,
                                 func::ResultData* out)
{
    out->set_code(merge_review_code(assign_result, diag_result));
    out->set_message(merge_review_message(review_label, assign_result, diag_result));
    out->clear_data();
    out->clear_summary();
    for (auto& kv : assign_result.data().tables().items())
        (*out->mutable_data()->mutable_tables()->mutable_items())[kv.first] = kv.second;
    for (auto& kv : diag_result.data().tables().items())
        (*out->mutable_data()->mutable_tables()->mutable_items())[kv.first] = kv.second;
    for (auto& kv : assign_result.data().counts().items())
        (*out->mutable_data()->mutable_counts()->mutable_items())[kv.first] = kv.second;
    for (auto& kv : diag_result.data().counts().items())
        (*out->mutable_data()->mutable_counts()->mutable_items())[kv.first] = kv.second;
    out->mutable_summary()->set_stage(review_label);
    (*out->mutable_summary()->mutable_attributes())["assign_code"] = std::to_string(assign_result.code());
    (*out->mutable_summary()->mutable_attributes())["diag_code"] = std::to_string(diag_result.code());
    (*out->mutable_summary()->mutable_attributes())["assign_message"] = assign_result.message();
    (*out->mutable_summary()->mutable_attributes())["diag_message"] = diag_result.message();
    for (const auto& log : assign_result.summary().logs())
        *out->mutable_summary()->mutable_logs()->Add() = log;
    for (const auto& log : diag_result.summary().logs())
        *out->mutable_summary()->mutable_logs()->Add() = log;
}

// 机动车/慢行评审 — 同步
static grpc::Status run_review_sync(const std::string& mode_suffix,
                                     const std::string& assign_label,
                                     const std::string& diag_module,
                                     const std::string& review_label,
                                     const func::ParamsData* request,
                                     func::ResultData* response)
{
    func::ResultData assign_result;
    grpc::Status s = run_base_network_sync(mode_suffix, assign_label, request, &assign_result);
    if (assign_result.code() < 0) {
        *response = assign_result;
        response->set_message(review_phase1_fail_message(review_label, assign_result));
        return s;
    }
    func::ResultData diag_result;
    tna_local_bridge::run_diagnosis_sync(diag_module, request, &diag_result);
    merge_review_result(review_label, assign_result, diag_result, response);
    return grpc::Status::OK;
}

// 机动车/慢行评审 — 流式（phase1: 0-55%，phase2: 55-100%）
static grpc::Status run_review_stream(const std::string& mode_suffix,
                                       const std::string& assign_label,
                                       const std::string& diag_module,
                                       const std::string& review_label,
                                       const func::ParamsData* request,
                                       grpc::ServerWriter<func::ProgressData>* writer)
{
    func::ResultData assign_result;
    bool ok = run_base_network_stream_impl(mode_suffix, assign_label, request, writer,
                                           0, 55, false, &assign_result);
    if (!ok) return grpc::Status::OK;

    if (assign_result.code() < 0) {
        func::ProgressData done_frame;
        done_frame.set_code(assign_result.code());
        done_frame.set_message(review_phase1_fail_message(review_label, assign_result));
        done_frame.mutable_data()->set_done(1);
        done_frame.mutable_result()->CopyFrom(assign_result);
        writer->Write(done_frame);
        return grpc::Status::OK;
    }

    func::ResultData diag_result;
    tna_local_bridge::run_diagnosis_stream_ranged(diag_module, request, writer, 55, 100, &diag_result);

    func::ResultData combined;
    merge_review_result(review_label, assign_result, diag_result, &combined);
    write_review_done_frame(review_label, combined, writer);
    return grpc::Status::OK;
}

// 公交评审 — 同步
static grpc::Status run_review_pt_sync(const func::ParamsData* request, func::ResultData* response)
{
    func::ResultData assign_result;
    tna_local_bridge::run_transit_assignment_sync(request, &assign_result);
    if (assign_result.code() < 0) {
        *response = assign_result;
        response->set_message(review_phase1_fail_message("review_pt", assign_result));
        return grpc::Status::OK;
    }
    func::ResultData diag_result;
    tna_local_bridge::run_diagnosis_sync("pt_macro", request, &diag_result);
    merge_review_result("review_pt", assign_result, diag_result, response);
    return grpc::Status::OK;
}

// 公交评审 — 流式（phase1: 0-55%，phase2: 55-100%）
static grpc::Status run_review_pt_stream(const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer)
{
    func::ResultData assign_result;
    tna_local_bridge::run_transit_assignment_stream_ranged(request, writer, 0, 55, &assign_result);
    if (assign_result.code() < 0) {
        func::ProgressData done_frame;
        done_frame.set_code(assign_result.code());
        done_frame.set_message(review_phase1_fail_message("review_pt", assign_result));
        done_frame.mutable_data()->set_done(1);
        done_frame.mutable_result()->CopyFrom(assign_result);
        writer->Write(done_frame);
        return grpc::Status::OK;
    }

    func::ResultData diag_result;
    tna_local_bridge::run_diagnosis_stream_ranged("pt_macro", request, writer, 55, 100, &diag_result);

    func::ResultData combined;
    merge_review_result("review_pt", assign_result, diag_result, &combined);
    write_review_done_frame("review_pt", combined, writer);
    return grpc::Status::OK;
}

class FuncServiceImpl final : public func::FuncService::Service {
public:
    // 机动车交通分配（严格不可达 OD — 主接口）
    grpc::Status base_motor_network(grpc::ServerContext* context,
                                    const func::ParamsData* request,
                                    func::ResultData* response) override {
        return run_base_network_sync(/*mode_suffix=*/"", "base_motor_network", request, response);
    }

    grpc::Status base_motor_network_stream(grpc::ServerContext* context,
                                           const func::ParamsData* request,
                                           grpc::ServerWriter<func::ProgressData>* writer) override {
        return run_base_network_stream(/*mode_suffix=*/"", "base_motor_network", request, writer);
    }

    // 旧版备份（兼容保留）
    grpc::Status base_motor_network_bak(grpc::ServerContext* context,
                                         const func::ParamsData* request,
                                         func::ResultData* response) override {
        return run_base_network_sync(/*mode_suffix=*/"", "base_motor_network_bak", request, response);
    }

    grpc::Status base_motor_network_stream_bak(grpc::ServerContext* context,
                                               const func::ParamsData* request,
                                               grpc::ServerWriter<func::ProgressData>* writer) override {
        return run_base_network_stream(/*mode_suffix=*/"", "base_motor_network_bak", request, writer);
    }

    // 机动车交通分配 — 多质心连杆（按 link_osmid 去重，每小区最多 N 条）
    grpc::Status base_motor_network_multi_centroid(grpc::ServerContext* context,
                                                    const func::ParamsData* request,
                                                    func::ResultData* response) override {
        CentroidConnectorEnvConfig cfg =
            parse_centroid_config_from_param2(request->param2(), "multi_osm");
        return run_base_network_sync(
            /*mode_suffix=*/"", "base_motor_network_multi_centroid", request, response, &cfg);
    }

    grpc::Status base_motor_network_multi_centroid_stream(grpc::ServerContext* context,
                                                           const func::ParamsData* request,
                                                           grpc::ServerWriter<func::ProgressData>* writer) override {
        CentroidConnectorEnvConfig cfg =
            parse_centroid_config_from_param2(request->param2(), "multi_osm");
        return run_base_network_stream(
            /*mode_suffix=*/"", "base_motor_network_multi_centroid", request, writer, &cfg);
    }

    // 慢行（非机动车）均衡分配（mode_suffix="slow_"）。算法与机动车完全一致，
    // 只是读写表名前缀加 "slow_"，与机动车数据互不污染。
    grpc::Status base_slow_network(grpc::ServerContext* context,
                                   const func::ParamsData* request,
                                   func::ResultData* response) override {
        return run_base_network_sync(/*mode_suffix=*/"slow_", "base_slow_network", request, response);
    }

    grpc::Status base_slow_network_stream(grpc::ServerContext* context,
                                          const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer) override {
        return run_base_network_stream(/*mode_suffix=*/"slow_", "base_slow_network", request, writer);
    }

    grpc::Status slow_capacity_adjustment(
        grpc::ServerContext* context,
        const func::ParamsData* request,
        func::ResultData* response) override {
        return run_slow_capacity_adjustment_sync(request, response);
    }

    grpc::Status slow_capacity_adjustment_stream(
        grpc::ServerContext* context,
        const func::ParamsData* request,
        grpc::ServerWriter<func::ProgressData>* writer) override {
        return run_slow_capacity_adjustment_stream_impl(request, writer);
    }

    grpc::Status slow_capacity_assignment(
        grpc::ServerContext* context,
        const func::ParamsData* request,
        func::ResultData* response) override {
        return run_slow_capacity_assignment_sync(request, response);
    }

    grpc::Status slow_capacity_assignment_stream(
        grpc::ServerContext* context,
        const func::ParamsData* request,
        grpc::ServerWriter<func::ProgressData>* writer) override {
        return run_slow_capacity_assignment_stream_impl(request, writer);
    }

    grpc::Status build_centroid_connectors(grpc::ServerContext* context,
                                           const func::ParamsData* request,
                                           func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] build_centroid_connectors: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        grpc::Status st = tna_local_bridge::run_centroid_connector_sync(request, response);
        response->mutable_summary()->set_stage("build_centroid_connectors");
        return st;
    }

    grpc::Status build_centroid_connectors_stream(grpc::ServerContext* context,
                                                  const func::ParamsData* request,
                                                  grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] build_centroid_connectors_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_centroid_connector_stream(request, writer);
    }



    // OD 反推 — 多质心连杆（按 link_osmid 去重；与 tool_estimation 同 CLI）
    grpc::Status tool_estimation_multi_centroid(grpc::ServerContext* context,
                                                const func::ParamsData* request,
                                                func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_estimation_multi_centroid: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_centroid_config_from_param2(request->param2(), "multi_osm");
        grpc::Status st = tna_local_bridge::run_tool_estimation_sync(request, response, cfg);
        response->mutable_summary()->set_stage("tool_estimation_multi_centroid");
        (*response->mutable_summary()->mutable_attributes())["centroid_connector_mode"] = cfg.mode;
        (*response->mutable_summary()->mutable_attributes())["max_connectors_per_zone"] =
            std::to_string(cfg.max_connectors_per_zone);
        return st;
    }

    grpc::Status tool_estimation_stream_multi_centroid(grpc::ServerContext* context,
                                                       const func::ParamsData* request,
                                                       grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_estimation_stream_multi_centroid: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_centroid_config_from_param2(request->param2(), "multi_osm");
        return tna_local_bridge::run_tool_estimation_stream(request, writer, cfg);
    }

    // OD 反推 — 方案前缀 + other_observation（多质心连杆）
    grpc::Status tool_estimation_case(grpc::ServerContext* context,
                                    const func::ParamsData* request,
                                    func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        if (request->case_id() < 0) {
            response->set_code(-1);
            response->set_message("case_id 须 >= 0（方案编号）。");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_estimation_case: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_scheme_observation_centroid_config(request->param2());
        grpc::Status st = tna_local_bridge::run_tool_estimation_sync(request, response, cfg);
        response->mutable_summary()->set_stage("tool_estimation_case");
        (*response->mutable_summary()->mutable_attributes())["table_prefix"] = "scheme";
        (*response->mutable_summary()->mutable_attributes())["observation_source"] = "other_observation";
        (*response->mutable_summary()->mutable_attributes())["centroid_connector_mode"] = cfg.mode;
        (*response->mutable_summary()->mutable_attributes())["max_connectors_per_zone"] =
            std::to_string(cfg.max_connectors_per_zone);
        return st;
    }

    grpc::Status tool_estimation_stream_case(grpc::ServerContext* context,
                                             const func::ParamsData* request,
                                             grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        if (request->case_id() < 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("case_id 须 >= 0（方案编号）。");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_estimation_stream_case: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_scheme_observation_centroid_config(request->param2());
        return tna_local_bridge::run_tool_estimation_stream(request, writer, cfg);
    }

    grpc::Status od_estimate_local(grpc::ServerContext* context,
                                   const func::ParamsData* request,
                                   func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] od_estimate_local: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_od_estimate_local_centroid_config(request->param2());
        grpc::Status st = tna_local_bridge::run_od_estimate_local_sync(request, response, cfg);
        response->mutable_summary()->set_stage("od_estimate_local");
        (*response->mutable_summary()->mutable_attributes())["table_prefix"] = "tool";
        (*response->mutable_summary()->mutable_attributes())["observation_source"] = "other_observation";
        (*response->mutable_summary()->mutable_attributes())["algorithm"] = "local_tnadriver";
        return st;
    }

    grpc::Status od_estimate_local_stream(grpc::ServerContext* context,
                                          const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] od_estimate_local_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_od_estimate_local_centroid_config(request->param2());
        return tna_local_bridge::run_od_estimate_local_stream(request, writer, cfg);
    }

    grpc::Status tool_estimation(grpc::ServerContext* context,
                                          const func::ParamsData* request,
                                          func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        if (request->case_id() < 0) {
            response->set_code(-1);
            response->set_message("case_id 须 >= 0（方案编号）。");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_estimation: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_tool_od_estimation_local_centroid_config(request->param2());
        grpc::Status st = tna_local_bridge::run_tool_od_estimation_local_sync(request, response, cfg);
        response->mutable_summary()->set_stage("tool_estimation");
        (*response->mutable_summary()->mutable_attributes())["table_prefix"] = "tool";
        (*response->mutable_summary()->mutable_attributes())["observation_source"] = "other_observation";
        (*response->mutable_summary()->mutable_attributes())["algorithm"] = "local_tnadriver";
        return st;
    }

    grpc::Status tool_estimation_stream(grpc::ServerContext* context,
                                                 const func::ParamsData* request,
                                                 grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        if (request->case_id() < 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("case_id 须 >= 0（方案编号）。");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_estimation_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        tna_local_bridge::CentroidConnectorEnvConfig cfg =
            tna_local_bridge::parse_tool_od_estimation_local_centroid_config(request->param2());
        return tna_local_bridge::run_tool_od_estimation_local_stream(request, writer, cfg);
    }

    grpc::Status tool_trip_generation(grpc::ServerContext* context,
                                 const func::ParamsData* request,
                                 func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_trip_generation: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_trip_sync("trip_generation", request, response);
    }

    grpc::Status tool_trip_generation_stream(grpc::ServerContext* context,
                                        const func::ParamsData* request,
                                        grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_trip_generation_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_trip_stream("trip_generation", "正在执行出行生成...", request, writer);
    }

    grpc::Status tool_trip_distribution(grpc::ServerContext* context,
                                   const func::ParamsData* request,
                                   func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_trip_distribution: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_trip_sync("trip_distribution", request, response);
    }

    grpc::Status tool_trip_distribution_stream(grpc::ServerContext* context,
                                          const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_trip_distribution_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_trip_stream("trip_distribution", "正在执行出行分布...", request, writer);
    }

    grpc::Status tool_trip_model_all(grpc::ServerContext* context,
                                const func::ParamsData* request,
                                func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_trip_model_all: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_trip_sync("trip_model_all", request, response);
    }

    grpc::Status tool_trip_model_all_stream(grpc::ServerContext* context,
                                       const func::ParamsData* request,
                                       grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] tool_trip_model_all_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_trip_stream("trip_model_all", "正在执行四阶段法全流程...", request, writer);
    }

    grpc::Status base_transit_assignment(grpc::ServerContext* context,
                                    const func::ParamsData* request,
                                    func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_transit_assignment: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_transit_assignment_sync(request, response);
    }

    grpc::Status base_transit_assignment_stream(grpc::ServerContext* context,
                                           const func::ParamsData* request,
                                           grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_transit_assignment_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_transit_assignment_stream(request, writer);
    }

    grpc::Status base_transit_assignment_new(grpc::ServerContext* context,
                                             const func::ParamsData* request,
                                             func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_transit_assignment_new: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        grpc::Status st = tna_local_bridge::run_transit_assignment_new_sync(request, response);
        response->mutable_summary()->set_stage("base_transit_assignment_new");
        return st;
    }

    grpc::Status base_transit_assignment_new_stream(grpc::ServerContext* context,
                                                    const func::ParamsData* request,
                                                    grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_transit_assignment_new_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_transit_assignment_new_stream(request, writer);
    }

    grpc::Status diagnosis_pt_meso(grpc::ServerContext* context,
                                   const func::ParamsData* request,
                                   func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_pt_meso: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_sync("pt_meso", request, response);
    }

    grpc::Status diagnosis_pt_meso_stream(grpc::ServerContext* context,
                                          const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_pt_meso_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_stream("pt_meso", "正在执行公共交通中观诊断...", request, writer);
    }

    grpc::Status diagnosis_pt_macro(grpc::ServerContext* context,
                                    const func::ParamsData* request,
                                    func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_pt_macro: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_sync("pt_macro", request, response);
    }

    grpc::Status diagnosis_pt_macro_stream(grpc::ServerContext* context,
                                           const func::ParamsData* request,
                                           grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_pt_macro_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_stream("pt_macro", "正在执行公共交通宏观诊断...", request, writer);
    }

    grpc::Status diagnosis_road_macro(grpc::ServerContext* context,
                                      const func::ParamsData* request,
                                      func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_road_macro: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_sync("road_macro", request, response);
    }

    grpc::Status diagnosis_road_macro_stream(grpc::ServerContext* context,
                                             const func::ParamsData* request,
                                             grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_road_macro_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_stream("road_macro", "正在执行道路交通宏观诊断...", request, writer);
    }

    grpc::Status diagnosis_road_micro(grpc::ServerContext* context,
                                      const func::ParamsData* request,
                                      func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_road_micro: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_sync("road_micro", request, response);
    }

    grpc::Status diagnosis_road_micro_stream(grpc::ServerContext* context,
                                             const func::ParamsData* request,
                                             grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_road_micro_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_stream("road_micro", "正在执行道路交通微观诊断...", request, writer);
    }


    grpc::Status od_trace(grpc::ServerContext* context,
                          const func::ParamsData* request,
                          func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] od_trace: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_od_trace_sync(request, response);
    }


    grpc::Status od_trace_stream(grpc::ServerContext* context,
                                 const func::ParamsData* request,
                                 grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] od_trace_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_od_trace_stream(request, writer);
    }

    grpc::Status diagnosis_slow_macro(grpc::ServerContext* context,
                                      const func::ParamsData* request,
                                      func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_slow_macro: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_sync("slow_macro", request, response);
    }

    grpc::Status diagnosis_slow_macro_stream(grpc::ServerContext* context,
                                             const func::ParamsData* request,
                                             grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_slow_macro_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_stream("slow_macro", "正在执行慢行交通宏观诊断...", request, writer);
    }

    grpc::Status diagnosis_slow_meso_micro(grpc::ServerContext* context,
                                           const func::ParamsData* request,
                                           func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_slow_meso_micro: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_sync("slow_meso_micro", request, response);
    }

    grpc::Status diagnosis_slow_meso_micro_stream(grpc::ServerContext* context,
                                                  const func::ParamsData* request,
                                                  grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] diagnosis_slow_meso_micro_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_diagnosis_stream("slow_meso_micro", "正在执行慢行交通中微观诊断...", request, writer);
    }

    // 方案评审：机动车分配 + 道路宏观诊断
    grpc::Status review_road(grpc::ServerContext* context,
                              const func::ParamsData* request,
                              func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data(); response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] review_road: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return run_review_sync("", "base_motor_network", "road_macro", "review_road", request, response);
    }

    grpc::Status review_road_stream(grpc::ServerContext* context,
                                     const func::ParamsData* request,
                                     grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame; frame.set_code(-1);
            frame.set_message("入参错误：project_id 与 user_id 须为正整数");
            frame.mutable_data()->set_done(1); writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] review_road_stream: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return run_review_stream("", "base_motor_network", "road_macro", "review_road", request, writer);
    }

    // 方案评审：慢行分配 + 慢行宏观诊断
    grpc::Status review_slow(grpc::ServerContext* context,
                              const func::ParamsData* request,
                              func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data(); response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] review_slow: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return run_review_sync("slow_", "base_slow_network", "slow_macro", "review_slow", request, response);
    }

    grpc::Status review_slow_stream(grpc::ServerContext* context,
                                     const func::ParamsData* request,
                                     grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame; frame.set_code(-1);
            frame.set_message("入参错误：project_id 与 user_id 须为正整数");
            frame.mutable_data()->set_done(1); writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] review_slow_stream: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return run_review_stream("slow_", "base_slow_network", "slow_macro", "review_slow", request, writer);
    }

    // 方案评审：公交分配 + 公交宏观诊断
    grpc::Status review_pt(grpc::ServerContext* context,
                            const func::ParamsData* request,
                            func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data(); response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] review_pt: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return run_review_pt_sync(request, response);
    }

    grpc::Status review_pt_stream(grpc::ServerContext* context,
                                   const func::ParamsData* request,
                                   grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame; frame.set_code(-1);
            frame.set_message("入参错误：project_id 与 user_id 须为正整数");
            frame.mutable_data()->set_done(1); writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] review_pt_stream: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return run_review_pt_stream(request, writer);
    }

    // 基础数据分析报告
    grpc::Status base_data_report(grpc::ServerContext* context,
                                  const func::ParamsData* request,
                                  func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            response->clear_summary();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_data_report: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_base_report_sync(request, response);
    }

    grpc::Status base_data_report_stream(grpc::ServerContext* context,
                                         const func::ParamsData* request,
                                         grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_data_report_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_base_report_stream(request, writer);
    }

    // 成本效益分析报告
    grpc::Status cost_benefit_report(grpc::ServerContext* context,
                                     const func::ParamsData* request,
                                     func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required.");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] cost_benefit_report: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_cost_benefit_report_sync(request, response);
    }

    grpc::Status cost_benefit_report_stream(grpc::ServerContext* context,
                                            const func::ParamsData* request,
                                            grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] cost_benefit_report_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_cost_benefit_report_stream(request, writer);
    }

    // 方案对比分析报告
    grpc::Status scheme_compare_report(grpc::ServerContext* context,
                                       const func::ParamsData* request,
                                       func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
            response->set_code(-1);
            response->set_message(
                "project_id/user_id must be > 0; case_id is the 改造方案 ID (>=0). "
                "现状方案请传 param2 JSON: {\"base_case_id\":<int>, ...}.");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] scheme_compare_report: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_scheme_compare_report_sync(request, response);
    }

    grpc::Status scheme_compare_report_stream(grpc::ServerContext* context,
                                              const func::ParamsData* request,
                                              grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message(
                "project_id/user_id must be > 0; case_id is the 改造方案 ID (>=0). "
                "现状方案请传 param2 JSON: {\"base_case_id\":<int>, ...}.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] scheme_compare_report_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_scheme_compare_report_stream(request, writer);
    }

    // 基础方案诊断指标报告（case_id 为当前基础方案 ID，与普通方案相同规则，勿强制为 0）
    grpc::Status base_scheme_diagnosis_report(grpc::ServerContext* context,
                                              const func::ParamsData* request,
                                              func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
            response->set_code(-1);
            response->set_message(
                "project_id/user_id must be > 0; case_id must be >= 0 (current base scheme id, "
                "table prefix project{p}_user{u}_case{cid}_).");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_scheme_diagnosis_report: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_base_scheme_diagnosis_report_sync(request, response);
    }

    grpc::Status base_scheme_diagnosis_report_stream(grpc::ServerContext* context,
                                                     const func::ParamsData* request,
                                                     grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message(
                "project_id/user_id must be > 0; case_id must be >= 0 for base scheme diagnosis report.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] base_scheme_diagnosis_report_stream: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_base_scheme_diagnosis_report_stream(request, writer);
    }

    // 普通/改造方案诊断指标报告（case_id 为当前方案 ID，规则同基础方案报告）
    grpc::Status scheme_diagnosis_report(grpc::ServerContext* context,
                                         const func::ParamsData* request,
                                         func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
            response->set_code(-1);
            response->set_message(
                "project_id/user_id must be > 0; case_id must be >= 0 for scheme diagnosis report. "
                "Renovation compare: param2 {\"base_case_id\":<base_scheme_id>,...}.");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] scheme_diagnosis_report: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_scheme_diagnosis_report_sync(request, response);
    }

    grpc::Status scheme_diagnosis_report_stream(grpc::ServerContext* context,
                                                const func::ParamsData* request,
                                                grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0 || request->case_id() < 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message(
                "project_id/user_id must be > 0; case_id must be >= 0 for scheme diagnosis report.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] scheme_diagnosis_report_stream: project=" << request->project_id()
                  << " user=" << request->user_id() << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_scheme_diagnosis_report_stream(request, writer);
    }

    // 承载力报告导出
    grpc::Status report_export(grpc::ServerContext* context,
                               const func::ParamsData* request,
                               func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] report_export: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_report_sync(1, request, response);
    }

    grpc::Status report_export_stream(grpc::ServerContext* context,
                                      const func::ParamsData* request,
                                      grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] report_export_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_report_stream(1, "正在生成承载力报告...", request, writer);
    }

    // 仿真流量导出：TESSNG Flow.csv
    grpc::Status flow_export(grpc::ServerContext* context,
                             const func::ParamsData* request,
                             func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] flow_export: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_report_sync(2, request, response);
    }

    grpc::Status flow_export_stream(grpc::ServerContext* context,
                                    const func::ParamsData* request,
                                    grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] flow_export_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_report_stream(2, "正在导出仿真流量 Flow.csv...", request, writer);
    }

    // 仿真路网导出：OSM 文件拷贝
    grpc::Status sim_network_export(grpc::ServerContext* context,
                                    const func::ParamsData* request,
                                    func::ResultData* response) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            response->clear_data();
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] sim_network_export: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_report_sync(3, request, response);
    }

    grpc::Status sim_network_export_stream(grpc::ServerContext* context,
                                           const func::ParamsData* request,
                                           grpc::ServerWriter<func::ProgressData>* writer) override {
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        std::cout << "[gRPC] sim_network_export_stream: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id() << std::endl;
        return tna_local_bridge::run_report_stream(3, "正在准备仿真路网 OSM 文件...", request, writer);
    }

};

static FuncServiceImpl service;
static std::unique_ptr<grpc::Server> g_server;

int main(int argc, char** argv)
{
    // 加载数据库配置
    g_db_conn_str = load_db_config();
    if (g_db_conn_str.empty()) {
        std::cerr << "[gRPC] WARNING: No database config found. Requests will fail until db.conf is provided." << std::endl;
    }

    const char* env_addr = std::getenv("TNA_GRPC_LISTEN");
    std::string server_address = env_addr ? std::string(env_addr) : "0.0.0.0:50051";
    if (argc >= 2) server_address = argv[1];

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    builder.SetMaxReceiveMessageSize(256 * 1024 * 1024);

    g_server = builder.BuildAndStart();
    if (!g_server) {
        std::cerr << "[gRPC] Failed to start server on " << server_address << std::endl;
        return 1;
    }

    std::cout << "[gRPC] TNA FuncService listening on " << server_address
              << " (default schema=" << resolve_db_schema() << ")" << std::endl;
    g_server->Wait();
    return 0;
}
