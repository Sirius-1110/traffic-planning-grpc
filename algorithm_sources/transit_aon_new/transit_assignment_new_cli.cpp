#include "PTNet.h"

#include <libpq-fe.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {

struct Args {
    std::string requestFile;
    std::string responseFile;
    std::string progressFile;
};

struct Response {
    int code = -99;
    std::string message;
    std::map<std::string, std::string> tables;
    std::map<std::string, long long> counts;
    std::map<std::string, double> metrics;
    std::string stage = "transit_assignment_new";
    std::vector<std::string> logs;
    std::map<std::string, std::string> attributes;
};

struct ColumnRequirement {
    std::string name;
    std::string category;
};

struct TableRequirement {
    std::string suffix;
    std::vector<ColumnRequirement> columns;
    bool required = true;
    bool requireRows = true;
};

struct PQConninfoOptionDeleter {
    void operator()(PQconninfoOption* options) const {
        if (options) {
            PQconninfoFree(options);
        }
    }
};

struct PGConnectionDeleter {
    void operator()(PGconn* connection) const {
        if (connection) {
            PQfinish(connection);
        }
    }
};

struct PGResultDeleter {
    void operator()(PGresult* result) const {
        if (result) {
            PQclear(result);
        }
    }
};

using PQConninfoOptionPtr =
    std::unique_ptr<PQconninfoOption, PQConninfoOptionDeleter>;
using PGConnectionPtr =
    std::unique_ptr<PGconn, PGConnectionDeleter>;
using PGResultPtr =
    std::unique_ptr<PGresult, PGResultDeleter>;

long long count_hyperpaths(const PTNET& network) {
    long long count = 0;
    for (PTDestination* destination : network.PTDestVector) {
        if (!destination) {
            continue;
        }
        for (int originIndex = 0; originIndex < destination->numOfOrg; ++originIndex) {
            PTOrg* origin = destination->orgVector[originIndex];
            if (!origin || !origin->state || origin->pathSet.empty()) {
                continue;
            }
            count += static_cast<long long>(origin->pathSet.size());
        }
    }
    return count;
}

std::string escape_value(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::string unescape_value(const std::string& value) {
    std::string unescaped;
    unescaped.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 1 < value.size()) {
            const char next = value[++index];
            if (next == 'n') {
                unescaped.push_back('\n');
            } else if (next == 'r') {
                unescaped.push_back('\r');
            } else {
                unescaped.push_back(next);
            }
        } else {
            unescaped.push_back(value[index]);
        }
    }
    return unescaped;
}

bool parse_args(int argc, char** argv, Args& args) {
    for (int index = 1; index < argc; ++index) {
        if (index + 1 >= argc) {
            return false;
        }
        const std::string key = argv[index];
        const std::string value = argv[++index];
        if (key == "--request-file") {
            args.requestFile = value;
        } else if (key == "--response-file") {
            args.responseFile = value;
        } else if (key == "--progress-file") {
            args.progressFile = value;
        } else {
            return false;
        }
    }
    return !args.requestFile.empty() && !args.responseFile.empty();
}

std::map<std::string, std::string> read_kv(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("无法打开请求文件：" + path);
    }

    std::map<std::string, std::string> values;
    std::string line;
    bool firstLine = true;
    while (std::getline(input, line)) {
        if (firstLine &&
            line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        firstLine = false;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[line.substr(0, separator)] =
            unescape_value(line.substr(separator + 1));
    }
    return values;
}

std::string get_or(
    const std::map<std::string, std::string>& values,
    const std::string& key,
    const std::string& defaultValue = "") {
    const auto found = values.find(key);
    return found == values.end() ? defaultValue : found->second;
}

int parse_int(const std::string& value, int defaultValue) {
    try {
        std::size_t parsed = 0;
        const int result = std::stoi(value, &parsed);
        return parsed == value.size() ? result : defaultValue;
    } catch (...) {
        return defaultValue;
    }
}

void write_kv(const std::string& path, const Response& response) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("无法打开响应文件：" + path);
    }

    output << "code=" << response.code << '\n';
    output << "message=" << escape_value(response.message) << '\n';
    for (const auto& item : response.tables) {
        output << "data.table." << item.first << '='
               << escape_value(item.second) << '\n';
    }
    for (const auto& item : response.counts) {
        output << "data.count." << item.first << '='
               << item.second << '\n';
    }
    for (const auto& item : response.metrics) {
        output << "data.metric." << item.first << '='
               << item.second << '\n';
    }
    output << "summary.stage=" << response.stage << '\n';
    for (std::size_t index = 0; index < response.logs.size(); ++index) {
        output << "summary.log." << index + 1 << '='
               << escape_value(response.logs[index]) << '\n';
    }
    for (const auto& item : response.attributes) {
        output << "summary.attr." << item.first << '='
               << escape_value(item.second) << '\n';
    }
}

void append_progress(
    const std::string& path,
    int percent,
    const std::string& title) {
    if (path.empty()) {
        return;
    }
    std::ofstream output(path, std::ios::app);
    if (output.is_open()) {
        output << "percent=" << percent
               << "\ttitle=" << escape_value(title) << '\n';
    }
}

std::string load_connection_string(std::string& schemaFromConfig) {
    const char* envConnection = std::getenv("TNA_DB_CONN_STR");
    if (envConnection && *envConnection) {
        return envConnection;
    }

    const char* configPath = std::getenv("TNA_DB_CONF");
    if (!(configPath && *configPath)) {
        return "";
    }

    std::ifstream input(configPath);
    if (!input.is_open()) {
        return "";
    }

    std::string connectionString;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("schema=", 0) == 0) {
            schemaFromConfig = line.substr(7);
            continue;
        }
        if (!connectionString.empty()) {
            connectionString.push_back(' ');
        }
        connectionString += line;
    }
    return connectionString;
}

PTPostgresConfig parse_postgres_config(
    const std::string& connectionString,
    const std::string& schema) {
    char* rawError = nullptr;
    PQConninfoOptionPtr options(
        PQconninfoParse(connectionString.c_str(), &rawError));
    if (!options) {
        const std::string error =
            rawError ? rawError : "未知连接串错误";
        if (rawError) {
            PQfreemem(rawError);
        }
        throw std::runtime_error("数据库连接参数格式错误：" + error);
    }
    if (rawError) {
        PQfreemem(rawError);
    }

    std::map<std::string, std::string> parsed;
    for (PQconninfoOption* option = options.get();
         option && option->keyword;
         ++option) {
        if (option->val) {
            parsed[option->keyword] = option->val;
        }
    }

    PTPostgresConfig config;
    config.host = parsed["host"].empty() ? "localhost" : parsed["host"];
    config.port = parsed["port"].empty() ? "5432" : parsed["port"];
    config.database = parsed["dbname"];
    config.user = parsed["user"];
    config.password = parsed["password"];
    config.schema = schema;
    config.connectTimeoutSeconds =
        parse_int(parsed["connect_timeout"], 10);
    return config;
}

std::string resolve_schema(const std::string& schemaFromConfig) {
    const char* schema = std::getenv("TNA_DB_SCHEMA");
    if (schema && *schema) {
        return schema;
    }
    return schemaFromConfig.empty()
        ? "user_project"
        : schemaFromConfig;
}

std::string build_prefix(int projectId, int userId, int caseId) {
    return
        "project" + std::to_string(projectId) +
        "_user" + std::to_string(userId) +
        "_case" + std::to_string(caseId) + "_";
}

bool is_safe_identifier(const std::string& value) {
    if (value.empty() ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) ||
          value.front() == '_')) {
        return false;
    }
    for (char ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
            return false;
        }
    }
    return true;
}

std::string quote_identifier(const std::string& value) {
    if (!is_safe_identifier(value)) {
        throw std::runtime_error("数据库标识符不合法：" + value);
    }
    return "\"" + value + "\"";
}

PGResultPtr query_params(
    PGconn* connection,
    const std::string& sql,
    const std::vector<std::string>& parameters) {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const std::string& parameter : parameters) {
        values.push_back(parameter.c_str());
    }

    PGResultPtr result(PQexecParams(
        connection,
        sql.c_str(),
        static_cast<int>(values.size()),
        nullptr,
        values.data(),
        nullptr,
        nullptr,
        0));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        throw std::runtime_error(
            "数据库查询失败：" +
            std::string(PQerrorMessage(connection)));
    }
    return result;
}

bool type_matches(
    const std::string& dataType,
    const std::string& category) {
    if (category == "text") {
        return dataType == "text" ||
               dataType == "character varying" ||
               dataType == "character";
    }
    if (category == "integer") {
        return dataType == "smallint" ||
               dataType == "integer" ||
               dataType == "bigint";
    }
    if (category == "numeric") {
        return type_matches(dataType, "integer") ||
               dataType == "real" ||
               dataType == "double precision" ||
               dataType == "numeric" ||
               dataType == "decimal";
    }
    return false;
}

std::string expected_type_name(const std::string& category) {
    if (category == "text") {
        return "文本类型";
    }
    if (category == "integer") {
        return "整数类型";
    }
    return "数值类型";
}

bool query_has_rows(
    PGconn* connection,
    const std::string& sql) {
    PGResultPtr result(PQexec(connection, sql.c_str()));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        throw std::runtime_error(
            "数据库校验查询失败：" +
            std::string(PQerrorMessage(connection)));
    }
    return PQntuples(result.get()) > 0;
}

bool column_exists(
    PGconn* connection,
    const std::string& schema,
    const std::string& table,
    const std::string& column) {
    PGResultPtr result = query_params(
        connection,
        "SELECT 1 FROM information_schema.columns "
        "WHERE table_schema=$1 AND table_name=$2 AND column_name=$3 "
        "LIMIT 1",
        {schema, table, column});
    return PQntuples(result.get()) > 0;
}

void require_no_rows(
    PGconn* connection,
    const std::string& sql,
    const std::string& errorMessage) {
    if (query_has_rows(connection, sql)) {
        throw std::runtime_error(errorMessage);
    }
}

void validate_database_inputs(
    const std::string& connectionString,
    const std::string& schema,
    const std::string& prefix) {
    PGConnectionPtr connection(PQconnectdb(connectionString.c_str()));
    if (!connection ||
        PQstatus(connection.get()) != CONNECTION_OK) {
        throw std::runtime_error(
            "数据库连接失败：" +
            std::string(
                connection
                    ? PQerrorMessage(connection.get())
                    : "无法创建连接对象"));
    }

    const std::vector<TableRequirement> requirements = {
        {"pt_route", {{"route_id", "text"}}},
        {"pt_stop", {{"stop_id", "text"}, {"stop_name", "text"}}},
        {
            "pt_shape",
            {
                {"route_id", "text"},
                {"frequency", "numeric"},
                {"stop_sequence", "integer"},
                {"stop_id", "text"}
            }
        },
        {
            "pt_transit",
            {
                {"shape_id", "text"},
                {"from_stop", "text"},
                {"to_stop", "text"},
                {"fft", "numeric"}
            }
        },
        {
            "pt_walk",
            {
                {"from_stop", "text"},
                {"to_stop", "text"},
                {"fft", "numeric"},
                {"length", "numeric"}
            },
            false,
            false
        },
        {
            "pt_trip",
            {
                {"origin_stop", "text"},
                {"destination_stop", "text"},
                {"demand", "numeric"}
            }
        }
    };

    bool hasWalkTable = false;
    for (const TableRequirement& requirement : requirements) {
        const std::string tableName = prefix + requirement.suffix;
        PGResultPtr columns = query_params(
            connection.get(),
            "SELECT column_name, data_type "
            "FROM information_schema.columns "
            "WHERE table_schema=$1 AND table_name=$2",
            {schema, tableName});

        if (PQntuples(columns.get()) == 0) {
            if (!requirement.required) {
                continue;
            }
            throw std::runtime_error(
                "缺少输入数据表：" + schema + "." + tableName);
        }
        if (requirement.suffix == "pt_walk") {
            hasWalkTable = true;
        }

        std::map<std::string, std::string> actualColumns;
        for (int row = 0; row < PQntuples(columns.get()); ++row) {
            actualColumns[PQgetvalue(columns.get(), row, 0)] =
                PQgetvalue(columns.get(), row, 1);
        }
        for (const ColumnRequirement& column : requirement.columns) {
            const auto found = actualColumns.find(column.name);
            if (found == actualColumns.end()) {
                throw std::runtime_error(
                    "数据表 " + schema + "." + tableName +
                    " 缺少字段：" + column.name);
            }
            if (!type_matches(found->second, column.category)) {
                throw std::runtime_error(
                    "数据表 " + schema + "." + tableName +
                    " 的字段 " + column.name +
                    " 类型错误，实际为 " + found->second +
                    "，要求为" +
                    expected_type_name(column.category));
            }
        }
        if (requirement.suffix == "pt_shape" &&
            actualColumns.find("shape_id") == actualColumns.end() &&
            actualColumns.find("dir") == actualColumns.end()) {
            throw std::runtime_error(
                "pt_shape 缺少 shape_id 字段；如果使用当前成都数据结构，"
                "必须提供 dir 字段用于派生 shape_id");
        }

        const std::string qualified =
            quote_identifier(schema) + "." +
            quote_identifier(tableName);
        PGResultPtr firstRow(PQexec(
            connection.get(),
            ("SELECT 1 FROM " + qualified + " LIMIT 1").c_str()));
        if (!firstRow ||
            PQresultStatus(firstRow.get()) != PGRES_TUPLES_OK) {
            throw std::runtime_error(
                "无法读取输入数据表 " + schema + "." + tableName +
                "：" + PQerrorMessage(connection.get()));
        }
        if (requirement.requireRows && PQntuples(firstRow.get()) == 0) {
            throw std::runtime_error(
                "输入数据表为空：" + schema + "." + tableName);
        }
    }

    const std::string route =
        quote_identifier(schema) + "." +
        quote_identifier(prefix + "pt_route");
    const std::string stop =
        quote_identifier(schema) + "." +
        quote_identifier(prefix + "pt_stop");
    const std::string shape =
        quote_identifier(schema) + "." +
        quote_identifier(prefix + "pt_shape");
    const std::string transit =
        quote_identifier(schema) + "." +
        quote_identifier(prefix + "pt_transit");
    const std::string walk =
        quote_identifier(schema) + "." +
        quote_identifier(prefix + "pt_walk");
    const std::string trip =
        quote_identifier(schema) + "." +
        quote_identifier(prefix + "pt_trip");
    const bool shapeHasShapeId =
        column_exists(connection.get(), schema, prefix + "pt_shape", "shape_id");
    const bool shapeHasDir =
        column_exists(connection.get(), schema, prefix + "pt_shape", "dir");

    require_no_rows(
        connection.get(),
        "SELECT 1 FROM " + route +
        " WHERE route_id IS NULL OR btrim(route_id)='' LIMIT 1",
        "pt_route.route_id 存在空值");
    require_no_rows(
        connection.get(),
        "SELECT 1 FROM " + stop +
        " WHERE stop_id IS NULL OR btrim(stop_id)='' LIMIT 1",
        "pt_stop.stop_id 存在空值");
    if (shapeHasShapeId) {
        require_no_rows(
            connection.get(),
            "SELECT 1 FROM " + shape +
            " WHERE shape_id IS NULL OR btrim(shape_id)='' "
            "OR route_id IS NULL OR btrim(route_id)='' "
            "OR stop_id IS NULL OR btrim(stop_id)='' LIMIT 1",
            "pt_shape 的 shape_id、route_id 或 stop_id 存在空值");
    } else {
        std::string shapeNullSql =
            "SELECT 1 FROM " + shape +
            " WHERE route_id IS NULL OR btrim(route_id)='' "
            "OR stop_id IS NULL OR btrim(stop_id)=''";
        if (shapeHasDir) {
            shapeNullSql += " OR dir IS NULL";
        }
        shapeNullSql += " LIMIT 1";
        require_no_rows(
            connection.get(),
            shapeNullSql,
            "pt_shape 的 route_id、dir 或 stop_id 存在空值");
    }
    require_no_rows(
        connection.get(),
        "SELECT 1 FROM " + shape +
        " WHERE frequency IS NULL OR frequency<=0 LIMIT 1",
        "pt_shape.frequency 必须大于 0");
    require_no_rows(
        connection.get(),
        "SELECT 1 FROM " + shape +
        " WHERE stop_sequence IS NULL OR stop_sequence<=0 LIMIT 1",
        "pt_shape.stop_sequence 必须为正整数");
    require_no_rows(
        connection.get(),
        "SELECT 1 FROM " + transit +
        " WHERE shape_id IS NULL OR from_stop IS NULL "
        "OR to_stop IS NULL OR fft IS NULL OR fft<0 LIMIT 1",
        "pt_transit 的 shape_id、from_stop、to_stop 不能为空，fft 不能为负数");
    if (hasWalkTable) {
        require_no_rows(
            connection.get(),
            "SELECT 1 FROM " + walk +
            " WHERE from_stop IS NULL OR to_stop IS NULL "
            "OR from_stop=to_stop OR fft IS NULL OR fft<0 "
            "OR length IS NULL OR length<0 LIMIT 1",
            "pt_walk 起终点不能为空或相同，fft 和 length 不能为负数");
    }
    require_no_rows(
        connection.get(),
        "SELECT 1 FROM " + trip +
        " WHERE origin_stop IS NULL OR destination_stop IS NULL "
        "OR demand IS NULL OR demand<0 LIMIT 1",
        "pt_trip 起终点不能为空，demand 不能为负数");

    PGResultPtr demandResult(PQexec(
        connection.get(),
        ("SELECT COALESCE(SUM(demand),0) FROM " + trip).c_str()));
    if (!demandResult ||
        PQresultStatus(demandResult.get()) != PGRES_TUPLES_OK) {
        throw std::runtime_error(
            "无法统计 pt_trip 总需求：" +
            std::string(PQerrorMessage(connection.get())));
    }
    if (std::stod(PQgetvalue(demandResult.get(), 0, 0)) <= 0.0) {
        throw std::runtime_error("pt_trip 总需求必须大于 0");
    }
}

class GlobalExecutionLock {
public:
    GlobalExecutionLock() {
#ifndef _WIN32
        descriptor_ = open(
            "/tmp/tna_transit_assignment_new.lock",
            O_CREAT | O_RDWR,
            0660);
        if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
            if (descriptor_ >= 0) {
                close(descriptor_);
            }
            throw std::runtime_error("无法获取公交分配执行锁");
        }
#endif
    }

    ~GlobalExecutionLock() {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
#endif
    }

    GlobalExecutionLock(const GlobalExecutionLock&) = delete;
    GlobalExecutionLock& operator=(const GlobalExecutionLock&) = delete;

private:
#ifndef _WIN32
    int descriptor_ = -1;
#endif
};

void fill_table_names(
    Response& response,
    const std::string& schema,
    const std::string& prefix) {
    const auto fullName = [&](const std::string& suffix) {
        return schema + "." + prefix + suffix;
    };
    response.tables["stop_table"] = fullName("pt_stop");
    response.tables["route_table"] = fullName("pt_route");
    response.tables["shape_table"] = fullName("pt_shape");
    response.tables["transit_table"] = fullName("pt_transit");
    response.tables["walk_table"] = fullName("pt_walk");
    response.tables["trip_table"] = fullName("pt_trip");
    response.tables["link_result_table"] = fullName("pt_link_result");
    response.tables["path_result_table"] = fullName("pt_path_result");
    response.tables["iter_result_table"] = fullName("pt_iter_result");
    response.tables["summary_result_table"] =
        fullName("pt_summary_result");
    response.tables["stop_link_vc_result_table"] =
        fullName("pt_stop_link_vc_result");
}

std::string build_error_message(int code) {
    const std::map<int, std::string> messages = {
        {1, "公交网络对象状态异常，不能重复构建"},
        {2, "站点数据存在空值或重复 ID"},
        {3, "线路数据存在空值或重复 ID"},
        {4, "shape 数据的线路、站点、频率或停站顺序不合法"},
        {5, "公交网络节点或链路创建失败"},
        {6, "公交运行弧数据不完整、重复或无法匹配 shape"},
        {7, "步行换乘数据不合法"},
        {8, "OD 需求数据不合法或引用了不存在的站点"}
    };
    const auto found = messages.find(code);
    return found == messages.end()
        ? "内部返回码 " + std::to_string(code)
        : found->second;
}

void set_failure(
    Response& response,
    const std::string& stage,
    int internalCode,
    const std::string& message) {
    response.code = -2;
    response.stage = "transit_assignment_new_" + stage;
    response.message = message;
    response.attributes["failure_stage"] = stage;
    response.attributes["failure_code"] =
        std::to_string(internalCode);
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::cerr
            << "用法：transit_assignment_new_cli "
            << "--request-file <file> --response-file <file> "
            << "[--progress-file <file>]\n";
        return 2;
    }

    Response response;
    try {
        const auto request = read_kv(args.requestFile);
        const int projectId = parse_int(get_or(request, "project_id"), -1);
        const int userId = parse_int(get_or(request, "user_id"), -1);
        const int caseId = parse_int(get_or(request, "case_id"), -1);
        if (projectId <= 0 || userId <= 0 || caseId < 0) {
            response.code = -1;
            response.message =
                "参数错误：project_id 和 user_id 必须大于 0，"
                "case_id 必须大于或等于 0";
            write_kv(args.responseFile, response);
            return 0;
        }

        std::string schemaFromConfig;
        std::string connectionString = get_or(request, "param1");
        if (connectionString.empty()) {
            connectionString =
                load_connection_string(schemaFromConfig);
        }
        if (connectionString.empty()) {
            response.code = -1;
            response.message =
                "数据库连接参数为空：请提供 param1，"
                "或配置 TNA_DB_CONN_STR/TNA_DB_CONF";
            write_kv(args.responseFile, response);
            return 0;
        }

        const std::string schema =
            resolve_schema(schemaFromConfig);
        const std::string prefix =
            build_prefix(projectId, userId, caseId);
        fill_table_names(response, schema, prefix);
        response.attributes["schema"] = schema;
        response.attributes["prefix"] = prefix;
        response.attributes["walk_mode"] = "table";
        response.attributes["algorithm"] = "AON";

        append_progress(args.progressFile, 1, "等待公交分配执行锁...");
        GlobalExecutionLock executionLock;
        append_progress(args.progressFile, 5, "正在校验公交输入数据...");

        const PTPostgresConfig database =
            parse_postgres_config(connectionString, schema);
        validate_database_inputs(connectionString, schema, prefix);

        append_progress(
            args.progressFile,
            15,
            "输入数据校验通过，正在读取公交网络...");
        PTNET network(prefix.substr(0, prefix.size() - 1));
        network.Settimescaler(60.0);
        network.SetWriteCsvResults(false);

        const int readResult = network.ReadPostgresInputs(
            projectId,
            userId,
            caseId,
            database);
        if (readResult != 0) {
            set_failure(
                response,
                "read",
                readResult,
                "读取公交输入数据失败：" +
                    (network.lastErrorMessage.empty()
                        ? "内部返回码 " + std::to_string(readResult)
                        : network.lastErrorMessage));
            write_kv(args.responseFile, response);
            return 0;
        }

        const int stabilizeResult = network.StabilizePostgresInputs();
        if (stabilizeResult != 0) {
            set_failure(
                response,
                "stabilize",
                stabilizeResult,
                "公交输入稳定器处理失败：" +
                    (network.lastErrorMessage.empty()
                        ? "内部返回码 " + std::to_string(stabilizeResult)
                        : network.lastErrorMessage));
            write_kv(args.responseFile, response);
            return 0;
        }
        response.counts["stabilizer_added_stops"] =
            network.inputStabilizerAddedStops;
        response.counts["stabilizer_dropped_routes"] =
            network.inputStabilizerDroppedRoutes;
        response.counts["stabilizer_dropped_shape_rows"] =
            network.inputStabilizerDroppedShapeRows;
        response.counts["stabilizer_dropped_shapes"] =
            network.inputStabilizerDroppedShapes;
        response.counts["stabilizer_rebuilt_transit_rows"] =
            network.inputStabilizerRebuiltTransitRows;
        response.counts["stabilizer_dropped_transit_rows"] =
            network.inputStabilizerDroppedTransitRows;
        response.counts["stabilizer_dropped_walk_rows"] =
            network.inputStabilizerDroppedWalkRows;
        response.counts["stabilizer_dropped_trip_rows"] =
            network.inputStabilizerDroppedTripRows;
        response.counts["stabilizer_merged_trip_rows"] =
            network.inputStabilizerMergedTripRows;
        for (const std::string& log : network.inputStabilizerLogs) {
            response.logs.push_back(log);
        }

        append_progress(
            args.progressFile,
            30,
            "公交输入读取完成，正在构建网络...");
        const int buildResult = network.BuildFromPostgresInputs();
        if (buildResult != 0) {
            set_failure(
                response,
                "build",
                buildResult,
                "公交网络构建失败：" +
                    build_error_message(buildResult));
            write_kv(args.responseFile, response);
            return 0;
        }

        append_progress(
            args.progressFile,
            55,
            "公交网络构建完成，正在执行 AON 分配...");
        const int solveResult =
            network.PCTAE_Solver(PTNET::PCTAE_algorithm::PCTAE_P_AON);
        if (solveResult != 0) {
            set_failure(
                response,
                "solve",
                solveResult,
                "公交 AON 分配失败，内部返回码：" +
                    std::to_string(solveResult));
            write_kv(args.responseFile, response);
            return 0;
        }

        append_progress(
            args.progressFile,
            85,
            "AON 分配完成，正在写入结果...");
        const int writeResult = network.WritePostgresResults(
            projectId,
            userId,
            caseId,
            database,
            network.lastAonCpuTimeSeconds);
        if (writeResult != 0) {
            set_failure(
                response,
                "write",
                writeResult,
                "数据库结果写入失败：" +
                    (network.lastErrorMessage.empty()
                        ? "内部返回码 " + std::to_string(writeResult)
                        : network.lastErrorMessage));
            write_kv(args.responseFile, response);
            return 0;
        }

        response.counts["num_of_nodes"] = network.numOfNode;
        response.counts["num_of_links"] = network.numOfLink;
        response.counts["num_of_od_pairs"] = network.numOfPTOD;
        response.counts["num_of_destinations"] = network.numOfPTDest;
        response.counts["num_of_hyperpaths"] = count_hyperpaths(network);
        response.counts["solve_result"] = solveResult;
        response.metrics["total_demand"] = network.numOfPTTrips;
        response.metrics["cpu_time"] =
            network.lastAonCpuTimeSeconds;
        response.code = 1;
        response.message = "公交 AON 分配成功";
        response.logs.push_back(
            "公交 AON 分配完成，四张结果表已在同一事务中提交。");
        append_progress(
            args.progressFile,
            100,
            "公交 AON 分配完成");
    } catch (const std::exception& error) {
        response.code = -2;
        response.stage = "transit_assignment_new_validation";
        response.message = error.what();
        response.attributes["failure_stage"] = "validation";
        response.logs.push_back(response.message);
    } catch (...) {
        response.code = -99;
        response.message = "公交分配发生未知异常";
        response.logs.push_back(response.message);
    }

    try {
        write_kv(args.responseFile, response);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
    return 0;
}
