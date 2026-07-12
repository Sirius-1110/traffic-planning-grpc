#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include <cstdlib>
#include <unistd.h>
#include <limits.h>
#include <sstream>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "tool_estimation.grpc.pb.h"

#if __has_include("stdafx.h")
#  include "stdafx.h"
#else
#  include "header/stdafx.h"
#endif

static std::string g_db_conn_str;

thread_local grpc::ServerWriter<func::ProgressData>* g_tool_estimation_progress_writer = nullptr;
thread_local int g_tool_estimation_last_percent = -1;
thread_local int g_tool_estimation_last_iter = -1;

static void reset_tool_estimation_progress_bridge()
{
    g_tool_estimation_progress_writer = nullptr;
    g_tool_estimation_last_percent = -1;
    g_tool_estimation_last_iter = -1;
}

static void bind_tool_estimation_progress_bridge(grpc::ServerWriter<func::ProgressData>* writer)
{
    g_tool_estimation_progress_writer = writer;
    g_tool_estimation_last_percent = -1;
    g_tool_estimation_last_iter = -1;
}

static void TNM_CDECL emit_tool_estimation_progress(int iter, double conv)
{
    if (g_tool_estimation_progress_writer == nullptr) {
        return;
    }
    int percent = 11 + iter * 3;
    if (percent < 11) percent = 11;
    if (percent > 95) percent = 95;
    if (percent < g_tool_estimation_last_percent) percent = g_tool_estimation_last_percent;
    if (percent == g_tool_estimation_last_percent && iter == g_tool_estimation_last_iter) {
        return;
    }

    func::ProgressData frame;
    frame.set_code(0);
    frame.set_message("Running");
    frame.mutable_data()->set_percent(percent);
    frame.mutable_data()->set_done(0);

    std::ostringstream title;
    title << "正在迭代 OD 估计";
    if (iter > 0) {
        title << "，第 " << iter << " 次";
    }
    title << "，当前收敛指标 " << conv;
    frame.mutable_data()->set_title(title.str());

    g_tool_estimation_progress_writer->Write(frame);
    g_tool_estimation_last_percent = percent;
    g_tool_estimation_last_iter = iter;
}

static std::string load_db_config()
{
    std::vector<std::string> candidates;

    const char* env_conf = std::getenv("TNA_DB_CONF");
    if (env_conf && *env_conf)
        candidates.push_back(env_conf);

    char exe_path[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        std::string exe_dir(exe_path, len);
        auto slash = exe_dir.rfind('/');
        if (slash != std::string::npos)
            exe_dir = exe_dir.substr(0, slash);

        auto parent_slash = exe_dir.rfind('/');
        if (parent_slash != std::string::npos)
            candidates.push_back(exe_dir.substr(0, parent_slash) + "/db.conf");

        candidates.push_back(exe_dir + "/db.conf");
    }

    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::string conn, line;
        while (std::getline(f, line)) {
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

static std::string resolve_db_schema()
{
    const char* schema_env = std::getenv("TNA_DB_SCHEMA");
    return (schema_env && *schema_env) ? std::string(schema_env) : std::string("user_project");
}

static std::string build_table_prefix(int32_t project_id, int32_t user_id, int32_t case_id)
{
    std::string prefix = "project" + std::to_string(project_id) + "_user" + std::to_string(user_id) + "_";
    if (case_id > 0) {
        prefix += "case" + std::to_string(case_id) + "_";
    }
    return prefix;
}

static void derive_table_names(
    int32_t project_id, int32_t user_id, int32_t case_id,
    std::string& networkTable, std::string& odTable,
    std::string& observedTable, std::string& scenarioPrefix)
{
    std::string prefix = build_table_prefix(project_id, user_id, case_id);
    scenarioPrefix = prefix;

    std::string schema = resolve_db_schema();
    networkTable  = schema + "." + prefix + "road_way";
    odTable       = schema + "." + prefix + "other_od";
    observedTable = schema + "." + prefix + "other_observation";
}

static std::string build_data_json(
    int iter, const std::string& networkTable,
    const std::string& odTable, const std::string& observedTable,
    double rmse, double rgp)
{
    std::ostringstream oss;
    oss << "{";
    oss << "\"summary\":{\"iter\":" << iter
        << ",\"rmse\":" << rmse
        << ",\"rgp\":" << rgp << "}";
    oss << ",\"params\":{"
        << "\"network_table\":\"" << networkTable << "\""
        << ",\"od_table\":\"" << odTable << "\""
        << ",\"observed_table\":\"" << observedTable << "\""
        << "}";
    oss << "}";
    return oss.str();
}

class FuncServiceImpl final : public func::FuncService::Service {
public:
    grpc::Status tool_estimation(grpc::ServerContext* context,
                                 const func::ParamsData* request,
                                 func::ResultData* response) override
    {
        const std::string& dbConnStr = g_db_conn_str;
        if (dbConnStr.empty()) {
            response->set_code(-1);
            response->set_message("Server db config not loaded. Check db.conf.");
            return grpc::Status::OK;
        }
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            response->set_code(-1);
            response->set_message("project_id and user_id are required positive integers. case_id is optional.");
            return grpc::Status::OK;
        }

        std::string networkTable, odTable, observedTable, scenarioPrefix;
        std::string schema = resolve_db_schema();
        derive_table_names(request->project_id(), request->user_id(), request->case_id(),
                           networkTable, odTable, observedTable, scenarioPrefix);

        std::cout << "[gRPC] tool_estimation: project=" << request->project_id()
                  << " user=" << request->user_id()
                  << " case=" << request->case_id()
                  << " schema=" << schema
                  << " net=" << networkTable << " od=" << odTable << std::endl;

        try {
            OD_ESTIMATION odes;
            odes.SetLPF(BPRLK);
            odes.SetRoleName(schema);
            odes.SetResultTablePrefix(scenarioPrefix);
            odes.SetDbConnStr(dbConnStr.c_str());
            odes.SetNetworkTableName(networkTable);
            odes.SetODTableName(odTable);

            int buildRet = odes.Build("grpc_od_estimation", "", NETPOSTGRESQL);
            if (buildRet != 0) {
                std::string msg = "Build failed with code " + std::to_string(buildRet);
                response->set_code(-2);
                response->set_message(msg);
                return grpc::Status::OK;
            }

            odes.overall_process();
            int reportRet = odes.ReportPG();
            if (reportRet != 0) {
                response->set_code(-3);
                response->set_message("ReportPG failed with code " + std::to_string(reportRet));
                return grpc::Status::OK;
            }

            std::string dataJson = build_data_json(
                odes.l, networkTable, odTable, observedTable, odes.RMSE, odes.RGP);

            response->set_code(1);
            response->set_message("Success");
            response->set_data(dataJson);

        } catch (const std::exception& e) {
            response->set_code(-99);
            response->set_message(std::string("Exception: ") + e.what());
        } catch (...) {
            response->set_code(-99);
            response->set_message("Unknown exception");
        }

        return grpc::Status::OK;
    }

    grpc::Status tool_estimation_stream(grpc::ServerContext* context,
                                        const func::ParamsData* request,
                                        grpc::ServerWriter<func::ProgressData>* writer) override
    {
        const std::string& dbConnStr = g_db_conn_str;
        if (dbConnStr.empty()) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("Server db config not loaded. Check db.conf.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }
        if (request->project_id() <= 0 || request->user_id() <= 0) {
            func::ProgressData frame;
            frame.set_code(-1);
            frame.set_message("project_id and user_id are required positive integers. case_id is optional.");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
            return grpc::Status::OK;
        }

        std::string networkTable, odTable, observedTable, scenarioPrefix;
        std::string schema = resolve_db_schema();
        derive_table_names(request->project_id(), request->user_id(), request->case_id(),
                           networkTable, odTable, observedTable, scenarioPrefix);

        {
            func::ProgressData frame;
            frame.set_code(0);
            frame.set_message("Starting");
            frame.mutable_data()->set_percent(0);
            frame.mutable_data()->set_done(0);
            frame.mutable_data()->set_title("正在加载路网与OD数据...");
            writer->Write(frame);
        }

        try {
            OD_ESTIMATION odes;
            odes.SetLPF(BPRLK);
            odes.SetRoleName(schema);
            odes.SetResultTablePrefix(scenarioPrefix);
            odes.SetDbConnStr(dbConnStr.c_str());
            odes.SetNetworkTableName(networkTable);
            odes.SetODTableName(odTable);

            int buildRet = odes.Build("grpc_od_estimation", "", NETPOSTGRESQL);
            if (buildRet != 0) {
                func::ProgressData frame;
                std::string msg = "Build failed with code " + std::to_string(buildRet);
                frame.set_code(-2);
                frame.set_message(msg);
                frame.mutable_data()->set_done(1);
                writer->Write(frame);
                return grpc::Status::OK;
            }

            {
                func::ProgressData frame;
                frame.set_code(0);
                frame.set_message("Running");
                frame.mutable_data()->set_percent(10);
                frame.mutable_data()->set_done(0);
                frame.mutable_data()->set_title("数据加载完成，正在执行OD估计...");
                writer->Write(frame);
            }

            bind_tool_estimation_progress_bridge(writer);
            odes.SetProgressCallback(emit_tool_estimation_progress);
            odes.overall_process();
            odes.SetProgressCallback(nullptr);
            reset_tool_estimation_progress_bridge();

            {
                func::ProgressData frame;
                frame.set_code(0);
                frame.set_message("Writing");
                frame.mutable_data()->set_percent(96);
                frame.mutable_data()->set_done(0);
                frame.mutable_data()->set_title("OD 估计完成，正在写入结果...");
                writer->Write(frame);
            }

            int reportRet = odes.ReportPG();
            if (reportRet != 0) {
                func::ProgressData frame;
                frame.set_code(-3);
                frame.set_message("ReportPG failed with code " + std::to_string(reportRet));
                frame.mutable_data()->set_done(1);
                writer->Write(frame);
                return grpc::Status::OK;
            }

            std::string dataJson = build_data_json(
                odes.l, networkTable, odTable, observedTable, odes.RMSE, odes.RGP);

            func::ProgressData frame;
            frame.set_code(1);
            frame.set_message("Success");
            frame.mutable_data()->set_percent(100);
            frame.mutable_data()->set_done(1);
            frame.mutable_data()->set_title("OD估计完成，共迭代 " + std::to_string(odes.l) + " 次");
            writer->Write(frame);

        } catch (const std::exception& e) {
            reset_tool_estimation_progress_bridge();
            func::ProgressData frame;
            frame.set_code(-99);
            frame.set_message(std::string("Exception: ") + e.what());
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
        } catch (...) {
            reset_tool_estimation_progress_bridge();
            func::ProgressData frame;
            frame.set_code(-99);
            frame.set_message("Unknown exception");
            frame.mutable_data()->set_done(1);
            writer->Write(frame);
        }

        return grpc::Status::OK;
    }
};

static FuncServiceImpl service;
static std::unique_ptr<grpc::Server> g_server;

int main(int argc, char** argv)
{
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

    std::cout << "[gRPC] TNA FuncService (tool_estimation) listening on " << server_address << std::endl;
    g_server->Wait();
    return 0;
}
