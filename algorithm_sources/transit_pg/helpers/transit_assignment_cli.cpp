// transit_assignment_cli.cpp
// Linux helper for the unified gRPC service.
// 输入：--request-file <kv> --response-file <kv> [--progress-file <file>]
// 输出：统一 tables/counts/metrics/summary 结构，与 tool_estimation_cli 保持一致。

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "PTNet.h"

namespace {

struct Args {
    std::string requestFile;
    std::string responseFile;
    std::string progressFile;
};

std::string escape_value(const std::string& v)
{
    std::string o;
    o.reserve(v.size());
    for (char ch : v) {
        if (ch == '\\') o += "\\\\";
        else if (ch == '\n') o += "\\n";
        else if (ch == '\r') o += "\\r";
        else o += ch;
    }
    return o;
}

std::string unescape_value(const std::string& v)
{
    std::string o;
    o.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[i + 1];
            if (n == 'n') o += '\n';
            else if (n == 'r') o += '\r';
            else o += n;
            ++i;
        } else {
            o += v[i];
        }
    }
    return o;
}

bool parse_args(int argc, char** argv, Args& out)
{
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (i + 1 >= argc) return false;
        std::string v = argv[++i];
        if (k == "--request-file")      out.requestFile = v;
        else if (k == "--response-file") out.responseFile = v;
        else if (k == "--progress-file") out.progressFile = v;
    }
    return !out.requestFile.empty() && !out.responseFile.empty();
}

std::map<std::string, std::string> read_kv(const std::string& path)
{
    std::map<std::string, std::string> m;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        m[line.substr(0, pos)] = unescape_value(line.substr(pos + 1));
    }
    return m;
}

std::string get_or(const std::map<std::string, std::string>& m, const std::string& k, const std::string& d = "")
{
    auto it = m.find(k);
    return it == m.end() ? d : it->second;
}

int to_int(const std::string& s, int d = 0)
{
    try { return std::stoi(s); } catch (...) { return d; }
}

std::string resolve_schema()
{
    const char* s = std::getenv("TNA_DB_SCHEMA");
    return (s && *s) ? std::string(s) : std::string("user_project");
}

std::string load_db_conn_from_conf()
{
    const char* confPath = std::getenv("TNA_DB_CONF");
    if (!(confPath && *confPath)) return "";

    std::ifstream in(confPath);
    if (!in.is_open()) return "";

    std::string conn;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!conn.empty()) conn += ' ';
        conn += line;
    }
    return conn;
}

std::string build_prefix(int pid, int uid, int cid)
{
    return "project" + std::to_string(pid) + "_user" + std::to_string(uid)
           + "_case" + std::to_string(cid) + "_";
}

void append_progress(const std::string& path, int percent, const std::string& title)
{
    if (path.empty()) return;
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;
    out << "percent=" << percent << "\t"
        << "title=" << escape_value(title) << "\n";
}

std::string trim_copy(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string join_nonempty_lines(const std::string& lhs, const std::string& rhs)
{
    if (lhs.empty()) return rhs;
    if (rhs.empty()) return lhs;
    if (lhs == rhs) return lhs;
    return lhs + "\n" + rhs;
}

std::string combine_phase_output(const std::string& stdoutText, const std::string& stderrText)
{
    std::string merged = join_nonempty_lines(trim_copy(stdoutText), trim_copy(stderrText));
    const size_t maxLength = 4000;
    if (merged.size() > maxLength) {
        merged = merged.substr(merged.size() - maxLength);
    }
    return merged;
}

std::string unquote_identifier(const std::string& value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool table_exists(const std::string& conninfo,
                  const std::string& defaultSchema,
                  const std::string& tableName,
                  bool& exists,
                  std::string& errorMessage)
{
    exists = false;
    std::string qualifiedName;
    if (!TNM_ResolveTableName(tableName, defaultSchema, qualifiedName, errorMessage)) {
        return false;
    }

    std::string schema = defaultSchema;
    std::string table = qualifiedName;
    size_t dot = qualifiedName.find('.');
    if (dot != std::string::npos) {
        schema = unquote_identifier(qualifiedName.substr(0, dot));
        table = unquote_identifier(qualifiedName.substr(dot + 1));
    } else {
        table = unquote_identifier(table);
    }

    TNM_PGDB db;
    if (!db.Open(conninfo, errorMessage)) {
        return false;
    }

    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT 1 FROM information_schema.tables WHERE table_schema='" + schema
        + "' AND table_name='" + table + "' LIMIT 1";
    bool ok = db.Query(sql, rows, errorMessage);
    db.Close();
    if (!ok) {
        return false;
    }
    exists = !rows.empty();
    return true;
}

bool validate_required_tables(const std::string& conninfo,
                              const std::string& defaultSchema,
                              const std::vector<std::string>& tableNames,
                              std::string& errorMessage)
{
    std::vector<std::string> missing;
    for (const auto& tableName : tableNames) {
        bool exists = false;
        std::string probeError;
        if (!table_exists(conninfo, defaultSchema, tableName, exists, probeError)) {
            errorMessage = "Failed to validate input table " + tableName + ": " + probeError;
            return false;
        }
        if (!exists) {
            missing.push_back(tableName);
        }
    }

    if (missing.empty()) {
        return true;
    }

    errorMessage = "Required input tables do not exist for project-style naming: ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) errorMessage += ", ";
        errorMessage += missing[i];
    }
    return false;
}

class ScopedStreamCapture {
public:
    explicit ScopedStreamCapture(std::ostream& stream)
        : stream_(stream), oldBuf_(stream.rdbuf(buffer_.rdbuf())) {}

    ~ScopedStreamCapture()
    {
        stream_.rdbuf(oldBuf_);
    }

    std::string take()
    {
        std::string text = buffer_.str();
        buffer_.str(std::string());
        buffer_.clear();
        return text;
    }

private:
    std::ostream& stream_;
    std::streambuf* oldBuf_;
    std::ostringstream buffer_;
};

bool table_has_rows(const std::string& conninfo, const std::string& defaultSchema, const std::string& tableName, bool& hasRows, std::string& errorMessage)
{
    hasRows = false;
    std::string qualifiedName;
    if (!TNM_ResolveTableName(tableName, defaultSchema, qualifiedName, errorMessage)) {
        return false;
    }

    TNM_PGDB db;
    if (!db.Open(conninfo, errorMessage)) {
        return false;
    }

    std::vector<std::vector<std::string>> rows;
    bool ok = db.Query("SELECT COUNT(*) FROM " + qualifiedName, rows, errorMessage);
    db.Close();
    if (!ok) {
        return false;
    }
    if (rows.empty() || rows[0].empty()) {
        errorMessage = "Failed to read walk table row count.";
        return false;
    }

    try {
        hasRows = std::stoll(rows[0][0]) > 0;
    } catch (...) {
        errorMessage = "Invalid walk table row count: " + rows[0][0];
        return false;
    }
    return true;
}

struct Response {
    int code = -99;
    std::string message;
    std::map<std::string, std::string> tables;   // data.table.*
    std::map<std::string, long long>   counts;   // data.count.*
    std::map<std::string, double>      metrics;  // data.metric.*
    std::string stage;
    std::vector<std::string> logs;
    std::map<std::string, std::string> attrs;
};

void write_kv(const std::string& path, const Response& r)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    out << "code=" << r.code << "\n";
    out << "message=" << escape_value(r.message) << "\n";
    for (const auto& kv : r.tables)  out << "data.table."  << kv.first << "=" << escape_value(kv.second) << "\n";
    for (const auto& kv : r.counts)  out << "data.count."  << kv.first << "=" << kv.second << "\n";
    for (const auto& kv : r.metrics) out << "data.metric." << kv.first << "=" << kv.second << "\n";
    out << "summary.stage=" << escape_value(r.stage) << "\n";
    for (size_t i = 0; i < r.logs.size(); ++i)
        out << "summary.log." << (i + 1) << "=" << escape_value(r.logs[i]) << "\n";
    for (const auto& kv : r.attrs)
        out << "summary.attr." << kv.first << "=" << escape_value(kv.second) << "\n";
}

void set_phase_failure(Response& resp, const std::string& phase, const std::string& prefix, int code, const std::string& detail)
{
    resp.code = -2;
    resp.stage = "transit_assignment_" + phase;
    resp.attrs["failure_stage"] = phase;
    resp.attrs["failure_code"] = std::to_string(code);
    resp.message = prefix + " with code " + std::to_string(code);
    if (!detail.empty()) {
        resp.message += ": " + detail;
        resp.logs.push_back(phase + " detail: " + detail);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::cerr << "usage: transit_assignment_cli --request-file <f> --response-file <f> [--progress-file <f>]" << std::endl;
        return 2;
    }

    Response resp;
    resp.stage = "transit_assignment";

    auto req = read_kv(args.requestFile);
    int  projectId = to_int(get_or(req, "project_id"));
    int  userId    = to_int(get_or(req, "user_id"));
    int  caseId    = to_int(get_or(req, "case_id"));
    std::string dbConn = get_or(req, "param1");
    if (dbConn.empty()) {
        const char* envConn = std::getenv("TNA_DB_CONN_STR");
        if (envConn && *envConn) dbConn = envConn;
    }
    if (dbConn.empty()) {
        dbConn = load_db_conn_from_conf();
    }

    if (projectId <= 0 || userId <= 0) {
        resp.code = -1;
        resp.message = "project_id and user_id are required positive integers. case_id is optional.";
        write_kv(args.responseFile, resp);
        return 0;
    }
    if (dbConn.empty()) {
        resp.code = -1;
        resp.message = "db connection string is empty (param1 or TNA_DB_CONN_STR).";
        write_kv(args.responseFile, resp);
        return 0;
    }

    std::string schema = resolve_schema();
    std::string prefix = build_prefix(projectId, userId, caseId);

    auto full = [&](const std::string& suffix) {
        return schema + "." + prefix + suffix;
    };
    std::string stopTable    = full("pt_stop");
    std::string routeTable   = full("pt_route");
    std::string shapeTable   = full("pt_shape");
    std::string transitTable = full("pt_transit");
    std::string walkTable    = full("pt_walk");
    std::string tripTable    = full("pt_trip");
    std::string linkResult    = full("pt_link_result");
    std::string pathResult    = full("pt_path_result");
    std::string iterResult    = full("pt_iter_result");
    std::string summaryResult = full("pt_summary_result");

    resp.tables["stop_table"]           = stopTable;
    resp.tables["route_table"]          = routeTable;
    resp.tables["shape_table"]          = shapeTable;
    resp.tables["transit_table"]        = transitTable;
    resp.tables["walk_table"]           = walkTable;
    resp.tables["trip_table"]           = tripTable;
    resp.tables["link_result_table"]    = linkResult;
    resp.tables["path_result_table"]    = pathResult;
    resp.tables["iter_result_table"]    = iterResult;
    resp.tables["summary_result_table"] = summaryResult;
    resp.attrs["schema"] = schema;
    resp.attrs["prefix"] = prefix;

    std::string requiredTableError;
    if (!validate_required_tables(dbConn, schema, {stopTable, routeTable, shapeTable, transitTable, tripTable}, requiredTableError)) {
        resp.code = -2;
        resp.message = requiredTableError;
        write_kv(args.responseFile, resp);
        return 0;
    }

    bool useWalkTable = true;
    bool walkTableHasRows = false;
    std::string walkTableError;
    if (!table_has_rows(dbConn, schema, walkTable, walkTableHasRows, walkTableError)) {
        useWalkTable = false;
        resp.logs.push_back("walk table probe failed, fallback to generated walks: " + walkTableError);
    } else if (!walkTableHasRows) {
        useWalkTable = false;
        resp.logs.push_back("walk table is empty, fallback to generated walks");
    }
    resp.attrs["walk_mode"] = useWalkTable ? "table" : "generated";

    std::string spec = "db:conninfo=" + dbConn + ";";
    spec += "schema=" + schema + ";";
    spec += "stop_table="          + stopTable    + ";";
    spec += "route_table="         + routeTable   + ";";
    spec += "shape_table="         + shapeTable   + ";";
    spec += "transit_table="       + transitTable + ";";
    if (useWalkTable) spec += "walk_table=" + walkTable + ";";
    spec += "trip_table="          + tripTable    + ";";
    spec += "link_result_table="   + linkResult    + ";";
    spec += "path_result_table="   + pathResult    + ";";
    spec += "iter_result_table="   + iterResult    + ";";
    spec += "summary_table="       + summaryResult + ";";

    append_progress(args.progressFile, 5, "正在加载公交网络...");

    PTNET* net = nullptr;
    try {
        ScopedStreamCapture coutCapture(std::cout);
        ScopedStreamCapture cerrCapture(std::cerr);
        net = new PTNET(spec);
        net->Settimescaler(60.0);
        net->SetAlightLoss(0.1);
        net->SetCapcityPerLine(30);
        net->SetSymLinkType(false);

        int buildRet = net->BuildAN(false, useWalkTable);
        std::string buildDetail = combine_phase_output(coutCapture.take(), cerrCapture.take());
        if (buildRet != 0) {
            set_phase_failure(resp, "build", "Network build failed", buildRet, buildDetail);
            delete net;
            write_kv(args.responseFile, resp);
            return 0;
        }
        append_progress(args.progressFile, 35, "路网加载完成，开始迭代求解...");

        net->SetConv(1e-8);
        net->SetMaxIter(500);
        net->SetMaxIterTime(60);
        net->SetInnerConv(1e-10);
        int solveRet = net->PCTAE_Solver(PTNET::PCTAE_algorithm::PCTAE_P_iNGP);
        std::string solveDetail = combine_phase_output(coutCapture.take(), cerrCapture.take());
        if (solveRet != 0) {
            set_phase_failure(resp, "solve", "Transit assignment solve failed", solveRet, solveDetail);
            delete net;
            write_kv(args.responseFile, resp);
            return 0;
        }
        append_progress(args.progressFile, 80, "求解完成，正在写入结果...");

        int writeRet = net->WritePG();
        std::string writeDetail = combine_phase_output(coutCapture.take(), cerrCapture.take());
        if (writeRet != 0) {
            set_phase_failure(resp, "write", "Writing results to PostgreSQL failed", writeRet, writeDetail);
            delete net;
            write_kv(args.responseFile, resp);
            return 0;
        }

        resp.counts["num_of_nodes"]        = net->numOfNode;
        resp.counts["num_of_links"]        = net->numOfLink;
        resp.counts["num_of_od_pairs"]     = net->numOfPTOD;
        resp.counts["num_of_destinations"] = net->numOfPTDest;
        resp.counts["num_of_trips"]        = static_cast<long long>(net->numOfPTTrips);
        resp.counts["solve_result"]        = solveRet;

        append_progress(args.progressFile, 100, "公交分配完成");

        resp.code = 1;
        resp.message = "Success";
        delete net;
    } catch (const std::exception& e) {
        if (net) { delete net; net = nullptr; }
        resp.code = -99;
        resp.message = std::string("Exception: ") + e.what();
        resp.logs.push_back(resp.message);
    } catch (...) {
        if (net) { delete net; net = nullptr; }
        resp.code = -99;
        resp.message = "Unknown exception";
        resp.logs.push_back(resp.message);
    }

    write_kv(args.responseFile, resp);
    return 0;
}
