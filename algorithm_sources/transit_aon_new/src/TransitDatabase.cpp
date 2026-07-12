#include "PTNet.h"

#include <libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {

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

using PGConnectionPtr = std::unique_ptr<PGconn, PGConnectionDeleter>;
using PGResultPtr = std::unique_ptr<PGresult, PGResultDeleter>;

constexpr const char* kStopInputSuffix = "bus_point";
constexpr const char* kWayInputSuffix = "bus_way";

bool is_safe_identifier(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(value[0])) ||
          value[0] == '_')) {
        return false;
    }
    for (char ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
            return false;
        }
    }
    return true;
}

std::string quote_identifier(const std::string& identifier) {
    if (!is_safe_identifier(identifier)) {
        throw std::runtime_error(
            "Unsafe PostgreSQL identifier: " + identifier);
    }
    return "\"" + identifier + "\"";
}

std::string quote_connection_value(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\\' || ch == '\'') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

std::string build_connection_string(
    const PTPostgresConfig& config,
    bool readOnly) {
    std::string connectionString =
        "host=" + quote_connection_value(config.host) +
        " port=" + quote_connection_value(config.port) +
        " dbname=" + quote_connection_value(config.database) +
        " user=" + quote_connection_value(config.user) +
        " password=" + quote_connection_value(config.password) +
        " connect_timeout=" +
            std::to_string(config.connectTimeoutSeconds);
    if (readOnly) {
        connectionString +=
            " options='-c default_transaction_read_only=on'";
    }
    return connectionString;
}

PGResultPtr execute_tuples(PGconn* connection, const std::string& query) {
    PGResultPtr result(PQexec(connection, query.c_str()));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        throw std::runtime_error(
            "PostgreSQL SELECT failed: " +
            std::string(PQerrorMessage(connection)));
    }
    return result;
}

void execute_command(PGconn* connection, const std::string& command) {
    PGResultPtr result(PQexec(connection, command.c_str()));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        throw std::runtime_error(
            "PostgreSQL command failed: " +
            std::string(PQerrorMessage(connection)));
    }
}

bool table_exists(
    PGconn* connection,
    const std::string& schema,
    const std::string& table) {
    PGResultPtr result = execute_tuples(
        connection,
        "SELECT 1 FROM information_schema.tables "
        "WHERE table_schema = " + quote_connection_value(schema) +
            " AND table_name = " + quote_connection_value(table) +
            " LIMIT 1");
    return PQntuples(result.get()) > 0;
}

bool column_exists(
    PGconn* connection,
    const std::string& schema,
    const std::string& table,
    const std::string& column) {
    PGResultPtr result = execute_tuples(
        connection,
        "SELECT 1 FROM information_schema.columns "
        "WHERE table_schema = " + quote_connection_value(schema) +
            " AND table_name = " + quote_connection_value(table) +
            " AND column_name = " + quote_connection_value(column) +
            " LIMIT 1");
    return PQntuples(result.get()) > 0;
}

std::string required_text(PGresult* result, int row, int column) {
    if (PQgetisnull(result, row, column)) {
        throw std::runtime_error("Required PostgreSQL text value is NULL.");
    }
    return PQgetvalue(result, row, column);
}

double required_double(PGresult* result, int row, int column) {
    const std::string value = required_text(result, row, column);
    std::size_t parsed = 0;
    const double number = std::stod(value, &parsed);
    if (parsed != value.size()) {
        throw std::runtime_error(
            "Invalid PostgreSQL numeric value: " + value);
    }
    return number;
}

int required_int(PGresult* result, int row, int column) {
    const std::string value = required_text(result, row, column);
    std::size_t parsed = 0;
    const long number = std::stol(value, &parsed);
    if (parsed != value.size()) {
        throw std::runtime_error(
            "Invalid PostgreSQL integer value: " + value);
    }
    return static_cast<int>(number);
}

std::string copy_text_value(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string copy_double_value(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(4) << value;
    return output.str();
}

std::string join_link_ids(const std::vector<GLINK*>& links) {
    std::ostringstream output;
    for (std::size_t index = 0; index < links.size(); ++index) {
        if (index > 0) {
            output << ';';
        }
        output << links[index]->m_linkPtr->id;
    }
    return output.str();
}

std::string join_link_probabilities(const std::vector<GLINK*>& links) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(4);
    for (std::size_t index = 0; index < links.size(); ++index) {
        if (index > 0) {
            output << ';';
        }
        output << links[index]->m_data;
    }
    return output.str();
}

struct StopLinkVcAccumulator {
    std::string fromStop;
    std::string toStop;
    std::string fromStopName;
    std::string toStopName;
    double flow = 0.0;
    std::set<std::string> routeIds;
    std::set<std::string> shapeIds;
    std::map<std::string, double> shapeCapacities;
};

struct PtWayUpdateRow {
    std::string linkId;
    double flow = 0.0;
    double capacity = 0.0;
    double vcRatio = 0.0;
    std::string vcClass;
    std::string matchedShapeId;
    std::string matchStatus;
};

std::string parse_shape_dir(
    const std::string& shapeId,
    const std::string& routeId) {
    auto parse_int64 = [](const std::string& value, long long& parsed) {
        if (value.empty()) {
            return false;
        }
        char* end = nullptr;
        parsed = std::strtoll(value.c_str(), &end, 10);
        return end && *end == '\0';
    };

    long long shapeNumber = 0;
    long long routeNumber = 0;
    if (parse_int64(shapeId, shapeNumber) &&
        parse_int64(routeId, routeNumber)) {
        const long long suffix = shapeNumber - routeNumber * 100;
        if (suffix >= 0 && suffix <= 99) {
            return std::to_string(suffix / 10);
        }
    }

    const std::string marker = "_dir";
    const std::size_t markerPosition = shapeId.rfind(marker);
    if (markerPosition == std::string::npos) {
        return "";
    }
    const std::string suffix = shapeId.substr(markerPosition + marker.size());
    const std::size_t groupPosition = suffix.find("_g");
    if (groupPosition != std::string::npos) {
        return suffix.substr(0, groupPosition);
    }
    if (!routeId.empty() &&
        markerPosition == routeId.size() &&
        shapeId.compare(0, routeId.size(), routeId) == 0) {
        return suffix;
    }
    return suffix;
}

int parse_shape_group(
    const std::string& shapeId,
    const std::string& routeId) {
    auto parse_int64 = [](const std::string& value, long long& parsed) {
        if (value.empty()) {
            return false;
        }
        char* end = nullptr;
        parsed = std::strtoll(value.c_str(), &end, 10);
        return end && *end == '\0';
    };

    long long shapeNumber = 0;
    long long routeNumber = 0;
    if (parse_int64(shapeId, shapeNumber) &&
        parse_int64(routeId, routeNumber)) {
        const long long suffix = shapeNumber - routeNumber * 100;
        if (suffix >= 0 && suffix <= 99) {
            return static_cast<int>(suffix % 10);
        }
    }

    const std::string marker = "_g";
    const std::size_t markerPosition = shapeId.rfind(marker);
    if (markerPosition == std::string::npos) {
        return 0;
    }
    const std::string suffix = shapeId.substr(markerPosition + marker.size());
    long long group = 0;
    return parse_int64(suffix, group) ? static_cast<int>(group) : 0;
}

std::string derive_way_route_ref(
    const std::string& routeId,
    const std::string& dir,
    int group) {
    if (routeId.empty() || dir.empty() || group < 0) {
        return routeId;
    }

    auto is_digits = [](const std::string& value) {
        return !value.empty() &&
            std::all_of(
                value.begin(),
                value.end(),
                [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                });
    };
    if (!is_digits(routeId) || !is_digits(dir)) {
        return routeId;
    }

    const std::string suffix = dir + std::to_string(group);
    if (routeId.size() <= suffix.size() ||
        routeId.compare(
            routeId.size() - suffix.size(),
            suffix.size(),
            suffix) != 0) {
        return routeId;
    }
    return routeId.substr(0, routeId.size() - suffix.size());
}

std::string encoded_route_dir(const std::string& routeId) {
    if (routeId.size() < 3) {
        return "";
    }
    if (!std::all_of(
            routeId.begin(),
            routeId.end(),
            [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
        return "";
    }
    return routeId.substr(routeId.size() - 2, 1);
}

int encoded_route_group(const std::string& routeId) {
    if (routeId.size() < 3 ||
        !std::all_of(
            routeId.begin(),
            routeId.end(),
            [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
        return 0;
    }
    return routeId.back() - '0';
}

bool column_is_row_identifier(
    PGconn* connection,
    const std::string& schema,
    const std::string& table,
    const std::string& column) {
    PGResultPtr result = execute_tuples(
        connection,
        "SELECT COUNT(*)::bigint, COUNT(DISTINCT " +
            quote_identifier(column) + "::text)::bigint FROM " +
            quote_identifier(schema) + "." + quote_identifier(table));
    const long total = std::stol(required_text(result.get(), 0, 0));
    const long distinct = std::stol(required_text(result.get(), 0, 1));
    return total > 0 && total == distinct;
}

std::string derived_shape_key_expression(bool includeDir) {
    std::string expression = "s.route_id";
    if (includeDir) {
        expression +=
            " || CASE WHEN s.dir IS NOT NULL AND s.dir <> '' "
            "AND position('_dir' in s.route_id) = 0 "
            "THEN '_dir' || s.dir ELSE '' END";
    }
    expression +=
        " || CASE WHEN s.shape_group <> 0 "
        "THEN '_g' || s.shape_group::text ELSE '' END";
    return expression;
}

std::string vc_class(double vcRatio) {
    if (vcRatio < 0.8) {
        return "free";
    }
    if (vcRatio < 1.0) {
        return "near_saturated";
    }
    if (vcRatio < 2.0) {
        return "saturated";
    }
    if (vcRatio < 5.0) {
        return "high";
    }
    return "severe";
}

void start_copy(PGconn* connection, const std::string& command) {
    PGResultPtr result(PQexec(connection, command.c_str()));
    if (!result || PQresultStatus(result.get()) != PGRES_COPY_IN) {
        throw std::runtime_error(
            "PostgreSQL COPY start failed: " +
            std::string(PQerrorMessage(connection)));
    }
}

void write_copy_row(
    PGconn* connection,
    const std::vector<std::string>& encodedFields) {
    std::string row;
    for (std::size_t index = 0; index < encodedFields.size(); ++index) {
        if (index > 0) {
            row.push_back('\t');
        }
        row += encodedFields[index];
    }
    row.push_back('\n');

    if (PQputCopyData(
            connection,
            row.data(),
            static_cast<int>(row.size())) != 1) {
        throw std::runtime_error(
            "PostgreSQL COPY data failed: " +
            std::string(PQerrorMessage(connection)));
    }
}

void finish_copy(PGconn* connection) {
    if (PQputCopyEnd(connection, nullptr) != 1) {
        throw std::runtime_error(
            "PostgreSQL COPY end failed: " +
            std::string(PQerrorMessage(connection)));
    }

    bool receivedResult = false;
    while (PGresult* rawResult = PQgetResult(connection)) {
        receivedResult = true;
        PGResultPtr result(rawResult);
        if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
            throw std::runtime_error(
                "PostgreSQL COPY failed: " +
                std::string(PQerrorMessage(connection)));
        }
    }
    if (!receivedResult) {
        throw std::runtime_error(
            "PostgreSQL COPY returned no completion result.");
    }
}

}  // namespace

int PTNET::ReadPostgresInputs(
    int projectId,
    int userId,
    int caseId,
    const PTPostgresConfig& config) {
    dbRouteRecords.clear();
    dbStopRecords.clear();
    dbShapeStopRecords.clear();
    dbTransitRecords.clear();
    dbWalkRecords.clear();
    dbTripRecords.clear();
    dbWayRecords.clear();
    lastErrorMessage.clear();

    try {
        if (config.host.empty() ||
            config.port.empty() ||
            config.database.empty() ||
            config.user.empty() ||
            config.password.empty() ||
            config.connectTimeoutSeconds <= 0) {
            throw std::runtime_error(
                "PostgreSQL configuration in main.cpp is incomplete.");
        }
        if (!is_safe_identifier(config.schema)) {
            throw std::runtime_error("Invalid PostgreSQL schema name.");
        }

        const std::string prefix =
            "project" + std::to_string(projectId) +
            "_user" + std::to_string(userId) +
            "_case" + std::to_string(caseId) + "_";
        const std::string qualifiedPrefix =
            quote_identifier(config.schema) + ".";

        PGConnectionPtr connection(
            PQconnectdb(build_connection_string(config, true).c_str()));
        if (!connection ||
            PQstatus(connection.get()) != CONNECTION_OK) {
            throw std::runtime_error(
                "Cannot connect to PostgreSQL: " +
                std::string(
                    connection
                        ? PQerrorMessage(connection.get())
                        : "connection allocation failed"));
        }

        execute_command(connection.get(), "BEGIN READ ONLY");

        {
            const std::string table = prefix + "pt_route";
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT route_id FROM " + qualifiedPrefix +
                    quote_identifier(table) + " ORDER BY route_id");
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                dbRouteRecords.push_back(
                    {required_text(result.get(), row, 0)});
            }
        }

        {
            const std::string table = prefix + kStopInputSuffix;
            const bool hasStopId =
                column_exists(connection.get(), config.schema, table, "stop_id");
            const bool hasNodeId =
                column_exists(connection.get(), config.schema, table, "node_id");
            const bool hasStopName =
                column_exists(connection.get(), config.schema, table, "stop_name");
            const bool hasNodeName =
                column_exists(connection.get(), config.schema, table, "node_name");
            if (!hasStopId && !hasNodeId) {
                throw std::runtime_error(
                    "bus_point 缺少 stop_id 或 node_id 字段。");
            }
            const std::string stopIdColumn = hasStopId ? "stop_id" : "node_id";
            const std::string stopNameExpression =
                hasStopName ? "stop_name" : (hasNodeName ? "node_name" : "''::text");
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT " + quote_identifier(stopIdColumn) +
                    "::text AS stop_id, " + stopNameExpression +
                    "::text AS stop_name FROM " + qualifiedPrefix +
                    quote_identifier(table) + " ORDER BY " +
                    quote_identifier(stopIdColumn));
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                dbStopRecords.push_back({
                    required_text(result.get(), row, 0),
                    PQgetisnull(result.get(), row, 1)
                        ? ""
                        : PQgetvalue(result.get(), row, 1)
                });
            }
        }

        {
            const std::string table = prefix + "pt_shape";
            const bool hasShapeId =
                column_exists(connection.get(), config.schema, table, "shape_id");
            const bool hasDir =
                column_exists(connection.get(), config.schema, table, "dir");
            const bool hasGroup =
                column_exists(connection.get(), config.schema, table, "group");
            const bool hasB =
                column_exists(connection.get(), config.schema, table, "b");
            const bool shapeIdIsRowId =
                hasShapeId &&
                column_is_row_identifier(
                    connection.get(), config.schema, table, "shape_id");

            const std::string sourceShapeIdExpression =
                hasShapeId ? "shape_id::text" : "NULL::text";
            const std::string dirSelectExpression =
                hasDir ? "dir::text" : "NULL::text";
            std::string orderByExpression = "stop_sequence::integer";
            if (hasShapeId) {
                orderByExpression =
                    "CASE WHEN shape_id::text ~ '^[0-9]+$' "
                    "THEN shape_id::bigint ELSE NULL END, "
                    "shape_id::text, stop_sequence::integer";
            }
            std::string groupSelectExpression;
            if (hasGroup) {
                groupSelectExpression = "\"group\"::integer";
            } else if (hasB && hasDir) {
                groupSelectExpression =
                    "GREATEST(("
                    "SUM(CASE WHEN b::integer = 1 THEN 1 ELSE 0 END) "
                    "OVER (PARTITION BY route_id::text, dir::text "
                    "ORDER BY " + orderByExpression + " "
                    "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)"
                    ") - 1, 0)::integer";
            } else {
                groupSelectExpression = "0::integer";
            }
            const std::string shapeIdExpression =
                shapeIdIsRowId
                    ? "s.route_id"
                    : (hasShapeId ? "s.source_shape_id"
                                  : derived_shape_key_expression(hasDir));
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT " + shapeIdExpression + " AS shape_id, "
                "s.route_id, s.dir, s.shape_group, s.frequency, "
                "s.stop_sequence, s.stop_id FROM ("
                "SELECT " + sourceShapeIdExpression + " AS source_shape_id, "
                "route_id::text AS route_id, " +
                dirSelectExpression + " AS dir, " +
                groupSelectExpression + " AS shape_group, "
                "frequency, stop_sequence, stop_id::text AS stop_id FROM " +
                    qualifiedPrefix + quote_identifier(table) +
                ") s ORDER BY shape_id, s.stop_sequence");
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                const std::string shapeId = required_text(result.get(), row, 0);
                const std::string routeId = required_text(result.get(), row, 1);
                std::string dir = PQgetisnull(result.get(), row, 2)
                    ? ""
                    : PQgetvalue(result.get(), row, 2);
                if (dir.empty()) {
                    dir = parse_shape_dir(shapeId, routeId);
                }
                int group = required_int(result.get(), row, 3);
                if (!hasGroup) {
                    const std::string encodedDir = encoded_route_dir(routeId);
                    if (!encodedDir.empty() &&
                        (dir.empty() || encodedDir == dir)) {
                        group = encoded_route_group(routeId);
                    } else {
                        const int parsedGroup =
                            parse_shape_group(shapeId, routeId);
                        if (parsedGroup != 0 || group == 0) {
                            group = parsedGroup;
                        }
                    }
                }
                dbShapeStopRecords.push_back({
                    shapeId,
                    routeId,
                    dir,
                    group,
                    required_double(result.get(), row, 4),
                    required_int(result.get(), row, 5),
                    required_text(result.get(), row, 6)
                });
            }
        }

        {
            const std::string table = prefix + "pt_transit";
            const bool hasShapeId =
                column_exists(connection.get(), config.schema, table, "shape_id");
            const bool hasRouteId =
                column_exists(connection.get(), config.schema, table, "route_id");
            if (!hasShapeId && !hasRouteId) {
                throw std::runtime_error(
                    "pt_transit 缺少 shape_id 或 route_id 字段。");
            }
            const std::string transitKeyColumn =
                hasShapeId ? "shape_id" : "route_id";
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT " + quote_identifier(transitKeyColumn) +
                    "::text AS shape_id, from_stop, to_stop, fft FROM " +
                    qualifiedPrefix + quote_identifier(table) +
                    " ORDER BY shape_id, from_stop, to_stop");
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                dbTransitRecords.push_back({
                    required_text(result.get(), row, 0),
                    required_text(result.get(), row, 1),
                    required_text(result.get(), row, 2),
                    required_double(result.get(), row, 3)
                });
            }
        }

        {
            const std::string table = prefix + "pt_walk";
            if (table_exists(connection.get(), config.schema, table)) {
                PGResultPtr result = execute_tuples(
                    connection.get(),
                    "SELECT from_stop, to_stop, fft, length FROM " +
                        qualifiedPrefix + quote_identifier(table) +
                        " ORDER BY from_stop, to_stop");
                for (int row = 0; row < PQntuples(result.get()); ++row) {
                    dbWalkRecords.push_back({
                        required_text(result.get(), row, 0),
                        required_text(result.get(), row, 1),
                        required_double(result.get(), row, 2),
                        required_double(result.get(), row, 3)
                    });
                }
            }
        }

        {
            const std::string table = prefix + kWayInputSuffix;
            if (!table_exists(connection.get(), config.schema, table)) {
                throw std::runtime_error(
                    "缺少输入数据表：" + config.schema + "." + table +
                    "。当前公交分配要求 bus_way 作为输入表。");
            }
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT link_id::text, route_ref::text, direction::text, "
                "GREATEST(("
                "SUM(CASE WHEN b::integer = 1 THEN 1 ELSE 0 END) "
                "OVER (PARTITION BY route_ref::text, direction::text "
                "ORDER BY power::integer, link_id::text "
                "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)"
                ") - 1, 0)::integer AS way_group, "
                "power::integer, init_node::text, term_node::text "
                "FROM " + qualifiedPrefix + quote_identifier(table) +
                " ORDER BY route_ref, direction, way_group, power");
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                dbWayRecords.push_back({
                    required_text(result.get(), row, 0),
                    required_text(result.get(), row, 1),
                    required_text(result.get(), row, 2),
                    required_int(result.get(), row, 3),
                    required_int(result.get(), row, 4),
                    required_text(result.get(), row, 5),
                    required_text(result.get(), row, 6)
                });
            }
        }

        {
            const std::string table = prefix + "pt_trip";
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT origin_stop, destination_stop, demand FROM " +
                    qualifiedPrefix + quote_identifier(table) +
                    " ORDER BY destination_stop, origin_stop");
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                dbTripRecords.push_back({
                    required_text(result.get(), row, 0),
                    required_text(result.get(), row, 1),
                    required_double(result.get(), row, 2)
                });
            }
        }

        execute_command(connection.get(), "ROLLBACK");
        return 0;
    } catch (const std::exception& error) {
        lastErrorMessage = error.what();
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

int PTNET::StabilizePostgresInputs() {
    inputStabilizerLogs.clear();
    inputStabilizerAddedStops = 0;
    inputStabilizerDroppedRoutes = 0;
    inputStabilizerDroppedShapeRows = 0;
    inputStabilizerDroppedShapes = 0;
    inputStabilizerRebuiltTransitRows = 0;
    inputStabilizerDroppedTransitRows = 0;
    inputStabilizerDroppedWalkRows = 0;
    inputStabilizerDroppedTripRows = 0;
    inputStabilizerMergedTripRows = 0;
    lastErrorMessage.clear();

    std::set<std::string> routeIds;
    std::vector<PTDbRouteRecord> cleanRoutes;
    cleanRoutes.reserve(dbRouteRecords.size());
    for (const PTDbRouteRecord& route : dbRouteRecords) {
        if (route.routeId.empty() || !routeIds.insert(route.routeId).second) {
            ++inputStabilizerDroppedRoutes;
            continue;
        }
        cleanRoutes.push_back(route);
    }
    dbRouteRecords.swap(cleanRoutes);

    std::set<std::string> stopIds;
    std::vector<PTDbStopRecord> cleanStops;
    cleanStops.reserve(dbStopRecords.size());
    for (const PTDbStopRecord& stop : dbStopRecords) {
        if (stop.stopId.empty() || !stopIds.insert(stop.stopId).second) {
            continue;
        }
        cleanStops.push_back(stop);
    }
    dbStopRecords.swap(cleanStops);

    int addedShapeRoutes = 0;
    for (const PTDbShapeStopRecord& row : dbShapeStopRecords) {
        if (!row.routeId.empty() && routeIds.insert(row.routeId).second) {
            dbRouteRecords.push_back({row.routeId});
            ++addedShapeRoutes;
        }
    }
    if (addedShapeRoutes > 0) {
        inputStabilizerLogs.push_back(
            "Added " + std::to_string(addedShapeRoutes) +
            " route ids from pt_shape for schema compatibility.");
    }

    std::map<std::string, std::vector<PTDbShapeStopRecord> > shapeGroups;
    for (const PTDbShapeStopRecord& row : dbShapeStopRecords) {
        shapeGroups[row.shapeId].push_back(row);
    }

    std::vector<PTDbShapeStopRecord> cleanShapes;
    for (auto& item : shapeGroups) {
        std::vector<PTDbShapeStopRecord>& rows = item.second;
        std::sort(
            rows.begin(),
            rows.end(),
            [](const PTDbShapeStopRecord& a, const PTDbShapeStopRecord& b) {
                return a.stopSequence < b.stopSequence;
            });

        std::vector<PTDbShapeStopRecord> shapeRows;
        std::set<int> usedSequences;
        for (const PTDbShapeStopRecord& row : rows) {
            if (row.shapeId.empty() ||
                row.routeId.empty() ||
                row.stopId.empty() ||
                row.frequency <= 0.0 ||
                row.stopSequence <= 0 ||
                routeIds.find(row.routeId) == routeIds.end() ||
                stopIds.find(row.stopId) == stopIds.end() ||
                !usedSequences.insert(row.stopSequence).second) {
                ++inputStabilizerDroppedShapeRows;
                continue;
            }
            if (!shapeRows.empty() && shapeRows.back().stopId == row.stopId) {
                ++inputStabilizerDroppedShapeRows;
                continue;
            }
            shapeRows.push_back(row);
        }

        if (shapeRows.size() < 2) {
            ++inputStabilizerDroppedShapes;
            inputStabilizerDroppedShapeRows +=
                static_cast<int>(shapeRows.size());
            continue;
        }

        for (std::size_t index = 0; index < shapeRows.size(); ++index) {
            shapeRows[index].stopSequence = static_cast<int>(index) + 1;
            cleanShapes.push_back(shapeRows[index]);
        }
    }
    dbShapeStopRecords.swap(cleanShapes);

    std::map<std::string, std::vector<PTDbShapeStopRecord> > cleanShapeGroups;
    for (const PTDbShapeStopRecord& row : dbShapeStopRecords) {
        cleanShapeGroups[row.shapeId].push_back(row);
    }

    using TransitKey = std::tuple<std::string, std::string, std::string>;
    std::map<TransitKey, double> originalTransitFft;
    std::map<TransitKey, double> originalRouteTransitFft;
    for (const PTDbTransitRecord& row : dbTransitRecords) {
        const TransitKey key(row.shapeId, row.fromStop, row.toStop);
        if (row.shapeId.empty() ||
            row.fromStop.empty() ||
            row.toStop.empty() ||
            row.fft < 0.0 ||
            originalTransitFft.find(key) != originalTransitFft.end()) {
            ++inputStabilizerDroppedTransitRows;
            continue;
        }
        originalTransitFft[key] = row.fft;
        originalRouteTransitFft[key] = row.fft;
    }

    std::vector<PTDbTransitRecord> rebuiltTransit;
    for (auto& item : cleanShapeGroups) {
        std::vector<PTDbShapeStopRecord>& rows = item.second;
        std::sort(
            rows.begin(),
            rows.end(),
            [](const PTDbShapeStopRecord& a, const PTDbShapeStopRecord& b) {
                return a.stopSequence < b.stopSequence;
            });
        for (std::size_t index = 1; index < rows.size(); ++index) {
            const TransitKey key(
                rows[index].shapeId,
                rows[index - 1].stopId,
                rows[index].stopId);
            const TransitKey routeKey(
                rows[index].routeId,
                rows[index - 1].stopId,
                rows[index].stopId);
            double fft = 1.0;
            bool foundFft = false;
            const auto foundByShape = originalTransitFft.find(key);
            if (foundByShape != originalTransitFft.end()) {
                fft = foundByShape->second;
                foundFft = true;
            } else {
                const auto foundByRoute = originalRouteTransitFft.find(routeKey);
                if (foundByRoute != originalRouteTransitFft.end()) {
                    fft = foundByRoute->second;
                    foundFft = true;
                }
            }
            if (!foundFft) {
                ++inputStabilizerRebuiltTransitRows;
            }
            rebuiltTransit.push_back({
                rows[index].shapeId,
                rows[index - 1].stopId,
                rows[index].stopId,
                fft
            });
        }
    }
    if (originalTransitFft.size() > rebuiltTransit.size()) {
        inputStabilizerDroppedTransitRows +=
            static_cast<int>(originalTransitFft.size() - rebuiltTransit.size());
    }
    dbTransitRecords.swap(rebuiltTransit);

    std::set<std::pair<std::string, std::string> > walkKeys;
    std::vector<PTDbWalkRecord> cleanWalk;
    cleanWalk.reserve(dbWalkRecords.size());
    for (const PTDbWalkRecord& row : dbWalkRecords) {
        const std::pair<std::string, std::string> key(row.fromStop, row.toStop);
        if (row.fromStop.empty() ||
            row.toStop.empty() ||
            row.fromStop == row.toStop ||
            row.fft < 0.0 ||
            row.length < 0.0 ||
            stopIds.find(row.fromStop) == stopIds.end() ||
            stopIds.find(row.toStop) == stopIds.end() ||
            !walkKeys.insert(key).second) {
            ++inputStabilizerDroppedWalkRows;
            continue;
        }
        cleanWalk.push_back(row);
    }
    dbWalkRecords.swap(cleanWalk);

    std::map<std::pair<std::string, std::string>, double> tripDemandByOd;
    for (const PTDbTripRecord& row : dbTripRecords) {
        if (row.originStop.empty() ||
            row.destinationStop.empty() ||
            row.demand <= 0.0 ||
            stopIds.find(row.originStop) == stopIds.end() ||
            stopIds.find(row.destinationStop) == stopIds.end()) {
            ++inputStabilizerDroppedTripRows;
            continue;
        }
        const std::pair<std::string, std::string> key(
            row.originStop,
            row.destinationStop);
        if (tripDemandByOd.find(key) != tripDemandByOd.end()) {
            ++inputStabilizerMergedTripRows;
        }
        tripDemandByOd[key] += row.demand;
    }

    std::vector<PTDbTripRecord> cleanTrips;
    cleanTrips.reserve(tripDemandByOd.size());
    double totalDemand = 0.0;
    for (const auto& item : tripDemandByOd) {
        cleanTrips.push_back({
            item.first.first,
            item.first.second,
            item.second
        });
        totalDemand += item.second;
    }
    dbTripRecords.swap(cleanTrips);

    if (dbRouteRecords.empty()) {
        lastErrorMessage = "稳定器处理后没有有效公交线路 route";
        return 1;
    }
    if (dbShapeStopRecords.empty() || dbTransitRecords.empty()) {
        lastErrorMessage = "稳定器处理后没有有效线路站序或运行弧";
        return 1;
    }
    if (dbTripRecords.empty() || totalDemand <= 0.0) {
        lastErrorMessage = "稳定器处理后没有有效 OD 需求";
        return 1;
    }

    std::ostringstream summary;
    summary
        << "公交输入稳定器完成："
        << "shape_rows_dropped=" << inputStabilizerDroppedShapeRows
        << ", shapes_dropped=" << inputStabilizerDroppedShapes
        << ", transit_rebuilt=" << inputStabilizerRebuiltTransitRows
        << ", transit_dropped=" << inputStabilizerDroppedTransitRows
        << ", walk_dropped=" << inputStabilizerDroppedWalkRows
        << ", trips_dropped=" << inputStabilizerDroppedTripRows
        << ", trips_merged=" << inputStabilizerMergedTripRows;
    inputStabilizerLogs.push_back(summary.str());
    std::cout << "\t" << summary.str() << std::endl;
    return 0;
}

int PTNET::WritePostgresResults(
    int projectId,
    int userId,
    int caseId,
    const PTPostgresConfig& config,
    double cpuTimeSeconds) {
    PGConnectionPtr connection;
    bool transactionStarted = false;
    lastErrorMessage.clear();

    try {
        if (projectId <= 0 || userId <= 0 || caseId < 0) {
            throw std::runtime_error(
                "Invalid projectId, userId, or caseId for result writing.");
        }
        if (config.host.empty() ||
            config.port.empty() ||
            config.database.empty() ||
            config.user.empty() ||
            config.password.empty() ||
            config.connectTimeoutSeconds <= 0) {
            throw std::runtime_error(
                "PostgreSQL configuration in main.cpp is incomplete.");
        }
        if (!is_safe_identifier(config.schema)) {
            throw std::runtime_error("Invalid PostgreSQL schema name.");
        }

        const std::string prefix =
            "project" + std::to_string(projectId) +
            "_user" + std::to_string(userId) +
            "_case" + std::to_string(caseId) + "_";
        const std::string qualifiedPrefix =
            quote_identifier(config.schema) + ".";
        const std::string linkTable =
            qualifiedPrefix + quote_identifier(prefix + "pt_link_result");
        const std::string pathTable =
            qualifiedPrefix + quote_identifier(prefix + "pt_path_result");
        const std::string iterTable =
            qualifiedPrefix + quote_identifier(prefix + "pt_iter_result");
        const std::string summaryTable =
            qualifiedPrefix + quote_identifier(prefix + "pt_summary_result");
        const std::string stopLinkVcTable =
            qualifiedPrefix + quote_identifier(prefix + "pt_stop_link_vc_result");
        const std::string ptWayTableName = prefix + kWayInputSuffix;
        const std::string ptWayTable =
            qualifiedPrefix + quote_identifier(ptWayTableName);
        const double assumedVehicleCapacity = 60.0;

        connection.reset(
            PQconnectdb(build_connection_string(config, false).c_str()));
        if (!connection ||
            PQstatus(connection.get()) != CONNECTION_OK) {
            throw std::runtime_error(
                "Cannot connect to PostgreSQL for result writing: " +
                std::string(
                    connection
                        ? PQerrorMessage(connection.get())
                        : "connection allocation failed"));
        }

        execute_command(connection.get(), "BEGIN");
        transactionStarted = true;

        execute_command(connection.get(), "DROP TABLE IF EXISTS " + linkTable);
        execute_command(
            connection.get(),
            "CREATE TABLE " + linkTable + " ("
            "link_id integer, "
            "link_type text, "
            "tail_node_id integer, "
            "head_node_id integer, "
            "tail_stop_id text, "
            "head_stop_id text, "
            "route_id text, "
            "shape_id text, "
            "seq integer, "
            "flow double precision, "
            "cost double precision)");

        execute_command(connection.get(), "DROP TABLE IF EXISTS " + pathTable);
        execute_command(
            connection.get(),
            "CREATE TABLE " + pathTable + " ("
            "origin_node_id integer, "
            "destination_node_id integer, "
            "origin_stop_id text, "
            "destination_stop_id text, "
            "path_index integer, "
            "path_flow double precision, "
            "path_cost double precision, "
            "wait_cost double precision, "
            "num_links integer, "
            "link_sequence text, "
            "link_probability text)");

        execute_command(connection.get(), "DROP TABLE IF EXISTS " + iterTable);
        execute_command(
            connection.get(),
            "CREATE TABLE " + iterTable + " ("
            "cpu_time double precision, "
            "num_hyperpaths integer, "
            "infeasible_flow double precision)");

        execute_command(
            connection.get(),
            "DROP TABLE IF EXISTS " + summaryTable);
        execute_command(
            connection.get(),
            "CREATE TABLE " + summaryTable + " ("
            "metric_key text, "
            "metric_value double precision)");

        execute_command(
            connection.get(),
            "DROP TABLE IF EXISTS " + stopLinkVcTable);
        execute_command(
            connection.get(),
            "CREATE TABLE " + stopLinkVcTable + " ("
            "from_stop text, "
            "to_stop text, "
            "from_stop_name text, "
            "to_stop_name text, "
            "flow double precision, "
            "route_count integer, "
            "shape_count integer, "
            "capacity double precision, "
            "vc_ratio double precision, "
            "capacity_assumption text)");

        start_copy(
            connection.get(),
            "COPY " + linkTable +
            " (link_id, link_type, tail_node_id, head_node_id, "
            "tail_stop_id, head_stop_id, route_id, shape_id, seq, flow, cost) "
            "FROM STDIN WITH (FORMAT text)");
        for (PTLink* link : linkVector) {
            const bool belongsToShape =
                link->GetTransitLinkType() != PTLink::WALK &&
                link->GetTransitLinkType() != PTLink::TRANSFER &&
                link->GetTransitLinkType() != PTLink::FAILWALK &&
                link->m_shape != nullptr;
            const std::string tailStopId =
                link->tail && link->tail->m_stop
                    ? copy_text_value(link->tail->m_stop->m_id)
                    : "\\N";
            const std::string headStopId =
                link->head && link->head->m_stop
                    ? copy_text_value(link->head->m_stop->m_id)
                    : "\\N";
            const std::string routeId =
                belongsToShape && link->m_shape->m_routePtr
                    ? copy_text_value(link->m_shape->m_routePtr->m_id)
                    : "\\N";
            const std::string shapeId =
                belongsToShape
                    ? copy_text_value(link->m_shape->m_id)
                    : "\\N";
            const std::string sequence =
                belongsToShape
                    ? std::to_string(link->seq + 1)
                    : "\\N";

            write_copy_row(
                connection.get(),
                {
                    std::to_string(link->id),
                    copy_text_value(link->GetTransitLinkTypeName()),
                    std::to_string(link->tail->id),
                    std::to_string(link->head->id),
                    tailStopId,
                    headStopId,
                    routeId,
                    shapeId,
                    sequence,
                    copy_double_value(link->volume),
                    copy_double_value(link->cost)
                });
        }
        finish_copy(connection.get());

        double assignedDemand = 0.0;
        double infeasibleFlow = 0.0;
        double totalSystemCost = 0.0;
        double totalWaitCost = 0.0;
        int numHyperpaths = 0;

        start_copy(
            connection.get(),
            "COPY " + pathTable +
            " (origin_node_id, destination_node_id, origin_stop_id, "
            "destination_stop_id, path_index, path_flow, path_cost, "
            "wait_cost, num_links, link_sequence, link_probability) "
            "FROM STDIN WITH (FORMAT text)");
        for (PTDestination* destination : PTDestVector) {
            for (int originIndex = 0;
                 originIndex < destination->numOfOrg;
                 ++originIndex) {
                PTOrg* origin = destination->orgVector[originIndex];
                if (!origin->state || origin->pathSet.empty()) {
                    infeasibleFlow += origin->assDemand;
                    continue;
                }

                for (std::size_t pathIndex = 0;
                     pathIndex < origin->pathSet.size();
                     ++pathIndex) {
                    TNM_HyperPath* path = origin->pathSet[pathIndex];
                    const std::vector<GLINK*> links = path->GetGlinks();
                    ++numHyperpaths;
                    assignedDemand += path->flow;
                    totalSystemCost += path->flow * path->cost;
                    totalWaitCost += path->flow * path->WaitCost;

                    write_copy_row(
                        connection.get(),
                        {
                            std::to_string(origin->org->id),
                            std::to_string(destination->destination->id),
                            copy_text_value(origin->org->m_stop->m_id),
                            copy_text_value(
                                destination->destination->m_stop->m_id),
                            std::to_string(pathIndex + 1),
                            copy_double_value(path->flow),
                            copy_double_value(path->cost),
                            copy_double_value(path->WaitCost),
                            std::to_string(links.size()),
                            copy_text_value(join_link_ids(links)),
                            copy_text_value(
                                join_link_probabilities(links))
                        });
                }
            }
        }
        finish_copy(connection.get());

        start_copy(
            connection.get(),
            "COPY " + iterTable +
            " (cpu_time, num_hyperpaths, infeasible_flow) "
            "FROM STDIN WITH (FORMAT text)");
        write_copy_row(
            connection.get(),
            {
                copy_double_value(cpuTimeSeconds),
                std::to_string(numHyperpaths),
                copy_double_value(infeasibleFlow)
            });
        finish_copy(connection.get());

        int walkLinks = 0;
        int positiveFlowLinks = 0;
        double totalPassengerLinkFlow = 0.0;
        std::map<std::pair<std::string, std::string>, StopLinkVcAccumulator>
            stopLinkVcRows;
        for (PTLink* link : linkVector) {
            if (link->GetTransitLinkType() == PTLink::WALK) {
                ++walkLinks;
            }
            if (link->volume > flowPrecision) {
                ++positiveFlowLinks;
            }
            totalPassengerLinkFlow += link->volume;

            if (link->GetTransitLinkType() == PTLink::ENROUTE &&
                link->tail && link->head &&
                link->tail->m_stop && link->head->m_stop) {
                const std::string fromStop = link->tail->m_stop->m_id;
                const std::string toStop = link->head->m_stop->m_id;
                StopLinkVcAccumulator& row =
                    stopLinkVcRows[std::make_pair(fromStop, toStop)];
                row.fromStop = fromStop;
                row.toStop = toStop;
                row.fromStopName = link->tail->m_stop->m_name;
                row.toStopName = link->head->m_stop->m_name;
                row.flow += link->volume;
                if (link->m_shape) {
                    row.shapeIds.insert(link->m_shape->m_id);
                    if (row.shapeCapacities.find(link->m_shape->m_id) ==
                        row.shapeCapacities.end()) {
                        const double frequency =
                            std::max(0.0, link->m_shape->m_freq);
                        row.shapeCapacities[link->m_shape->m_id] =
                            frequency * assumedVehicleCapacity;
                    }
                    if (link->m_shape->m_routePtr) {
                        row.routeIds.insert(link->m_shape->m_routePtr->m_id);
                    }
                }
            }
        }

        start_copy(
            connection.get(),
            "COPY " + stopLinkVcTable +
            " (from_stop, to_stop, from_stop_name, to_stop_name, flow, "
            "route_count, shape_count, capacity, vc_ratio, "
            "capacity_assumption) FROM STDIN WITH (FORMAT text)");
        for (const auto& item : stopLinkVcRows) {
            const StopLinkVcAccumulator& row = item.second;
            const int shapeCount =
                static_cast<int>(row.shapeIds.empty() ? 1 : row.shapeIds.size());
            double capacity = 0.0;
            for (const auto& shapeCapacity : row.shapeCapacities) {
                capacity += shapeCapacity.second;
            }
            if (capacity <= 0.0) {
                capacity = assumedVehicleCapacity * shapeCount;
            }
            const double vcRatio =
                capacity > 0.0 ? row.flow / capacity : 0.0;
            write_copy_row(
                connection.get(),
                {
                    copy_text_value(row.fromStop),
                    copy_text_value(row.toStop),
                    copy_text_value(row.fromStopName),
                    copy_text_value(row.toStopName),
                    copy_double_value(row.flow),
                    std::to_string(row.routeIds.size()),
                    std::to_string(shapeCount),
                    copy_double_value(capacity),
                    copy_double_value(vcRatio),
                    copy_text_value(
                        "capacity = sum(shape.frequency * 60 passengers/vehicle)")
                });
        }
        finish_copy(connection.get());

        int ptWayVcRows = 0;
        int ptWayFlowRows = 0;
        int busWayZeroFlowRows = 0;
        double busWayZeroFlowRatio = 0.0;
        if (!dbWayRecords.empty() &&
            table_exists(connection.get(), config.schema, ptWayTableName)) {
            using RouteShapeKey = std::tuple<std::string, std::string, int>;

            std::map<RouteShapeKey, std::vector<PTDbWayRecord> > wayGroups;
            for (const PTDbWayRecord& way : dbWayRecords) {
                if (way.linkId.empty() ||
                    way.routeId.empty() ||
                    way.initNode.empty() ||
                    way.termNode.empty() ||
                    way.linkSequence <= 0) {
                    continue;
                }
                wayGroups[RouteShapeKey(way.routeId, way.dir, way.group)].
                    push_back(way);
            }
            for (auto& item : wayGroups) {
                std::sort(
                    item.second.begin(),
                    item.second.end(),
                    [](const PTDbWayRecord& a, const PTDbWayRecord& b) {
                        return a.linkSequence < b.linkSequence;
                    });
            }

            std::map<RouteShapeKey, std::vector<PTDbShapeStopRecord> >
                shapeGroupsByDir;
            std::map<RouteShapeKey, double> shapeFrequencyByDir;
            std::map<RouteShapeKey, std::string> shapeIdByDir;
            for (const PTDbShapeStopRecord& row : dbShapeStopRecords) {
                const std::string dir = row.dir.empty()
                    ? parse_shape_dir(row.shapeId, row.routeId)
                    : row.dir;
                const int group = row.group;
                const RouteShapeKey key(row.routeId, dir, group);
                shapeGroupsByDir[key].push_back(row);
                shapeFrequencyByDir[key] =
                    std::max(shapeFrequencyByDir[key], row.frequency);
                if (shapeIdByDir[key].empty()) {
                    shapeIdByDir[key] = row.shapeId;
                }
            }
            for (auto& item : shapeGroupsByDir) {
                std::sort(
                    item.second.begin(),
                    item.second.end(),
                    [](const PTDbShapeStopRecord& a,
                       const PTDbShapeStopRecord& b) {
                        return a.stopSequence < b.stopSequence;
                    });
            }

            std::map<RouteShapeKey, RouteShapeKey> shapeDirToWayDir;
            for (const auto& wayItem : wayGroups) {
                const RouteShapeKey& wayKey = wayItem.first;
                const std::vector<PTDbWayRecord>& ways = wayItem.second;
                if (ways.empty()) {
                    continue;
                }

                std::map<std::string, int> nodePosition;
                nodePosition[ways.front().initNode] = 0;
                for (std::size_t index = 0; index < ways.size(); ++index) {
                    nodePosition[ways[index].termNode] =
                        static_cast<int>(index) + 1;
                }

                bool hasBest = false;
                RouteShapeKey bestShapeKey;
                int bestOrdered = -1;
                int bestOverlap = -1;
                for (const auto& shapeItem : shapeGroupsByDir) {
                    const RouteShapeKey& shapeKey = shapeItem.first;
                    const std::string shapeRouteId = std::get<0>(shapeKey);
                    const std::string shapeRouteRef =
                        derive_way_route_ref(
                            shapeRouteId,
                            std::get<1>(shapeKey),
                            std::get<2>(shapeKey));
                    const std::string wayRouteRef = std::get<0>(wayKey);
                    if ((shapeRouteId != wayRouteRef &&
                         shapeRouteRef != wayRouteRef) ||
                        std::get<2>(shapeKey) != std::get<2>(wayKey)) {
                        continue;
                    }
                    const std::vector<PTDbShapeStopRecord>& stops =
                        shapeItem.second;
                    int overlap = 0;
                    int ordered = 0;
                    for (const PTDbShapeStopRecord& stopRow : stops) {
                        if (nodePosition.find(stopRow.stopId) !=
                            nodePosition.end()) {
                            ++overlap;
                        }
                    }
                    for (std::size_t index = 1;
                         index < stops.size();
                         ++index) {
                        const auto previous =
                            nodePosition.find(stops[index - 1].stopId);
                        const auto current =
                            nodePosition.find(stops[index].stopId);
                        if (previous != nodePosition.end() &&
                            current != nodePosition.end() &&
                            previous->second < current->second) {
                            ++ordered;
                        }
                    }
                    if (!hasBest ||
                        ordered > bestOrdered ||
                        (ordered == bestOrdered &&
                         overlap > bestOverlap)) {
                        hasBest = true;
                        bestShapeKey = shapeKey;
                        bestOrdered = ordered;
                        bestOverlap = overlap;
                    }
                }
                if (hasBest && bestOverlap > 0) {
                    shapeDirToWayDir[bestShapeKey] = wayKey;
                }
            }

            std::map<std::string, double> flowByWayLink;
            std::map<std::string, double> capacityByWayLink;
            std::map<std::string, std::set<std::string> > shapeIdsByWayLink;

            for (const auto& item : shapeDirToWayDir) {
                const RouteShapeKey& shapeKey = item.first;
                const RouteShapeKey& wayKey = item.second;
                const auto waysFound = wayGroups.find(wayKey);
                if (waysFound == wayGroups.end()) {
                    continue;
                }
                const double frequency =
                    std::max(0.0, shapeFrequencyByDir[shapeKey]);
                const double capacity =
                    frequency > 0.0
                        ? frequency * assumedVehicleCapacity
                        : assumedVehicleCapacity;
                const std::string shapeId = shapeIdByDir[shapeKey].empty()
                    ? std::get<0>(shapeKey) + "_dir" + std::get<1>(shapeKey)
                    : shapeIdByDir[shapeKey];
                for (const PTDbWayRecord& way : waysFound->second) {
                    capacityByWayLink[way.linkId] += capacity;
                    shapeIdsByWayLink[way.linkId].insert(shapeId);
                }
            }

            for (PTLink* link : linkVector) {
                if (link->GetTransitLinkType() != PTLink::ENROUTE ||
                    link->volume <= 0.0 ||
                    !link->tail ||
                    !link->head ||
                    !link->tail->m_stop ||
                    !link->head->m_stop ||
                    !link->m_shape ||
                    !link->m_shape->m_routePtr) {
                    continue;
                }

                const std::string routeId =
                    link->m_shape->m_routePtr->m_id;
                const std::string shapeDir =
                    parse_shape_dir(link->m_shape->m_id, routeId);
                const int shapeGroup =
                    parse_shape_group(link->m_shape->m_id, routeId);
                const RouteShapeKey shapeKey(routeId, shapeDir, shapeGroup);
                auto matchedWayKey = shapeDirToWayDir.find(shapeKey);
                if (matchedWayKey == shapeDirToWayDir.end()) {
                    const std::string fallbackDir =
                        encoded_route_dir(routeId);
                    const int fallbackGroup =
                        encoded_route_group(routeId);
                    if (!fallbackDir.empty()) {
                        matchedWayKey =
                            shapeDirToWayDir.find(
                                RouteShapeKey(
                                    routeId,
                                    fallbackDir,
                                    fallbackGroup));
                    }
                }
                if (matchedWayKey == shapeDirToWayDir.end()) {
                    continue;
                }
                const auto waysFound = wayGroups.find(matchedWayKey->second);
                if (waysFound == wayGroups.end() ||
                    waysFound->second.empty()) {
                    continue;
                }

                const std::vector<PTDbWayRecord>& ways =
                    waysFound->second;
                std::map<std::string, int> nodePosition;
                nodePosition[ways.front().initNode] = 0;
                for (std::size_t index = 0; index < ways.size(); ++index) {
                    nodePosition[ways[index].termNode] =
                        static_cast<int>(index) + 1;
                }

                const std::string fromStop = link->tail->m_stop->m_id;
                const std::string toStop = link->head->m_stop->m_id;
                const auto fromPosition = nodePosition.find(fromStop);
                const auto toPosition = nodePosition.find(toStop);
                if (fromPosition == nodePosition.end() ||
                    toPosition == nodePosition.end() ||
                    fromPosition->second >= toPosition->second) {
                    continue;
                }

                const int start = fromPosition->second;
                const int end = toPosition->second;
                const double flowShare =
                    link->volume / static_cast<double>(end - start);
                for (int index = start; index < end; ++index) {
                    const std::string& wayLinkId =
                        ways[static_cast<std::size_t>(index)].linkId;
                    flowByWayLink[wayLinkId] += flowShare;
                    shapeIdsByWayLink[wayLinkId].insert(link->m_shape->m_id);
                }
            }

            std::vector<PtWayUpdateRow> ptWayUpdates;
            ptWayUpdates.reserve(capacityByWayLink.size());
            for (const auto& item : capacityByWayLink) {
                const std::string& linkId = item.first;
                const double capacity = item.second;
                const double flow = flowByWayLink[linkId];
                const double vcRatio =
                    capacity > 0.0 ? flow / capacity : 0.0;
                std::ostringstream matchedShapes;
                const auto shapesFound = shapeIdsByWayLink.find(linkId);
                if (shapesFound != shapeIdsByWayLink.end()) {
                    bool first = true;
                    for (const std::string& shapeId : shapesFound->second) {
                        if (!first) {
                            matchedShapes << ';';
                        }
                        first = false;
                        matchedShapes << shapeId;
                    }
                }
                ptWayUpdates.push_back({
                    linkId,
                    flow,
                    capacity,
                    vcRatio,
                    vc_class(vcRatio),
                    matchedShapes.str(),
                    "matched"
                });
                ++ptWayVcRows;
                if (flow > flowPrecision) {
                    ++ptWayFlowRows;
                }
            }

            execute_command(
                connection.get(),
                "UPDATE " + ptWayTable +
                " SET volume = 0, v_c = 0, bottleneck = 0");
            execute_command(
                connection.get(),
                "CREATE TEMP TABLE tmp_pt_way_vc_update ("
                "link_id text, "
                "volume double precision, "
                "capacity double precision, "
                "v_c double precision, "
                "bottleneck integer) ON COMMIT DROP");
            start_copy(
                connection.get(),
                "COPY tmp_pt_way_vc_update "
                "(link_id, volume, capacity, v_c, bottleneck) "
                "FROM STDIN WITH (FORMAT text)");
            for (const PtWayUpdateRow& row : ptWayUpdates) {
                write_copy_row(
                    connection.get(),
                    {
                        copy_text_value(row.linkId),
                        copy_double_value(row.flow),
                        copy_double_value(row.capacity),
                        copy_double_value(row.vcRatio),
                        row.vcRatio >= 1.0 ? "1" : "0"
                    });
            }
            finish_copy(connection.get());
            execute_command(
                connection.get(),
                "UPDATE " + ptWayTable + " AS w SET "
                "capacity = u.capacity, "
                "volume = u.volume, "
                "v_c = u.v_c, "
                "bottleneck = u.bottleneck "
                "FROM tmp_pt_way_vc_update AS u "
                "WHERE w.link_id::text = u.link_id");
        }
        if (!dbWayRecords.empty()) {
            const int busWayTotalRows =
                static_cast<int>(dbWayRecords.size());
            busWayZeroFlowRows = busWayTotalRows > ptWayFlowRows
                ? busWayTotalRows - ptWayFlowRows
                : 0;
            busWayZeroFlowRatio =
                static_cast<double>(busWayZeroFlowRows) /
                static_cast<double>(busWayTotalRows);
        }

        const std::vector<std::pair<std::string, double> > summaryRows = {
            {"num_of_nodes", static_cast<double>(numOfNode)},
            {"num_of_links", static_cast<double>(numOfLink)},
            {"num_of_stops", static_cast<double>(m_stops.size())},
            {"num_of_routes", static_cast<double>(m_routes.size())},
            {"num_of_shapes", static_cast<double>(m_shapes.size())},
            {"num_of_walk_links", static_cast<double>(walkLinks)},
            {"num_of_od_pairs", static_cast<double>(numOfPTOD)},
            {"num_of_destinations", static_cast<double>(numOfPTDest)},
            {"num_of_hyperpaths", static_cast<double>(numHyperpaths)},
            {"num_of_stop_link_vc_rows", static_cast<double>(stopLinkVcRows.size())},
            {"num_of_pt_way_vc_rows", static_cast<double>(ptWayVcRows)},
            {"num_of_pt_way_flow_rows", static_cast<double>(ptWayFlowRows)},
            {"bus_way_zero_flow_link_ratio", busWayZeroFlowRatio},
            {"assumed_vehicle_capacity", assumedVehicleCapacity},
            {"total_demand", numOfPTTrips},
            {"assigned_demand", assignedDemand},
            {"infeasible_flow", infeasibleFlow},
            {"positive_flow_links", static_cast<double>(positiveFlowLinks)},
            {"total_passenger_link_flow", totalPassengerLinkFlow},
            {"total_system_cost", totalSystemCost},
            {
                "avg_path_cost",
                assignedDemand > 0.0
                    ? totalSystemCost / assignedDemand
                    : 0.0
            },
            {"total_wait_cost", totalWaitCost},
            {
                "avg_wait_cost",
                assignedDemand > 0.0
                    ? totalWaitCost / assignedDemand
                    : 0.0
            },
            {"cpu_time", cpuTimeSeconds}
        };

        start_copy(
            connection.get(),
            "COPY " + summaryTable +
            " (metric_key, metric_value) FROM STDIN WITH (FORMAT text)");
        for (const auto& summaryRow : summaryRows) {
            write_copy_row(
                connection.get(),
                {
                    copy_text_value(summaryRow.first),
                    copy_double_value(summaryRow.second)
                });
        }
        finish_copy(connection.get());

        execute_command(connection.get(), "COMMIT");
        transactionStarted = false;

        std::cout
            << "\tPostgreSQL results written successfully\n"
            << "\t  " << prefix << "pt_link_result: "
            << linkVector.size() << " rows\n"
            << "\t  " << prefix << "pt_path_result: "
            << numHyperpaths << " rows\n"
            << "\t  " << prefix << "pt_iter_result: 1 row\n"
            << "\t  " << prefix << "pt_summary_result: "
            << summaryRows.size() << " rows\n"
            << "\t  " << prefix << "pt_stop_link_vc_result: "
            << stopLinkVcRows.size() << " rows\n"
            << "\t  " << prefix << "pt_way vc updates: "
            << ptWayVcRows << " rows" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        if (transactionStarted && connection) {
            PGResultPtr rollback(PQexec(connection.get(), "ROLLBACK"));
        }
        lastErrorMessage = error.what();
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
