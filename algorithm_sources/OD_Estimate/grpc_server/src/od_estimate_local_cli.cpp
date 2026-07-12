#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef _WIN32
#include "../../../Include/postgresql/libpq-fe.h"
#else
#include <libpq-fe.h>
#endif

#if __has_include("stdafx.h")
#  include "stdafx.h"
#else
#  include "header/stdafx.h"
#endif

namespace {

std::string trim_copy(const std::string& input)
{
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    return input.substr(begin, end - begin);
}

std::string unescape_value(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char nxt = value[i + 1];
            if (nxt == 'n') out += '\n';
            else if (nxt == 'r') out += '\r';
            else out += nxt;
            ++i;
            continue;
        }
        out += value[i];
    }
    return out;
}

std::string escape_value(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out += ch;
    }
    return out;
}

void append_progress_file(const std::string& path, int percent, const std::string& title)
{
    if (path.empty()) return;
    int safe_percent = percent;
    if (safe_percent < 0) safe_percent = 0;
    if (safe_percent > 100) safe_percent = 100;
    std::ofstream out(path, std::ios::out | std::ios::app);
    out << "percent=" << safe_percent << "\ttitle=" << escape_value(title) << "\n";
}

std::string g_progress_file;
int g_last_progress_percent = 20;

void tool_progress_callback(int iteration, double progress)
{
    int safe_percent = static_cast<int>(progress);
    if (safe_percent < 0) safe_percent = 0;
    if (safe_percent > 99) safe_percent = 99;
    int overall_percent = 20 + (safe_percent * 70 / 100);
    if (overall_percent < g_last_progress_percent) overall_percent = g_last_progress_percent;
    if (overall_percent > 95) overall_percent = 95;
    g_last_progress_percent = overall_percent;
    std::ostringstream title;
    title << "OD 估计迭代第 " << iteration << " 步";
    title << "（阶段值 " << progress << "）";
    append_progress_file(g_progress_file, overall_percent, title.str());
}

std::map<std::string, std::string> read_kv_file(const std::string& path)
{
    std::map<std::string, std::string> kv;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        kv[line.substr(0, pos)] = unescape_value(line.substr(pos + 1));
    }
    return kv;
}

void write_response_file(const std::string& path,
                          int code,
                          const std::string& message,
                          const std::map<std::string, std::string>& payload)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    out << "code=" << code << "\n";
    out << "message=" << escape_value(message) << "\n";
    for (const auto& item : payload) {
        out << item.first << "=" << escape_value(item.second) << "\n";
    }
}

std::string current_exe_dir()
{
    char exe_path[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) return ".";
    std::string exe_dir(exe_path, len);
    auto slash = exe_dir.rfind('/');
    if (slash != std::string::npos) exe_dir = exe_dir.substr(0, slash);
    return exe_dir;
}

std::string load_db_config(const std::string& param1)
{
    if (!trim_copy(param1).empty()) {
        std::string conn = param1;
        for (char& ch : conn) {
            if (ch == ';' || ch == '\n' || ch == '\r') ch = ' ';
        }
        return trim_copy(conn);
    }

    std::vector<std::string> candidates;
    const char* env_conf = std::getenv("TNA_DB_CONF");
    if (env_conf && *env_conf) candidates.push_back(env_conf);

    std::string exe_dir = current_exe_dir();
    auto parent_slash = exe_dir.rfind('/');
    if (parent_slash != std::string::npos) candidates.push_back(exe_dir.substr(0, parent_slash) + "/db.conf");
    candidates.push_back(exe_dir + "/db.conf");

    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::string conn, line;
        while (std::getline(f, line)) {
            line = trim_copy(line);
            if (line.empty() || line[0] == '#') continue;
            if (!conn.empty()) conn += ' ';
            conn += line;
        }
        if (!conn.empty()) return conn;
    }
    return "";
}

std::string resolve_db_schema()
{
    const char* schema_env = std::getenv("TNA_DB_SCHEMA");
    return (schema_env && *schema_env) ? std::string(schema_env) : std::string("user_project");
}

struct SchemaTableName {
    std::string schema;
    std::string table;
};

SchemaTableName split_schema_table_name(const std::string& qualified_name)
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

bool postgres_table_exists(PGconn* conn, const std::string& qualified_name)
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

bool validate_input_tables(const std::string& db_conn_str,
                           const std::string& network_table,
                           const std::string& od_table,
                           const std::string& observed_table,
                           std::string& message)
{
    message.clear();
    PGconn* conn = PQconnectdb(db_conn_str.c_str());
    if (conn == nullptr) {
        message = "校验输入表失败：无法创建数据库连接。";
        return false;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        message = std::string("校验输入表失败：") + PQerrorMessage(conn);
        PQfinish(conn);
        return false;
    }

    bool network_exists = postgres_table_exists(conn, network_table);
    bool od_exists = postgres_table_exists(conn, od_table);
    bool observed_exists = postgres_table_exists(conn, observed_table);
    PQfinish(conn);

    if (network_exists && od_exists && observed_exists) {
        return true;
    }

    if (!observed_exists) {
        message = "未找到路段流量观测表: " + observed_table
            + "。请先导入路段流量观测数据（link_id、flow）。";
        return false;
    }
    if (!network_exists || !od_exists) {
        message = "缺少必需输入表。路网表=" + network_table
            + (network_exists ? "（存在）" : "（缺失）")
            + "，OD表=" + od_table
            + (od_exists ? "（存在）" : "（缺失）");
        return false;
    }
    return true;
}

std::string build_tool_table_prefix(int32_t project_id, int32_t user_id)
{
    return "project" + std::to_string(project_id) + "_user" + std::to_string(user_id) + "_";
}

std::string build_scheme_table_prefix(int32_t project_id, int32_t user_id, int32_t case_id)
{
    return "project" + std::to_string(project_id) + "_user" + std::to_string(user_id)
           + "_case" + std::to_string(case_id) + "_";
}

void derive_table_names(int32_t project_id, int32_t user_id, int32_t case_id,
                        bool use_scheme_prefix,
                        std::string& network_table, std::string& od_table,
                        std::string& observed_table, std::string& scenario_prefix)
{
    scenario_prefix = use_scheme_prefix
        ? build_scheme_table_prefix(project_id, user_id, case_id)
        : build_tool_table_prefix(project_id, user_id);
    std::string schema = resolve_db_schema();
    network_table = schema + "." + scenario_prefix + "road_way";
    od_table = schema + "." + scenario_prefix + "other_od";
    observed_table = schema + "." + scenario_prefix + "other_observation";
}

std::map<std::string, std::string> build_result_payload(int iter,
                                                         const std::string& network_table,
                                                         const std::string& od_table,
                                                         const std::string& observed_table,
                                                         double rmse,
                                                         double rgp)
{
    std::map<std::string, std::string> payload;
    payload["data.table.network_table"] = network_table;
    payload["data.table.od_table"] = od_table;
    payload["data.table.observed_table"] = observed_table;
    const char* stage_env = std::getenv("TNA_OD_LOCAL_STAGE");
    payload["summary.stage"] = (stage_env && *stage_env) ? stage_env : "od_estimate_local";
    payload["data.count.iter"] = std::to_string(iter);
    {
        std::ostringstream oss;
        oss << rmse;
        payload["data.metric.rmse"] = oss.str();
    }
    {
        std::ostringstream oss;
        oss << rgp;
        payload["data.metric.rgp"] = oss.str();
    }
    return payload;
}

bool json_find_number(const std::string& json, const std::string& key, double& out)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return false;
    char* end_ptr = nullptr;
    out = std::strtod(json.c_str() + pos, &end_ptr);
    return end_ptr != json.c_str() + pos;
}

bool json_find_int(const std::string& json, const std::string& key, int& out)
{
    double d = 0.0;
    if (!json_find_number(json, key, d)) return false;
    out = static_cast<int>(d);
    return true;
}

bool json_find_bool(const std::string& json, const std::string& key, bool& out)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    const std::string tail = json.substr(pos + 1, 12);
    if (tail.find("true") != std::string::npos) {
        out = true;
        return true;
    }
    if (tail.find("false") != std::string::npos) {
        out = false;
        return true;
    }
    return false;
}

bool peek_use_roadway_observed(const std::string& param2)
{
    if (param2.empty()) {
        return false;
    }
    bool use_roadway = false;
    return json_find_bool(param2, "use_roadway_observed", use_roadway) && use_roadway;
}

bool peek_scheme_observation_prefix(const std::string& param2,
                                    const std::map<std::string, std::string>& req)
{
    const char* tool_obs = std::getenv("TNA_OD_TOOL_OBSERVATION");
    if (tool_obs != nullptr && std::string(tool_obs) == "1") {
        return false;
    }
    const char* env_mode = std::getenv("TNA_OD_TABLE_PREFIX_MODE");
    if (env_mode != nullptr && std::string(env_mode) == "scheme_observation") {
        return true;
    }
    auto it = req.find("table_prefix_mode");
    if (it != req.end() && it->second == "scheme_observation") {
        return true;
    }
    if (param2.empty()) {
        return false;
    }
    bool use_scheme_obs = false;
    return json_find_bool(param2, "use_scheme_observation", use_scheme_obs) && use_scheme_obs;
}

bool peek_delete_type10_after_od(const std::string& param2,
                               const std::map<std::string, std::string>& req)
{
    const char* env = std::getenv("TNA_DELETE_TYPE10_AFTER_OD");
    const bool env_on = (env != nullptr && std::string(env) == "1");
    auto it = req.find("delete_type10_after_od");
    if (it != req.end()) {
        const std::string& v = it->second;
        if (v == "0" || v == "false" || v == "False") return false;
        if (v == "1" || v == "true" || v == "True") return true;
    }
    bool explicit_val = false;
    if (json_find_bool(param2, "delete_type10_after_od", explicit_val)) {
        return explicit_val;
    }
    if (!param2.empty()) {
        const size_t ap = param2.find("\"algorithm_params\"");
        const std::string sub = (ap != std::string::npos) ? param2.substr(ap) : param2;
        if (json_find_bool(sub, "delete_type10_after_od", explicit_val)) {
            return explicit_val;
        }
    }
    return env_on;
}

int delete_type10_connectors(const std::string& db_conn_str,
                             const std::string& network_table,
                             std::string& message)
{
    message.clear();
    PGconn* conn = PQconnectdb(db_conn_str.c_str());
    if (conn == nullptr) {
        message = "删除质心连杆失败：无法创建数据库连接。";
        return -1;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        message = std::string("删除质心连杆失败：") + PQerrorMessage(conn);
        PQfinish(conn);
        return -1;
    }

    const std::string sql = "DELETE FROM " + network_table + " WHERE \"type\" = 10";
    PGresult* res = PQexec(conn, sql.c_str());
    if (res == nullptr || PQresultStatus(res) != PGRES_COMMAND_OK) {
        message = std::string("删除质心连杆失败：")
            + (res != nullptr ? PQerrorMessage(conn) : "PQexec 返回空");
        if (res != nullptr) PQclear(res);
        PQfinish(conn);
        return -1;
    }
    const int deleted = std::atoi(PQcmdTuples(res));
    PQclear(res);
    PQfinish(conn);
    return deleted;
}

bool peek_tool_observation_relaxed_defaults(const std::string& param2,
                                            const std::map<std::string, std::string>& req)
{
    const char* env_mode = std::getenv("TNA_OD_TOOL_OBSERVATION");
    if (env_mode != nullptr && std::string(env_mode) == "1") {
        return true;
    }
    auto it = req.find("table_prefix_mode");
    if (it != req.end() && it->second == "tool_observation") {
        return true;
    }
    if (param2.empty()) {
        return false;
    }
    bool use_tool_obs = false;
    return json_find_bool(param2, "use_tool_observation", use_tool_obs) && use_tool_obs;
}

bool resolve_use_scheme_prefix(const std::string& param2,
                               const std::map<std::string, std::string>& req)
{
    return peek_use_roadway_observed(param2) || peek_scheme_observation_prefix(param2, req);
}

void apply_algorithm_params_json(OD_ESTIMATION& odes, const std::string& param2)
{
    if (param2.empty()) return;
    std::string json = param2;
    const size_t ap = json.find("\"algorithm_params\"");
    if (ap != std::string::npos) json = json.substr(ap);

    double d = 0.0;
    int i = 0;
    bool b = false;

    if (json_find_int(json, "max_iter_fast", i) && i > 0) {
        odes.SetSolveFastParams(i, odes.solveConvFast);
    }
    if (json_find_number(json, "conv_fast", d) && d > 0.0) {
        odes.SetSolveFastParams(odes.solveMaxIterFast, static_cast<floatType>(d));
    }
    if (json_find_int(json, "max_iter_strict", i) && i > 0) {
        odes.SetSolveStrictParams(i, odes.solveConvStrict);
    }
    if (json_find_number(json, "conv_strict", d) && d > 0.0) {
        odes.SetSolveStrictParams(odes.solveMaxIterStrict, static_cast<floatType>(d));
    }
    // 兼容旧字段：max_iter / conv 表示严收敛档
    if (json_find_int(json, "max_iter", i) && i > 0) {
        odes.SetSolveStrictParams(i, odes.solveConvStrict);
    }
    if (!json_find_int(json, "max_iter", i)) {
        if (json_find_int(json, "maxiter", i) && i > 0) {
            odes.SetSolveStrictParams(i, odes.solveConvStrict);
        }
    }
    if (json_find_number(json, "conv", d) && d > 0.0) {
        odes.SetSolveStrictParams(odes.solveMaxIterStrict, static_cast<floatType>(d));
    }

    if (json_find_int(json, "mainloop_maxiter", i) && i > 0) {
        odes.SetMainloop_maxiter(i);
    }
    if (json_find_number(json, "mainloop_conv", d) && d > 0.0) {
        odes.SetMainloop_conv(static_cast<floatType>(d));
    }
    if (json_find_int(json, "armijo_maxiter", i) && i > 0) {
        odes.SetArmijo_maxiter(i);
    }
    if (json_find_number(json, "gamma1", d) && d >= 0.0) {
        odes.SetGamma(static_cast<floatType>(d), odes.gamma2);
    }
    if (json_find_number(json, "gamma2", d) && d >= 0.0) {
        odes.SetGamma(odes.gamma1, static_cast<floatType>(d));
    }
    double obs_mult = 0.0;
    if (json_find_number(json, "obs_weight_multiplier", obs_mult) && obs_mult > 0.0) {
        odes.SetGamma(odes.gamma1, static_cast<floatType>(odes.gamma2 * obs_mult));
    }
    if (json_find_bool(json, "final_strict_solve", b)) {
        odes.SetFinalStrictSolve(b);
    }
    if (json_find_bool(json, "use_roadway_observed", b)) {
        odes.SetUseRoadwayObserved(b);
        if (b) odes.SetUseExternalObservedFlow(false);
    }
}

void apply_local_default_parameters(OD_ESTIMATION& odes)
{
    TNM_FloatFormat::SetFormat(18, 6);
    odes.SetGamma(0.5f, 1.5f);
    odes.SetOblink_ratio(0.3);
    odes.SetMainloop_maxiter(300);
    odes.SetMainloop_conv(1e-4);
    odes.SetArmijo_maxiter(5);
    odes.SetArmijo_stopcriterion(100);
    odes.SetArmijo_coefficient(10.0);
    odes.SetArmijo_alphamax(1000.0);
    odes.SetConv(1e-6f);
    odes.SetMaxIter(500);
    odes.SetLPF(BPRLK);
    odes.SetCostScalar(60);
    odes.SetCostCoef(1.0, 0.0);
    odes.SetUseExternalObservedFlow(true);
    odes.SetUseRoadwayObserved(false);
    odes.SetLocalEstimateMode(true);
    odes.reportIterHistory = false;
    odes.reportLinkDetail = false;
    odes.reportPathDetail = false;
    odes.reportDemandDetail = true;
    odes.reportlinkinforDetail = true;
    odes.reportUpperObjective = true;
}

void apply_default_parameters(OD_ESTIMATION& odes)
{
    TNM_FloatFormat::SetFormat(18, 6);
    odes.SetGamma(0.5);
    odes.SetOblink_ratio(0.3);
    odes.SetMainloop_maxiter(300);
    odes.SetMainloop_conv(1e-4);
    odes.SetArmijo_maxiter(5);
    odes.SetArmijo_stopcriterion(100);
    odes.SetArmijo_coefficient(10.0);
    odes.SetArmijo_alphamax(1000.0);
    odes.SetSolveFastParams(20, 1e-5f);
    odes.SetSolveStrictParams(500, 1e-6f);
    odes.SetFinalStrictSolve(true);
    odes.SetConv(1e-5f);
    odes.SetMaxIter(20);
    odes.SetLPF(BPRLK);
    odes.SetCostScalar(60);
    odes.SetCostCoef(1.0, 0.0);
    odes.SetUseExternalObservedFlow(true);
    odes.SetUseRoadwayObserved(false);
    odes.reportIterHistory = false;
    odes.reportLinkDetail = false;
    odes.reportPathDetail = false;
    odes.reportDemandDetail = true;
    odes.reportlinkinforDetail = true;
    odes.reportUpperObjective = true;
}


void apply_tool_estimation_fast_defaults(OD_ESTIMATION& odes)
{
    // tool_estimation 快速参数写死（与 tool_estimation 外循环同档）
    odes.SetGamma(0.2f, 1.0f);
    odes.SetMainloop_maxiter(30);
    odes.SetMainloop_conv(1e-2f);
    odes.SetSolveFastParams(100, 1e-2f);
    odes.SetSolveStrictParams(100, 1e-2f);
    odes.SetFinalStrictSolve(false);
    odes.SetMaxIter(100);
    odes.SetConv(1e-2f);
}

void apply_scheme_observation_relaxed_defaults(OD_ESTIMATION& odes)
{
    // tool_estimation / tool_estimation_case 默认：多连杆构网；观测权重高于初始 OD
    odes.SetGamma(0.2f, 1.0f);
    odes.SetMainloop_maxiter(30);
    odes.SetMainloop_conv(0.01f);
    odes.SetSolveFastParams(100, 0.01f);
    odes.SetSolveStrictParams(100, 0.01f);
    odes.SetFinalStrictSolve(false);
    odes.SetMaxIter(100);
    odes.SetConv(0.01f);
}

int run_od_estimate_local(const std::map<std::string, std::string>& req,
                          std::string& message,
                          std::map<std::string, std::string>& payload,
                          const std::string& progress_file)
{
    int32_t project_id = std::atoi(req.count("project_id") ? req.at("project_id").c_str() : "0");
    int32_t user_id = std::atoi(req.count("user_id") ? req.at("user_id").c_str() : "0");
    int32_t case_id = std::atoi(req.count("case_id") ? req.at("case_id").c_str() : "0");
    std::string param1 = req.count("param1") ? req.at("param1") : std::string();
    std::string param2 = req.count("param2") ? req.at("param2") : std::string();

    payload.clear();

    if (project_id <= 0 || user_id <= 0) {
        message = "参数错误：project_id、user_id 须为正整数。";
        return -1;
    }

    std::string db_conn_str = load_db_config(param1);
    if (db_conn_str.empty()) {
        message = "服务器未加载数据库配置，请检查 db.conf。";
        return -1;
    }

    const bool scheme_observation = peek_scheme_observation_prefix(param2, req);
    if (scheme_observation && case_id < 0) {
        message = "参数错误：方案前缀 OD 反推须 case_id >= 0。";
        return -1;
    }
    // 工具前缀（默认）；方案前缀（TNA_OD_TABLE_PREFIX_MODE=scheme_observation）；承载力读 road_way.volume
    const bool use_scheme_prefix = peek_use_roadway_observed(param2) || scheme_observation;
    std::string network_table, od_table, observed_table, scenario_prefix;
    std::string schema = resolve_db_schema();
    derive_table_names(project_id, user_id, case_id, use_scheme_prefix,
                        network_table, od_table, observed_table, scenario_prefix);

    if (!validate_input_tables(db_conn_str, network_table, od_table, observed_table, message)) {
        return -2;
    }

    try {
        append_progress_file(progress_file, 5, "正在加载网络与OD数据...");
        g_progress_file = progress_file;
        g_last_progress_percent = 20;
        OD_ESTIMATION odes;
        const char* stage_env = std::getenv("TNA_OD_LOCAL_STAGE");
        const bool is_tool_od_local = stage_env
            && std::string(stage_env) == "tool_estimation";
        if (is_tool_od_local) {
            TNM_FloatFormat::SetFormat(18, 6);
            odes.SetOblink_ratio(0.3);
            odes.SetArmijo_maxiter(5);
            odes.SetArmijo_stopcriterion(100);
            odes.SetArmijo_coefficient(10.0);
            odes.SetArmijo_alphamax(1000.0);
            odes.SetLPF(BPRLK);
            odes.SetCostScalar(60);
            odes.SetCostCoef(1.0, 0.0);
            odes.SetUseExternalObservedFlow(true);
            odes.SetUseRoadwayObserved(false);
            odes.SetLocalEstimateMode(true);
            odes.reportIterHistory = false;
            odes.reportLinkDetail = false;
            odes.reportPathDetail = false;
            odes.reportDemandDetail = true;
            odes.reportlinkinforDetail = true;
            odes.reportUpperObjective = true;
            apply_tool_estimation_fast_defaults(odes);
        } else {
            apply_local_default_parameters(odes);
            apply_algorithm_params_json(odes, param2);
        }
        odes.SetLocalEstimateMode(true);
        odes.SetUseExternalObservedFlow(true);
        if (peek_use_roadway_observed(param2)) {
            odes.SetUseRoadwayObserved(true);
            odes.SetUseExternalObservedFlow(false);
            odes.SetLocalEstimateMode(false);
        }
        odes.SetRoleName(schema);
        odes.SetResultTablePrefix(scenario_prefix);
        odes.SetDbConnStr(db_conn_str.c_str());
        odes.SetNetworkTableName(network_table);
        odes.SetODTableName(od_table);
        odes.SetObservedTableName(observed_table);
        odes.SetProgressCallback(tool_progress_callback);

#ifdef _WIN32
        _putenv_s("PG_SCENARIO_PREFIX", scenario_prefix.c_str());
        _putenv_s("TNA_OD_MODE", "");
#else
        setenv("PG_SCENARIO_PREFIX", scenario_prefix.c_str(), 1);
        setenv("TNA_OD_MODE", "", 1);
#endif

        int build_ret = odes.Build("grpc_od_estimate_local", "", NETPOSTGRESQL);
        if (build_ret != 0) {
            message = trim_copy(TNM_GetLastError());
            if (message.empty()) {
                message = "构网失败，错误码 " + std::to_string(build_ret);
            }
            return -2;
        }

        append_progress_file(progress_file, 20, "数据加载完成，正在执行 OD 估计...");
        if (is_tool_od_local) {
            apply_tool_estimation_fast_defaults(odes);
        }
        TNM_ResetLastError();
        odes.overall_process();
        std::string solve_error = trim_copy(TNM_GetLastError());
        if (odes.ReachError() || !solve_error.empty()) {
            message = solve_error.empty() ? "OD 反推求解失败。" : solve_error;
            return -2;
        }
        int solve_percent = 90;
        append_progress_file(progress_file, solve_percent, "OD 估计完成，正在写入结果...");
        int report_ret = odes.ReportPG();
        if (report_ret != 0) {
            message = "结果写入数据库失败，错误码 " + std::to_string(report_ret);
            return -3;
        }

        append_progress_file(progress_file, 95, "结果写入完成");
        payload = build_result_payload(odes.l, network_table, od_table, observed_table, odes.RMSE, odes.RGP);
        if (peek_delete_type10_after_od(param2, req)) {
            std::string delete_msg;
            const int deleted = delete_type10_connectors(db_conn_str, network_table, delete_msg);
            if (deleted < 0) {
                g_progress_file.clear();
                g_last_progress_percent = 20;
                message = delete_msg.empty() ? "反推后删除质心连杆失败。" : delete_msg;
                return -3;
            }
            payload["data.count.type10_deleted"] = std::to_string(deleted);
            append_progress_file(progress_file, 98,
                "已删除质心连杆 type=10（" + std::to_string(deleted) + " 条）");
        }
        g_progress_file.clear();
        g_last_progress_percent = 20;
        message = "成功";
        return 1;
    } catch (const std::exception& e) {
        g_progress_file.clear();
        g_last_progress_percent = 20;
        message = std::string("运行异常：") + e.what();
        return -99;
    } catch (...) {
        g_progress_file.clear();
        g_last_progress_percent = 20;
        message = "未知异常";
        return -99;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::string request_file;
    std::string response_file;
    std::string progress_file;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--request-file" && i + 1 < argc) {
            request_file = argv[++i];
        } else if (arg == "--response-file" && i + 1 < argc) {
            response_file = argv[++i];
        } else if (arg == "--progress-file" && i + 1 < argc) {
            progress_file = argv[++i];
        }
    }

    if (request_file.empty() || response_file.empty()) {
        std::cerr << "usage: od_estimate_local_cli --request-file <path> --response-file <path> [--progress-file <path>]" << std::endl;
        return 2;
    }

    auto req = read_kv_file(request_file);
    std::string message;
    std::map<std::string, std::string> payload;
    int code = run_od_estimate_local(req, message, payload, progress_file);
    write_response_file(response_file, code, message, payload);
    return (code == 1) ? 0 : 1;
}
