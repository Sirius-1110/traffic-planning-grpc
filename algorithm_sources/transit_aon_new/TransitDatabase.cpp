#include "PTNet.h"

#include <libpq-fe.h>

#include <algorithm>
#include <cctype>
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
};

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
            const std::string table = prefix + "pt_stop";
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT stop_id, stop_name FROM " + qualifiedPrefix +
                    quote_identifier(table) + " ORDER BY stop_id");
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
            std::string shapeIdExpression;
            std::string orderExpression;
            if (hasShapeId) {
                shapeIdExpression = "shape_id";
                orderExpression = "shape_id";
            } else if (hasDir) {
                shapeIdExpression =
                    "CASE WHEN route_id LIKE '%_dir%' "
                    "THEN route_id ELSE route_id || '_dir' || dir::text END";
                orderExpression =
                    "CASE WHEN route_id LIKE '%_dir%' "
                    "THEN route_id ELSE route_id || '_dir' || dir::text END";
            } else {
                shapeIdExpression = "route_id";
                orderExpression = "route_id";
            }
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT " + shapeIdExpression + " AS shape_id, route_id, frequency, "
                "stop_sequence, stop_id FROM " +
                    qualifiedPrefix + quote_identifier(table) +
                    " ORDER BY " + orderExpression + ", stop_sequence");
            for (int row = 0; row < PQntuples(result.get()); ++row) {
                dbShapeStopRecords.push_back({
                    required_text(result.get(), row, 0),
                    required_text(result.get(), row, 1),
                    required_double(result.get(), row, 2),
                    required_int(result.get(), row, 3),
                    required_text(result.get(), row, 4)
                });
            }
        }

        {
            const std::string table = prefix + "pt_transit";
            PGResultPtr result = execute_tuples(
                connection.get(),
                "SELECT shape_id, from_stop, to_stop, fft FROM " +
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
            const auto found = originalTransitFft.find(key);
            const double fft =
                found == originalTransitFft.end() ? 1.0 : found->second;
            if (found == originalTransitFft.end()) {
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
        const double defaultCapacityPerShape = 60.0;

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
            const double capacity = defaultCapacityPerShape * shapeCount;
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
                        "capacity = 60 passengers/hour/shape * shape_count")
                });
        }
        finish_copy(connection.get());

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
            {"default_stop_link_capacity_per_shape", defaultCapacityPerShape},
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
            << stopLinkVcRows.size() << " rows" << std::endl;
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
