#include "PTNet.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

std::string build_prefix(int projectId, int userId, int caseId) {
    return
        "project" + std::to_string(projectId) +
        "_user" + std::to_string(userId) +
        "_case" + std::to_string(caseId) + "_";
}

std::string strip_bom(std::string value) {
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
    return value;
}

std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (quoted) {
            if (ch == '"' &&
                index + 1 < line.size() &&
                line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else if (ch == '"') {
                quoted = false;
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

std::string required_text(
    const std::vector<std::string>& row,
    const std::map<std::string, int>& columns,
    const std::string& columnName,
    const std::string& tableName) {
    const auto found = columns.find(columnName);
    if (found == columns.end()) {
        throw std::runtime_error(
            tableName + " missing required column: " + columnName);
    }
    const int index = found->second;
    if (index < 0 || static_cast<std::size_t>(index) >= row.size()) {
        return "";
    }
    return row[index];
}

double required_double(
    const std::vector<std::string>& row,
    const std::map<std::string, int>& columns,
    const std::string& columnName,
    const std::string& tableName) {
    const std::string text = required_text(row, columns, columnName, tableName);
    std::size_t parsed = 0;
    const double value = std::stod(text, &parsed);
    if (parsed != text.size()) {
        throw std::runtime_error(
            tableName + "." + columnName + " is not numeric: " + text);
    }
    return value;
}

int required_int(
    const std::vector<std::string>& row,
    const std::map<std::string, int>& columns,
    const std::string& columnName,
    const std::string& tableName) {
    const std::string text = required_text(row, columns, columnName, tableName);
    std::size_t parsed = 0;
    const long value = std::stol(text, &parsed);
    if (parsed != text.size()) {
        throw std::runtime_error(
            tableName + "." + columnName + " is not integer: " + text);
    }
    return static_cast<int>(value);
}

std::vector<std::vector<std::string> > read_csv(
    const std::string& path,
    std::map<std::string, int>& columns) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Cannot open CSV input: " + path);
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("CSV input is empty: " + path);
    }

    std::vector<std::string> header = parse_csv_line(strip_bom(line));
    for (std::size_t index = 0; index < header.size(); ++index) {
        columns[header[index]] = static_cast<int>(index);
    }

    std::vector<std::vector<std::string> > rows;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(parse_csv_line(line));
    }
    return rows;
}

std::string csv_path(
    const std::string& inputDirectory,
    const std::string& fileName) {
    if (inputDirectory.empty()) {
        return fileName;
    }
    const char last = inputDirectory[inputDirectory.size() - 1];
    const std::string separator =
        (last == '/' || last == '\\') ? "" : "\\";
    return inputDirectory + separator + fileName;
}

bool file_exists(const std::string& path) {
    std::ifstream input(path);
    return input.is_open();
}

bool has_column(
    const std::map<std::string, int>& columns,
    const std::string& columnName) {
    return columns.find(columnName) != columns.end();
}

bool column_values_are_unique(
    const std::vector<std::vector<std::string> >& rows,
    const std::map<std::string, int>& columns,
    const std::string& columnName) {
    const auto found = columns.find(columnName);
    if (found == columns.end() || rows.empty()) {
        return false;
    }
    std::set<std::string> values;
    for (const auto& row : rows) {
        if (static_cast<std::size_t>(found->second) >= row.size()) {
            return false;
        }
        values.insert(row[found->second]);
    }
    return values.size() == rows.size();
}

std::string make_shape_key(
    const std::string& routeId,
    const std::string& dir,
    int group,
    bool routeIdIsShapeKey = false) {
    if (routeIdIsShapeKey) {
        return routeId;
    }
    std::string shapeId = routeId;
    if (!dir.empty() && routeId.find("_dir") == std::string::npos) {
        shapeId += "_dir" + dir;
    }
    if (group != 0) {
        shapeId += "_g" + std::to_string(group);
    }
    return shapeId;
}

bool is_digit_string(const std::string& value) {
    return !value.empty() &&
        std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            });
}

std::string encoded_route_dir(const std::string& routeId) {
    if (routeId.size() < 3 || !is_digit_string(routeId)) {
        return "";
    }
    return routeId.substr(routeId.size() - 2, 1);
}

int encoded_route_group(const std::string& routeId) {
    if (routeId.size() < 3 || !is_digit_string(routeId)) {
        return 0;
    }
    return routeId.back() - '0';
}

std::string resolve_csv_path(
    const std::string& inputDirectory,
    const std::string& prefix,
    const std::string& suffix,
    bool required = true) {
    const std::string prefixed =
        csv_path(inputDirectory, prefix + suffix + ".csv");
    if (file_exists(prefixed)) {
        return prefixed;
    }

    const std::string plain =
        csv_path(inputDirectory, suffix + ".csv");
    if (file_exists(plain)) {
        return plain;
    }

    if (required) {
        throw std::runtime_error(
            "Cannot find CSV input: " + prefixed + " or " + plain);
    }
    return "";
}

}  // namespace

int PTNET::ReadCsvInputs(
    const string& inputDirectory,
    int projectId,
    int userId,
    int caseId) {
    dbRouteRecords.clear();
    dbStopRecords.clear();
    dbShapeStopRecords.clear();
    dbTransitRecords.clear();
    dbWalkRecords.clear();
    dbTripRecords.clear();
    dbWayRecords.clear();
    lastErrorMessage.clear();

    try {
        const std::string prefix = build_prefix(projectId, userId, caseId);

        {
            const std::string table = "pt_route";
            std::map<std::string, int> columns;
            const auto rows =
                read_csv(
                    resolve_csv_path(inputDirectory, prefix, table),
                    columns);
            for (const auto& row : rows) {
                dbRouteRecords.push_back({
                    required_text(row, columns, "route_id", table)
                });
            }
        }

        {
            const std::string table = "pt_stop";
            std::map<std::string, int> columns;
            const auto rows =
                read_csv(
                    resolve_csv_path(inputDirectory, prefix, table),
                    columns);
            for (const auto& row : rows) {
                dbStopRecords.push_back({
                    required_text(row, columns, "stop_id", table),
                    required_text(row, columns, "stop_name", table)
                });
            }
        }

        {
            const std::string table = "pt_shape";
            std::map<std::string, int> columns;
            const auto rows =
                read_csv(
                    resolve_csv_path(inputDirectory, prefix, table),
                    columns);
            const bool shapeIdIsRowId =
                has_column(columns, "shape_id") &&
                column_values_are_unique(rows, columns, "shape_id");
            std::map<std::string, int> currentGroupByRouteDir;
            std::set<std::string> seenRouteDir;
            for (const auto& row : rows) {
                const std::string routeId =
                    required_text(row, columns, "route_id", table);
                const std::string dir =
                    has_column(columns, "dir")
                        ? required_text(row, columns, "dir", table)
                        : "";
                int group = 0;
                if (has_column(columns, "group")) {
                    group = required_int(row, columns, "group", table);
                } else if (has_column(columns, "b") ||
                           has_column(columns, "start_st")) {
                    const std::string routeDirKey = routeId + "\t" + dir;
                    const std::string flagColumn =
                        has_column(columns, "b") ? "b" : "start_st";
                    const int startFlag =
                        required_int(row, columns, flagColumn, table);
                    if (seenRouteDir.insert(routeDirKey).second) {
                        currentGroupByRouteDir[routeDirKey] = 0;
                    } else if (startFlag == 1) {
                        ++currentGroupByRouteDir[routeDirKey];
                    }
                    group = currentGroupByRouteDir[routeDirKey];
                }
                if (!has_column(columns, "group")) {
                    const std::string encodedDir = encoded_route_dir(routeId);
                    if (!encodedDir.empty() &&
                        (dir.empty() || encodedDir == dir)) {
                        group = encoded_route_group(routeId);
                    }
                }

                std::string shapeId;
                if (has_column(columns, "shape_id") && !shapeIdIsRowId) {
                    shapeId = required_text(row, columns, "shape_id", table);
                } else {
                    shapeId = make_shape_key(routeId, dir, group, shapeIdIsRowId);
                }
                dbShapeStopRecords.push_back({
                    shapeId,
                    routeId,
                    dir,
                    group,
                    required_double(row, columns, "frequency", table),
                    required_int(row, columns, "stop_sequence", table),
                    required_text(row, columns, "stop_id", table)
                });
            }
        }

        {
            const std::string table = "pt_transit";
            std::map<std::string, int> columns;
            const auto rows =
                read_csv(
                    resolve_csv_path(inputDirectory, prefix, table),
                    columns);
            const std::string transitKeyColumn =
                has_column(columns, "shape_id") ? "shape_id" : "route_id";
            for (const auto& row : rows) {
                dbTransitRecords.push_back({
                    required_text(row, columns, transitKeyColumn, table),
                    required_text(row, columns, "from_stop", table),
                    required_text(row, columns, "to_stop", table),
                    required_double(row, columns, "fft", table)
                });
            }
        }

        {
            const std::string table = "pt_walk";
            const std::string path =
                resolve_csv_path(inputDirectory, prefix, table, false);
            if (path.empty()) {
                // pt_walk is optional for this AON helper.
            } else {
                std::map<std::string, int> columns;
                const auto rows = read_csv(path, columns);
                for (const auto& row : rows) {
                    dbWalkRecords.push_back({
                        required_text(row, columns, "from_stop", table),
                        required_text(row, columns, "to_stop", table),
                        required_double(row, columns, "fft", table),
                        required_double(row, columns, "length", table)
                    });
                }
            }
        }

        {
            const std::string table = "pt_way";
            std::map<std::string, int> columns;
            const auto rows =
                read_csv(
                    resolve_csv_path(inputDirectory, prefix, table),
                    columns);
            std::map<std::string, int> currentGroupByRouteDir;
            std::set<std::string> seenRouteDir;
            for (const auto& row : rows) {
                const std::string routeId =
                    required_text(row, columns, "route_ref", table);
                const std::string dir =
                    required_text(row, columns, "direction", table);
                const std::string routeDirKey = routeId + "\t" + dir;
                const int startFlag = required_int(row, columns, "b", table);
                if (seenRouteDir.insert(routeDirKey).second) {
                    currentGroupByRouteDir[routeDirKey] = 0;
                } else if (startFlag == 1) {
                    ++currentGroupByRouteDir[routeDirKey];
                }

                dbWayRecords.push_back({
                    required_text(row, columns, "link_id", table),
                    routeId,
                    dir,
                    currentGroupByRouteDir[routeDirKey],
                    required_int(row, columns, "power", table),
                    required_text(row, columns, "init_node", table),
                    required_text(row, columns, "term_node", table)
                });
            }
        }

        {
            const std::string table = "pt_trip";
            std::map<std::string, int> columns;
            const auto rows =
                read_csv(
                    resolve_csv_path(inputDirectory, prefix, table),
                    columns);
            for (const auto& row : rows) {
                dbTripRecords.push_back({
                    required_text(row, columns, "origin_stop", table),
                    required_text(row, columns, "destination_stop", table),
                    required_double(row, columns, "demand", table)
                });
            }
        }

        return 0;
    } catch (const std::exception& error) {
        lastErrorMessage = error.what();
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
