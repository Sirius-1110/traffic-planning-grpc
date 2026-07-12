#include "local_bridge.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <vector>

namespace {

std::string shell_quote(const std::string& value)
{
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\"'\"'";
        else out += ch;
    }
    out += "'";
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

std::string helper_root()
{
    const char* env_root = std::getenv("TNA_TOTAL_HELPERS");
    if (env_root && *env_root) return std::string(env_root);
    return current_exe_dir() + "/helpers";
}

std::string tool_helper_path()
{
    const char* env_path = std::getenv("TNA_TOOL_EST_HELPER");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/tool_estimation_cli";
}

std::string od_estimate_local_helper_path()
{
    const char* env_path = std::getenv("TNA_OD_ESTIMATE_LOCAL_HELPER");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/od_estimate_local_cli";
}

std::string transit_helper_path()
{
    const char* env_path = std::getenv("TNA_TRANSIT_HELPER");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/transit_assignment_cli";
}

std::string transit_assignment_new_helper_path()
{
    const char* env_path = std::getenv("TNA_TRANSIT_NEW_HELPER");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/transit_assignment_new_cli";
}


std::string trip_bridge_path()
{
    const char* env_path = std::getenv("TNA_TRIP_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/trip_bridge.py";
}

std::string centroid_connector_bridge_path()
{
    const char* env_path = std::getenv("TNA_CENTROID_CONNECTOR_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/centroid_connector_bridge.py";
}

std::string trip_python_path()
{
    const char* env_path = std::getenv("TNA_TRIP_PYTHON");
    if (env_path && *env_path) return std::string(env_path);
    return "python3";
}

std::string trip_repo_root()
{
    const char* env_path = std::getenv("TNA_TRIP_ROOT");
    if (env_path && *env_path) return std::string(env_path);
    return "";
}

std::string diagnosis_bridge_path()
{
    const char* env_path = std::getenv("TNA_DIAGNOSIS_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/diagnosis_bridge.py";
}

std::string diagnosis_repo_root()
{
    const char* env_path = std::getenv("TNA_DIAGNOSIS_ROOT");
    if (env_path && *env_path) return std::string(env_path);
    return "";
}

std::string diagnosis_python_path()
{
    const char* env_path = std::getenv("TNA_DIAGNOSIS_PYTHON");
    if (env_path && *env_path) return std::string(env_path);
    return trip_python_path();  // 默认与 trip bridge 共用同一个 python 解释器
}

std::string make_temp_file(const std::string& prefix)
{
    std::string tpl = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0) return "";
    close(fd);
    return std::string(buf.data());
}

bool write_request_file(const std::string& path, const func::ParamsData* request)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << "project_id=" << request->project_id() << "\n";
    out << "user_id=" << request->user_id() << "\n";
    out << "case_id=" << request->case_id() << "\n";
    out << "param1=" << escape_value(request->param1()) << "\n";
    out << "param2=" << escape_value(request->param2()) << "\n";
    return true;
}

std::map<std::string, std::string> read_kv_file(const std::string& path)
{
    std::map<std::string, std::string> result;
    std::ifstream in(path);
    if (!in.is_open()) return result;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        result[line.substr(0, pos)] = unescape_value(line.substr(pos + 1));
    }
    return result;
}

void cleanup_temp_file(const std::string& path)
{
    if (!path.empty()) ::unlink(path.c_str());
}

void fill_result_body_from_map(const std::map<std::string, std::string>& kv,
                               const std::string& prefix,
                               func::ResultBody* body);

void fill_result_summary_from_map(const std::map<std::string, std::string>& kv,
                                  const std::string& prefix,
                                  func::ResultSummary* summary);

void fill_result_from_map(const std::map<std::string, std::string>& kv, func::ResultData* response, const std::string& fallback_message)
{
    auto it_code = kv.find("code");
    auto it_msg = kv.find("message");
    int code = -99;
    if (it_code != kv.end()) {
        try {
            code = std::stoi(it_code->second);
        } catch (...) {
            code = -99;
        }
    }
    response->set_code(code);
    response->set_message(it_msg != kv.end() ? it_msg->second : fallback_message);
    response->clear_data();
    response->clear_summary();
    fill_result_body_from_map(kv, "", response->mutable_data());
    fill_result_summary_from_map(kv, "", response->mutable_summary());
}

func::ProgressData make_progress_frame(int code,
                                       const std::string& message,
                                       int percent,
                                       int done,
                                       const std::string& title,
                                       const func::ResultData* result = nullptr);

int helper_idle_timeout_seconds()
{
    const char* value = std::getenv("TNA_HELPER_IDLE_TIMEOUT_SEC");
    if (value && *value) {
        try {
            int parsed = std::stoi(value);
            if (parsed > 0) return parsed;
        } catch (...) {
        }
    }
    return 3600;
}

void terminate_process_group(const std::string& pid_text)
{
    try {
        int pid = std::stoi(pid_text);
        if (pid > 0) {
            ::kill(-pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ::kill(-pid, SIGKILL);
        }
    } catch (...) {
    }
}

struct ProgressEvent {
    int percent = 0;
    std::string title;
};

std::vector<ProgressEvent> read_progress_events(const std::string& path, std::streamoff& offset)
{
    std::vector<ProgressEvent> events;
    std::ifstream in(path);
    if (!in.is_open()) return events;
    in.seekg(offset);
    std::string line;
    while (std::getline(in, line)) {
        ProgressEvent event;
        size_t pos = 0;
        while (pos < line.size()) {
            size_t tab = line.find('\t', pos);
            std::string part = line.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos);
            auto eq = part.find('=');
            if (eq != std::string::npos) {
                std::string key = part.substr(0, eq);
                std::string value = unescape_value(part.substr(eq + 1));
                if (key == "percent") {
                    try {
                        event.percent = std::stoi(value);
                    } catch (...) {
                        event.percent = 0;
                    }
                } else if (key == "title") {
                    event.title = value;
                }
            }
            if (tab == std::string::npos) break;
            pos = tab + 1;
        }
        if (!event.title.empty()) {
            if (event.percent < 0) event.percent = 0;
            if (event.percent > 100) event.percent = 100;
            events.push_back(event);
        }
    }
    offset = static_cast<std::streamoff>(in.tellg());
    if (offset < 0) {
        in.clear();
        in.seekg(0, std::ios::end);
        offset = static_cast<std::streamoff>(in.tellg());
        if (offset < 0) offset = 0;
    }
    return events;
}

int run_command_with_progress(const std::string& command,
                              const std::string& progress_file,
                              const std::string& status_file,
                              grpc::ServerWriter<func::ProgressData>* writer,
                              int start_percent,
                              int end_percent,
                              const std::string& running_message)
{
    std::string grouped_command = "( " + command + "; printf '%s' $? > " + shell_quote(status_file) + " )";
    std::string shell_command = "setsid sh -c " + shell_quote(grouped_command) + " > /dev/null 2>&1 & echo $!";
    FILE* pipe = ::popen(shell_command.c_str(), "r");
    if (!pipe) return -1;

    char buf[64] = {};
    std::string pid_text;
    if (std::fgets(buf, sizeof(buf), pipe)) pid_text = buf;
    int popen_rc = ::pclose(pipe);
    if (popen_rc == -1 || pid_text.empty()) return -1;

    std::streamoff offset = 0;
    int last_percent = start_percent;
    std::string last_title;
    auto last_activity = std::chrono::steady_clock::now();
    int idle_timeout_seconds = helper_idle_timeout_seconds();
    while (true) {
        auto events = read_progress_events(progress_file, offset);
        for (const auto& event : events) {
            int mapped = event.percent;
            if (mapped < start_percent) mapped = start_percent;
            if (mapped > end_percent) mapped = end_percent;
            if (mapped == last_percent && event.title == last_title) continue;
            writer->Write(make_progress_frame(0, running_message, mapped, 0, event.title));
            last_percent = mapped;
            last_title = event.title;
            last_activity = std::chrono::steady_clock::now();
        }

        std::ifstream status_in(status_file);
        if (status_in.good()) break;
        auto now = std::chrono::steady_clock::now();
        auto idle_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity).count();
        if (idle_seconds >= idle_timeout_seconds) {
            terminate_process_group(pid_text);
            std::ofstream status_out(status_file, std::ios::out | std::ios::trunc);
            status_out << "-124";
            writer->Write(make_progress_frame(-124, running_message, last_percent, 0,
                "算法执行超过 " + std::to_string(idle_timeout_seconds) + " 秒未产生新进度，已停止本次任务"));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    auto final_events = read_progress_events(progress_file, offset);
    for (const auto& event : final_events) {
        int mapped = event.percent;
        if (mapped < start_percent) mapped = start_percent;
        if (mapped > end_percent) mapped = end_percent;
        if (mapped == last_percent && event.title == last_title) continue;
        writer->Write(make_progress_frame(0, running_message, mapped, 0, event.title));
        last_percent = mapped;
        last_title = event.title;
    }

    std::ifstream status_in(status_file);
    int rc = -1;
    if (status_in.is_open()) {
        status_in >> rc;
    }
    return rc;
}

bool starts_with(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

void collect_indexed_strings(const std::map<std::string, std::string>& kv,
                             const std::string& prefix,
                             const std::string& name,
                             google::protobuf::RepeatedPtrField<std::string>* out)
{
    std::vector<std::pair<int, std::string>> ordered;
    std::string full_prefix = prefix + name + ".";
    for (const auto& item : kv) {
        if (!starts_with(item.first, full_prefix)) continue;
        std::string index_text = item.first.substr(full_prefix.size());
        try {
            ordered.emplace_back(std::stoi(index_text), item.second);
        } catch (...) {
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    for (const auto& item : ordered) {
        *out->Add() = item.second;
    }
}

template <typename MapType>
void fill_string_map_with_prefix(const std::map<std::string, std::string>& kv,
                                 const std::string& prefix,
                                 const std::string& name,
                                 MapType* out)
{
    std::string full_prefix = prefix + name + ".";
    for (const auto& item : kv) {
        if (!starts_with(item.first, full_prefix)) continue;
        (*out)[item.first.substr(full_prefix.size())] = item.second;
    }
}

void fill_int64_map_with_prefix(const std::map<std::string, std::string>& kv,
                                const std::string& prefix,
                                const std::string& name,
                                google::protobuf::Map<std::string, int64_t>* out)
{
    std::string full_prefix = prefix + name + ".";
    for (const auto& item : kv) {
        if (!starts_with(item.first, full_prefix)) continue;
        try {
            (*out)[item.first.substr(full_prefix.size())] = std::stoll(item.second);
        } catch (...) {
        }
    }
}

void fill_double_map_with_prefix(const std::map<std::string, std::string>& kv,
                                 const std::string& prefix,
                                 const std::string& name,
                                 google::protobuf::Map<std::string, double>* out)
{
    std::string full_prefix = prefix + name + ".";
    for (const auto& item : kv) {
        if (!starts_with(item.first, full_prefix)) continue;
        try {
            (*out)[item.first.substr(full_prefix.size())] = std::stod(item.second);
        } catch (...) {
        }
    }
}

void fill_result_body_from_map(const std::map<std::string, std::string>& kv,
                               const std::string& prefix,
                               func::ResultBody* body)
{
    body->Clear();
    fill_string_map_with_prefix(kv, prefix, "data.table", body->mutable_tables()->mutable_items());
    fill_int64_map_with_prefix(kv, prefix, "data.count", body->mutable_counts()->mutable_items());
    fill_double_map_with_prefix(kv, prefix, "data.metric", body->mutable_metrics()->mutable_items());
}

void fill_result_summary_from_map(const std::map<std::string, std::string>& kv,
                                  const std::string& prefix,
                                  func::ResultSummary* summary)
{
    summary->Clear();
    auto it_stage = kv.find(prefix + "summary.stage");
    if (it_stage != kv.end()) {
        summary->set_stage(it_stage->second);
    }
    collect_indexed_strings(kv, prefix, "summary.log", summary->mutable_logs());
    fill_string_map_with_prefix(kv, prefix, "summary.attr", summary->mutable_attributes());
    fill_int64_map_with_prefix(kv, prefix, "summary.count", summary->mutable_counts());
    fill_double_map_with_prefix(kv, prefix, "summary.metric", summary->mutable_metrics());
}

func::ProgressData make_progress_frame(int code,
                                       const std::string& message,
                                       int percent,
                                       int done,
                                       const std::string& title,
                                       const func::ResultData* result)
{
    func::ProgressData frame;
    frame.set_code(code);
    frame.set_message(message);
    frame.mutable_data()->set_percent(percent);
    frame.mutable_data()->set_done(done);
    frame.mutable_data()->set_title(title);
    if (result) {
        frame.mutable_result()->CopyFrom(*result);
    }
    return frame;
}

grpc::Status run_request_response_command(const std::string& command, const func::ParamsData* request, func::ResultData* response, const std::string& failure_message)
{
    std::string request_file = make_temp_file("tna_req_");
    std::string response_file = make_temp_file("tna_resp_");
    if (request_file.empty() || response_file.empty()) {
        response->set_code(-99);
        response->set_message("failed to create temporary files");
        response->clear_data();
        response->clear_summary();
        cleanup_temp_file(request_file);
        cleanup_temp_file(response_file);
        return grpc::Status::OK;
    }

    if (!write_request_file(request_file, request)) {
        response->set_code(-99);
        response->set_message("failed to write request file");
        response->clear_data();
        response->clear_summary();
        cleanup_temp_file(request_file);
        cleanup_temp_file(response_file);
        return grpc::Status::OK;
    }

    std::string full_command = command + " --request-file " + shell_quote(request_file) + " --response-file " + shell_quote(response_file);
    int rc = std::system(full_command.c_str());
    auto kv = read_kv_file(response_file);
    if (kv.empty()) {
        response->set_code(-99);
        response->set_message(failure_message + " exit_code=" + std::to_string(rc));
        response->clear_data();
        response->clear_summary();
    } else {
        fill_result_from_map(kv, response, failure_message);
    }

    cleanup_temp_file(request_file);
    cleanup_temp_file(response_file);
    return grpc::Status::OK;
}

grpc::Status run_request_response_command_streaming(const std::string& command,
                                                    const func::ParamsData* request,
                                                    func::ResultData* response,
                                                    const std::string& failure_message,
                                                    grpc::ServerWriter<func::ProgressData>* writer,
                                                    const std::string& progress_file,
                                                    const std::string& status_file,
                                                    int start_percent,
                                                    int end_percent,
                                                    const std::string& running_message)
{
    std::string request_file = make_temp_file("tna_req_");
    std::string response_file = make_temp_file("tna_resp_");
    if (request_file.empty() || response_file.empty() || progress_file.empty() || status_file.empty()) {
        response->set_code(-99);
        response->set_message("failed to create temporary files");
        response->clear_data();
        response->clear_summary();
        cleanup_temp_file(request_file);
        cleanup_temp_file(response_file);
        cleanup_temp_file(progress_file);
        cleanup_temp_file(status_file);
        return grpc::Status::OK;
    }

    if (!write_request_file(request_file, request)) {
        response->set_code(-99);
        response->set_message("failed to write request file");
        response->clear_data();
        response->clear_summary();
        cleanup_temp_file(request_file);
        cleanup_temp_file(response_file);
        cleanup_temp_file(progress_file);
        cleanup_temp_file(status_file);
        return grpc::Status::OK;
    }

    cleanup_temp_file(progress_file);
    cleanup_temp_file(status_file);

    std::string full_command = command + " --request-file " + shell_quote(request_file)
        + " --response-file " + shell_quote(response_file)
        + " --progress-file " + shell_quote(progress_file);
    int rc = run_command_with_progress(full_command, progress_file, status_file, writer, start_percent, end_percent, running_message);
    auto kv = read_kv_file(response_file);
    if (kv.empty()) {
        response->set_code(-99);
        response->set_message(failure_message + " exit_code=" + std::to_string(rc));
        response->clear_data();
        response->clear_summary();
    } else {
        fill_result_from_map(kv, response, failure_message);
    }

    cleanup_temp_file(request_file);
    cleanup_temp_file(response_file);
    cleanup_temp_file(progress_file);
    cleanup_temp_file(status_file);
    return grpc::Status::OK;
}

}  // namespace

namespace tna_local_bridge {

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

CentroidConnectorEnvConfig parse_centroid_config_from_param2(
    const std::string& param2, const char* default_mode)
{
    CentroidConnectorEnvConfig cfg;
    cfg.mode = default_mode ? std::string(default_mode) : "single";
    cfg.max_connectors_per_zone = extract_json_int_field(param2, "max_connectors_per_zone", 5);
    if (cfg.max_connectors_per_zone < 1) cfg.max_connectors_per_zone = 1;
    if (cfg.max_connectors_per_zone > 20) cfg.max_connectors_per_zone = 20;
    return cfg;
}

CentroidConnectorEnvConfig parse_scheme_observation_centroid_config(const std::string& param2)
{
    CentroidConnectorEnvConfig cfg = parse_centroid_config_from_param2(param2, "multi_osm");
    cfg.table_prefix_mode = "scheme_observation";
    constexpr int kMinConnectors = 2;
    if (cfg.max_connectors_per_zone < kMinConnectors) {
        cfg.max_connectors_per_zone = kMinConnectors;
    }
    return cfg;
}

CentroidConnectorEnvConfig parse_tool_observation_centroid_config(const std::string& param2)
{
    CentroidConnectorEnvConfig cfg = parse_centroid_config_from_param2(param2, "multi_osm");
    cfg.table_prefix_mode = "tool_observation";
    constexpr int kMinConnectors = 2;
    if (cfg.max_connectors_per_zone < kMinConnectors) {
        cfg.max_connectors_per_zone = kMinConnectors;
    }
    return cfg;
}

CentroidConnectorEnvConfig parse_od_estimate_local_centroid_config(const std::string& param2)
{
    CentroidConnectorEnvConfig cfg = parse_centroid_config_from_param2(param2, "single");
    cfg.table_prefix_mode = "scheme_observation";
    return cfg;
}

CentroidConnectorEnvConfig parse_tool_od_estimation_local_centroid_config(const std::string& param2)
{
    CentroidConnectorEnvConfig cfg = parse_centroid_config_from_param2(param2, "single");
    cfg.table_prefix_mode = "tool_observation";
    return cfg;
}

static std::string build_centroid_env_prefix(const CentroidConnectorEnvConfig& cfg)
{
    std::string env = "TNA_CENTROID_CONNECTOR_MODE=" + cfg.mode + " TNA_CENTROID_MAX_CONNECTORS="
        + std::to_string(cfg.max_connectors_per_zone) + " ";
    if (cfg.table_prefix_mode == "scheme_observation") {
        env += "TNA_OD_TABLE_PREFIX_MODE=scheme_observation ";
    } else if (cfg.table_prefix_mode == "tool_observation") {
        env += "TNA_OD_TOOL_OBSERVATION=1 ";
    }
    return env;
}

static bool param2_field_is_true(const std::string& param2, const char* key)
{
    if (param2.empty()) return false;
    const std::string needle = std::string("\"") + key + "\":true";
    return param2.find(needle) != std::string::npos;
}

static bool param2_field_is_false(const std::string& param2, const char* key)
{
    if (param2.empty()) return false;
    const std::string needle = std::string("\"") + key + "\":false";
    return param2.find(needle) != std::string::npos;
}

static bool default_auto_centroid_prebuild(const std::string& param2)
{
    if (param2_field_is_false(param2, "auto_centroid_prebuild")) return false;
    if (param2_field_is_true(param2, "auto_centroid_prebuild")) return true;
    return true;
}

static bool default_skip_centroid_prebuild(const std::string& param2)
{
    if (param2_field_is_true(param2, "skip_centroid_prebuild")) return true;
    if (param2_field_is_false(param2, "skip_centroid_prebuild")) return false;
    return true;
}

static bool default_delete_type10_after_od(const std::string& param2)
{
    if (param2_field_is_true(param2, "delete_type10_after_od")) return true;
    if (param2_field_is_false(param2, "delete_type10_after_od")) return false;
    return false;
}

static std::string build_centroid_prebuild_param2(
    const std::string& table_prefix_mode,
    const std::string& connector_mode,
    int max_connectors_per_zone,
    bool force_rebuild)
{
    return std::string("{\"table_prefix_mode\":\"") + table_prefix_mode
        + "\",\"connector_mode\":\"" + connector_mode
        + "\",\"max_connectors_per_zone\":" + std::to_string(max_connectors_per_zone)
        + ",\"force_rebuild\":" + (force_rebuild ? "true" : "false") + "}";
}

grpc::Status ensure_centroid_connectors_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const std::string& table_prefix_mode,
    const std::string& connector_mode,
    int max_connectors_per_zone)
{
    func::ParamsData req = *request;
    const bool force = param2_field_is_true(request->param2(), "force_rebuild");
    req.set_param2(build_centroid_prebuild_param2(
        table_prefix_mode, connector_mode, max_connectors_per_zone, force));
    func::ResultData resp;
    grpc::Status st = run_centroid_connector_sync(&req, &resp);
    if (!st.ok()) return st;
    response->CopyFrom(resp);
    return grpc::Status::OK;
}

/** @return false 表示 prebuild 业务失败，failure 已写入 out_failure */
static bool maybe_prebuild_centroid_for_od(
    const func::ParamsData* request,
    func::ResultData* out_failure,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    if (!default_auto_centroid_prebuild(request->param2())) {
        return true;
    }
    const std::string prefix_mode =
        (centroid_cfg.table_prefix_mode == "scheme_observation") ? "scheme" : "tool";
    func::ResultData pre;
    grpc::Status st = ensure_centroid_connectors_sync(
        request, &pre, prefix_mode, centroid_cfg.mode, centroid_cfg.max_connectors_per_zone);
    if (!st.ok()) {
        out_failure->set_code(-99);
        out_failure->set_message(st.error_message());
        return false;
    }
    if (pre.code() != 1) {
        out_failure->CopyFrom(pre);
        return false;
    }
    return true;
}

static std::string tool_estimation_command(const CentroidConnectorEnvConfig* centroid_cfg, const std::string& param2)
{
    std::string env;
    if (centroid_cfg) {
        env = build_centroid_env_prefix(*centroid_cfg);
    }
    env += std::string("TNA_SKIP_CENTROID_PREBUILD=")
        + (default_skip_centroid_prebuild(param2) ? "1" : "0") + " ";
    env += std::string("TNA_DELETE_TYPE10_AFTER_OD=")
        + (default_delete_type10_after_od(param2) ? "1" : "0") + " ";
    return env + shell_quote(tool_helper_path());
}

static std::string od_estimate_local_command(const CentroidConnectorEnvConfig* centroid_cfg, const std::string& param2)
{
    std::string env;
    if (centroid_cfg) {
        env = build_centroid_env_prefix(*centroid_cfg);
    }
    if (centroid_cfg && centroid_cfg->table_prefix_mode == "scheme_observation") {
        env += "TNA_OD_TABLE_PREFIX_MODE=scheme_observation TNA_OD_LOCAL_STAGE=od_estimate_local ";
    } else {
        env += "TNA_OD_TOOL_OBSERVATION=1 TNA_OD_LOCAL_STAGE=tool_estimation ";
    }
    const bool skip_prebuild = !param2_field_is_true(param2, "auto_centroid_prebuild");
    env += std::string("TNA_SKIP_CENTROID_PREBUILD=") + (skip_prebuild ? "1" : "0") + " ";
    env += std::string("TNA_DELETE_TYPE10_AFTER_OD=")
        + (default_delete_type10_after_od(param2) ? "1" : "0") + " ";
    return env + shell_quote(od_estimate_local_helper_path());
}

grpc::Status run_tool_estimation_sync(const func::ParamsData* request, func::ResultData* response)
{
    return run_tool_estimation_sync(request, response, CentroidConnectorEnvConfig{});
}

grpc::Status run_tool_estimation_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    func::ResultData pre_err;
    if (!maybe_prebuild_centroid_for_od(request, &pre_err, centroid_cfg)) {
        response->CopyFrom(pre_err);
        return grpc::Status::OK;
    }
    return run_request_response_command(
        tool_estimation_command(&centroid_cfg, request->param2()), request, response, "tool_estimation helper failed");
}

grpc::Status run_tool_estimation_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    CentroidConnectorEnvConfig cfg;
    return run_tool_estimation_stream(request, writer, cfg);
}

grpc::Status run_tool_estimation_stream(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在检查/构建质心连杆..."));
    func::ResultData pre_err;
    if (!maybe_prebuild_centroid_for_od(request, &pre_err, centroid_cfg)) {
        writer->Write(make_progress_frame(pre_err.code(), pre_err.message(), 100, 1, "质心连杆失败", &pre_err));
        return grpc::Status::OK;
    }
    writer->Write(make_progress_frame(0, "Starting", 5, 0, "正在执行 OD 估计..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    run_request_response_command_streaming(
        tool_estimation_command(&centroid_cfg, request->param2()),
        request,
        &response,
        "tool_estimation helper failed",
        writer,
        progress_file,
        status_file,
        5,
        100,
        "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "OD 估计完成", &response));
    return grpc::Status::OK;
}

static bool maybe_prebuild_centroid_for_od_local(
    const func::ParamsData* request,
    func::ResultData* out_failure,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    if (!param2_field_is_true(request->param2(), "auto_centroid_prebuild")) {
        return true;
    }
    return maybe_prebuild_centroid_for_od(request, out_failure, centroid_cfg);
}

grpc::Status run_od_estimate_local_sync(const func::ParamsData* request, func::ResultData* response)
{
    return run_od_estimate_local_sync(request, response, CentroidConnectorEnvConfig{});
}

grpc::Status run_od_estimate_local_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    func::ResultData pre_err;
    if (!maybe_prebuild_centroid_for_od_local(request, &pre_err, centroid_cfg)) {
        response->CopyFrom(pre_err);
        return grpc::Status::OK;
    }
    return run_request_response_command(
        od_estimate_local_command(&centroid_cfg, request->param2()), request, response, "od_estimate_local helper failed");
}

grpc::Status run_od_estimate_local_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    CentroidConnectorEnvConfig cfg;
    return run_od_estimate_local_stream(request, writer, cfg);
}

grpc::Status run_od_estimate_local_stream(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在执行本地 OD 反推..."));
    func::ResultData pre_err;
    if (!maybe_prebuild_centroid_for_od_local(request, &pre_err, centroid_cfg)) {
        writer->Write(make_progress_frame(pre_err.code(), pre_err.message(), 100, 1, "质心连杆失败", &pre_err));
        return grpc::Status::OK;
    }
    writer->Write(make_progress_frame(0, "Starting", 5, 0, "正在执行 OD 估计..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    run_request_response_command_streaming(
        od_estimate_local_command(&centroid_cfg, request->param2()),
        request,
        &response,
        "od_estimate_local helper failed",
        writer,
        progress_file,
        status_file,
        5,
        100,
        "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "OD 估计完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_tool_od_estimation_local_sync(const func::ParamsData* request, func::ResultData* response)
{
    return run_tool_od_estimation_local_sync(request, response, CentroidConnectorEnvConfig{});
}

grpc::Status run_tool_od_estimation_local_sync(
    const func::ParamsData* request,
    func::ResultData* response,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    func::ResultData pre_err;
    if (!maybe_prebuild_centroid_for_od_local(request, &pre_err, centroid_cfg)) {
        response->CopyFrom(pre_err);
        return grpc::Status::OK;
    }
    return run_request_response_command(
        od_estimate_local_command(&centroid_cfg, request->param2()),
        request,
        response,
        "tool_estimation helper failed");
}

grpc::Status run_tool_od_estimation_local_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    CentroidConnectorEnvConfig cfg;
    return run_tool_od_estimation_local_stream(request, writer, cfg);
}

grpc::Status run_tool_od_estimation_local_stream(
    const func::ParamsData* request,
    grpc::ServerWriter<func::ProgressData>* writer,
    const CentroidConnectorEnvConfig& centroid_cfg)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在执行方案库本地 OD 反推..."));
    func::ResultData pre_err;
    if (!maybe_prebuild_centroid_for_od_local(request, &pre_err, centroid_cfg)) {
        writer->Write(make_progress_frame(pre_err.code(), pre_err.message(), 100, 1, "质心连杆失败", &pre_err));
        return grpc::Status::OK;
    }
    writer->Write(make_progress_frame(0, "Starting", 5, 0, "正在执行 OD 估计..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    run_request_response_command_streaming(
        od_estimate_local_command(&centroid_cfg, request->param2()),
        request,
        &response,
        "tool_estimation helper failed",
        writer,
        progress_file,
        status_file,
        5,
        100,
        "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "OD 估计完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_trip_sync(const std::string& method, const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(trip_python_path()) + " " + shell_quote(trip_bridge_path()) + " --method " + shell_quote(method);
    std::string repo_root = trip_repo_root();
    if (!repo_root.empty()) {
        command += " --repo-root " + shell_quote(repo_root);
    }
    return run_request_response_command(command, request, response, method + " bridge failed");
}

grpc::Status run_trip_stream(const std::string& method, const std::string& start_title, const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, start_title));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(trip_python_path()) + " " + shell_quote(trip_bridge_path()) + " --method " + shell_quote(method);
    std::string repo_root = trip_repo_root();
    if (!repo_root.empty()) {
        command += " --repo-root " + shell_quote(repo_root);
    }
    run_request_response_command_streaming(command, request, &response, method + " bridge failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, start_title + "完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_centroid_connector_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(trip_python_path()) + " " + shell_quote(centroid_connector_bridge_path());
    return run_request_response_command(command, request, response, "build_centroid_connectors bridge failed");
}

grpc::Status run_centroid_connector_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在构建质心连杆..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(trip_python_path()) + " " + shell_quote(centroid_connector_bridge_path());
    run_request_response_command_streaming(
        command, request, &response, "build_centroid_connectors bridge failed",
        writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "质心连杆构建完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_transit_assignment_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(transit_helper_path());
    return run_request_response_command(command, request, response, "transit_assignment helper failed");
}

grpc::Status run_transit_assignment_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在执行公交分配..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(transit_helper_path());
    run_request_response_command_streaming(command, request, &response, "transit_assignment helper failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "公交分配完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_transit_assignment_new_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(transit_assignment_new_helper_path());
    return run_request_response_command(command, request, response, "transit_assignment_new helper failed");
}

grpc::Status run_transit_assignment_new_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在执行公交 AON 分配..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(transit_assignment_new_helper_path());
    run_request_response_command_streaming(
        command, request, &response, "transit_assignment_new helper failed",
        writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "公交 AON 分配完成", &response));
    return grpc::Status::OK;
}


grpc::Status run_diagnosis_sync(const std::string& module, const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(diagnosis_python_path()) + " " + shell_quote(diagnosis_bridge_path())
        + " --module " + shell_quote(module);
    std::string repo_root = diagnosis_repo_root();
    if (!repo_root.empty()) {
        command += " --repo-root " + shell_quote(repo_root);
    }
    return run_request_response_command(command, request, response, "diagnosis_" + module + " bridge failed");
}

grpc::Status run_diagnosis_stream(const std::string& module, const std::string& start_title, const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, start_title));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(diagnosis_python_path()) + " " + shell_quote(diagnosis_bridge_path())
        + " --module " + shell_quote(module);
    std::string repo_root = diagnosis_repo_root();
    if (!repo_root.empty()) {
        command += " --repo-root " + shell_quote(repo_root);
    }
    run_request_response_command_streaming(command, request, &response, "diagnosis_" + module + " bridge failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, start_title + "完成", &response));
    return grpc::Status::OK;
}

std::string base_report_bridge_path()
{
    const char* env_path = std::getenv("TNA_BASE_REPORT_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/base_report_bridge.py";
}

std::string base_report_python_path()
{
    const char* env_path = std::getenv("TNA_BASE_REPORT_PYTHON");
    if (env_path && *env_path) return std::string(env_path);
    return trip_python_path();
}

grpc::Status run_base_report_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(base_report_bridge_path());
    return run_request_response_command(command, request, response, "base_data_report bridge failed");
}

grpc::Status run_base_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在生成基础数据分析报告..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(base_report_bridge_path());
    run_request_response_command_streaming(command, request, &response, "base_data_report bridge failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "基础数据分析报告生成完成", &response));
    return grpc::Status::OK;
}

std::string cost_benefit_bridge_path()
{
    const char* env_path = std::getenv("TNA_COST_BENEFIT_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/cost_benefit_bridge.py";
}

grpc::Status run_cost_benefit_report_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(cost_benefit_bridge_path());
    return run_request_response_command(command, request, response, "cost_benefit_report bridge failed");
}

grpc::Status run_cost_benefit_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在生成成本效益分析报告..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(cost_benefit_bridge_path());
    run_request_response_command_streaming(command, request, &response, "cost_benefit_report bridge failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "成本效益分析报告生成完成", &response));
    return grpc::Status::OK;
}

std::string report_bridge_path()
{
    const char* env_path = std::getenv("TNA_REPORT_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/report_bridge.py";
}

std::string report_repo_root()
{
    const char* env_path = std::getenv("TNA_REPORT_ROOT");
    if (env_path && *env_path) return std::string(env_path);
    return "";
}

std::string report_python_path()
{
    const char* env_path = std::getenv("TNA_REPORT_PYTHON");
    if (env_path && *env_path) return std::string(env_path);
    return trip_python_path();  // 默认与 trip bridge 共用同一个 python 解释器
}

grpc::Status run_report_sync(int task, const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(report_python_path()) + " " + shell_quote(report_bridge_path())
        + " --task " + std::to_string(task);
    std::string repo_root = report_repo_root();
    if (!repo_root.empty()) {
        command += " --report-root " + shell_quote(repo_root);
    }
    return run_request_response_command(command, request, response, "report bridge task=" + std::to_string(task) + " failed");
}

grpc::Status run_report_stream(int task, const std::string& start_title, const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, start_title));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(report_python_path()) + " " + shell_quote(report_bridge_path())
        + " --task " + std::to_string(task);
    std::string repo_root = report_repo_root();
    if (!repo_root.empty()) {
        command += " --report-root " + shell_quote(repo_root);
    }
    run_request_response_command_streaming(command, request, &response, "report bridge task=" + std::to_string(task) + " failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, start_title + "完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_transit_assignment_stream_ranged(const func::ParamsData* request,
                                                   grpc::ServerWriter<func::ProgressData>* writer,
                                                   int start_percent, int end_percent,
                                                   func::ResultData* result_out)
{
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file   = make_temp_file("tna_status_");
    std::string command = shell_quote(transit_helper_path());
    run_request_response_command_streaming(command, request, result_out,
        "transit_assignment helper failed", writer,
        progress_file, status_file, start_percent, end_percent, "Running");
    return grpc::Status::OK;
}

grpc::Status run_diagnosis_stream_ranged(const std::string& module,
                                          const func::ParamsData* request,
                                          grpc::ServerWriter<func::ProgressData>* writer,
                                          int start_percent, int end_percent,
                                          func::ResultData* result_out)
{
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file   = make_temp_file("tna_status_");
    std::string command = shell_quote(diagnosis_python_path()) + " " + shell_quote(diagnosis_bridge_path())
        + " --module " + shell_quote(module);
    std::string repo_root = diagnosis_repo_root();
    if (!repo_root.empty()) {
        command += " --repo-root " + shell_quote(repo_root);
    }
    run_request_response_command_streaming(command, request, result_out,
        "diagnosis_" + module + " bridge failed", writer,
        progress_file, status_file, start_percent, end_percent, "Running");
    return grpc::Status::OK;
}

std::string scheme_compare_bridge_path()
{
    const char* env_path = std::getenv("TNA_SCHEME_COMPARE_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/scheme_compare_bridge.py";
}

grpc::Status run_scheme_compare_report_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(scheme_compare_bridge_path());
    return run_request_response_command(command, request, response, "scheme_compare_report bridge failed");
}

grpc::Status run_scheme_compare_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在生成方案对比分析报告..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file   = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(scheme_compare_bridge_path());
    run_request_response_command_streaming(command, request, &response, "scheme_compare_report bridge failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "方案对比分析报告生成完成", &response));
    return grpc::Status::OK;
}

static bool report_param2_type_is(const std::string& param2, const char* type_name)
{
    if (param2.empty() || !type_name || !*type_name) return false;
    const std::string value = std::string("\"") + type_name + "\"";
    const std::string key1 = "\"type\"";
    const std::string key2 = "\"scheme_type\"";
    auto has_type_value = [&](const std::string& key) {
        std::size_t pos = param2.find(key);
        if (pos == std::string::npos) return false;
        pos = param2.find(':', pos + key.size());
        if (pos == std::string::npos) return false;
        std::size_t next_comma = param2.find(',', pos + 1);
        std::size_t next_brace = param2.find('}', pos + 1);
        std::size_t end = std::min(next_comma == std::string::npos ? param2.size() : next_comma,
                                   next_brace == std::string::npos ? param2.size() : next_brace);
        std::string segment = param2.substr(pos + 1, end - pos - 1);
        return segment.find(value) != std::string::npos;
    };
    return has_type_value(key1) || has_type_value(key2);
}

std::string diagnosis_report_bridge_path(const func::ParamsData* request)
{
    const char* env_path = std::getenv("TNA_DIAGNOSIS_REPORT_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);

    const std::string p2 = request ? request->param2() : std::string();
    if (report_param2_type_is(p2, "slow") || report_param2_type_is(p2, "motor")) {
        const char* env_ms = std::getenv("TNA_DIAGNOSIS_REPORT_MOTOR_SLOW_BRIDGE_PY");
        if (env_ms && *env_ms) return std::string(env_ms);
        return helper_root() + "/diagnosis_report_bridge_motor_slow.py";
    }
    return helper_root() + "/diagnosis_report_bridge.py";
}

grpc::Status run_diagnosis_report_sync(const func::ParamsData* request, func::ResultData* response, const std::string& report_kind)
{
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(diagnosis_report_bridge_path(request))
        + " --report-kind " + shell_quote(report_kind);
    return run_request_response_command(command, request, response, report_kind + " diagnosis report bridge failed");
}

grpc::Status run_diagnosis_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer, const std::string& report_kind, const std::string& start_title)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, start_title));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file   = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(diagnosis_report_bridge_path(request))
        + " --report-kind " + shell_quote(report_kind);
    run_request_response_command_streaming(command, request, &response, report_kind + " diagnosis report bridge failed", writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, start_title + "完成", &response));
    return grpc::Status::OK;
}

grpc::Status run_base_scheme_diagnosis_report_sync(const func::ParamsData* request, func::ResultData* response)
{
    return run_diagnosis_report_sync(request, response, "base");
}

grpc::Status run_base_scheme_diagnosis_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    return run_diagnosis_report_stream(request, writer, "base", "正在生成基础方案诊断指标报告...");
}

grpc::Status run_scheme_diagnosis_report_sync(const func::ParamsData* request, func::ResultData* response)
{
    return run_diagnosis_report_sync(request, response, "scheme");
}

grpc::Status run_scheme_diagnosis_report_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    return run_diagnosis_report_stream(request, writer, "scheme", "正在生成方案诊断指标报告...");
}


std::string od_trace_bridge_path()
{
    const char* env_path = std::getenv("TNA_OD_TRACE_BRIDGE_PY");
    if (env_path && *env_path) return std::string(env_path);
    return helper_root() + "/od_trace_bridge.py";
}

grpc::Status run_od_trace_sync(const func::ParamsData* request, func::ResultData* response)
{
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(od_trace_bridge_path());
    return run_request_response_command(command, request, response, "od_trace bridge failed");
}

grpc::Status run_od_trace_stream(const func::ParamsData* request, grpc::ServerWriter<func::ProgressData>* writer)
{
    writer->Write(make_progress_frame(0, "Starting", 0, 0, "正在执行 OD 溯源..."));
    std::string progress_file = make_temp_file("tna_progress_");
    std::string status_file = make_temp_file("tna_status_");
    func::ResultData response;
    std::string command = shell_quote(base_report_python_path()) + " " + shell_quote(od_trace_bridge_path());
    run_request_response_command_streaming(
        command, request, &response, "od_trace bridge failed",
        writer, progress_file, status_file, 0, 100, "Running");
    writer->Write(make_progress_frame(response.code(), response.message(), 100, 1, "OD 溯源完成", &response));
    return grpc::Status::OK;
}

}  // namespace tna_local_bridge
