#include "..\TNA\header\stdafx.h"
#include <algorithm>
#include <math.h>
#include <cmath>
#include <iostream>
#include <tchar.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <string>
#include <map>
#include <cctype>
#include <vector>
#include <ctime>
#include <cstdlib>
// #include <regex> // C++11 feature, removed for compatibility
#include "TNADriver.h"
#include "..\\TNA\\header\\TNM_Algorithm.h"
#include "../../../Include/postgresql/libpq-fe.h"
// #include "../../TNA/TNA/header/TNM_Net.h"
#include "..\\TNA\\header\\TNM_Net.h"
#include <windows.h>    // 若本文件已间接包含则可省略
extern "C" IMAGE_DOS_HEADER __ImageBase;
using namespace std;

// 辅助函数：去除字符串前后空格
static inline bool is_not_space(int ch) {
    return !std::isspace(ch);
}

// Forward declarations to allow use before definition
static string extract_json_object_value(const string& json, const string& key);
static inline void trim_inplace(string& s);
static inline string json_escape(const string& s);
static string ReadFileToString(const string& filename);
static std::vector<std::string> ParseCsvLine(const std::string& line);
static string extract_nested_json_string_value(const string& json, const string& parent_key, const string& child_key);
static double extract_nested_json_double_value(const string& json, const string& parent_key, const string& child_key);
static int    extract_nested_json_int_value(const string& json, const string& parent_key, const string& child_key);
static double extract_json_double_value(const string& json, const string& key);
static int    extract_json_int_value(const string& json, const string& key);

static inline string json_quote(const string& s) {
    return string("\"") + json_escape(s) + "\"";
}

// Build bilingual params JSON: include DB config, tables, scenario, and algorithm params
static string BuildBilingualParamsJSON(const string& jsonConfig,
                                       const string& role,
                                       const string& fullNet,
                                       const string& fullOD,
                                       const string& scenarioPrefix)
{
    // db
    string host = extract_nested_json_string_value(jsonConfig, "db_conn_str", "host");
    string dbname = extract_nested_json_string_value(jsonConfig, "db_conn_str", "dbname");
    string user = extract_nested_json_string_value(jsonConfig, "db_conn_str", "user");
    string password = extract_nested_json_string_value(jsonConfig, "db_conn_str", "password");
    string role2 = extract_nested_json_string_value(jsonConfig, "db_conn_str", "role");
    if (!role.empty()) role2 = role; // prefer resolved role
    string portS = extract_nested_json_string_value(jsonConfig, "db_conn_str", "port");
    if (!portS.empty()) {
        bool digits_only = true; for (size_t i=0;i<portS.size();++i){ if (!isdigit((unsigned char)portS[i])) { digits_only=false; break; } }
        if (!digits_only) portS.clear();
    }
    if (portS.empty()) {
        int pi = extract_nested_json_int_value(jsonConfig, "db_conn_str", "port");
        if (pi > 0) portS = std::to_string(pi);
    }
    // alg
    double conv = extract_nested_json_double_value(jsonConfig, "algorithm_params", "conv");
    int max_iter = extract_nested_json_int_value(jsonConfig, "algorithm_params", "max_iter");
    string lpf = extract_nested_json_string_value(jsonConfig, "algorithm_params", "lpf");
    double cost_scalar = extract_nested_json_double_value(jsonConfig, "algorithm_params", "cost_scalar");
    double cost_coef_time = extract_nested_json_double_value(jsonConfig, "algorithm_params", "cost_coef_time");

    // Build bilingual object
    string j = "{";
    bool first = true;
    auto addCommaIfNeeded = [&](){ if (!first) j += ","; };
    auto addKV = [&](const string& k_en, const string& k_zh, const string& v){
        addCommaIfNeeded();
        j += json_quote(k_en) + ":" + json_quote(v) + "," + json_quote(k_zh) + ":" + json_quote(v);
        first = false;
    };
    auto addKVD = [&](const string& k_en, const string& k_zh, double v){
        addCommaIfNeeded();
        string vs = std::to_string(v);
        j += json_quote(k_en) + ":" + vs + "," + json_quote(k_zh) + ":" + vs;
        first = false;
    };
    auto addKVI = [&](const string& k_en, const string& k_zh, int v){
        addCommaIfNeeded();
        string vs = std::to_string(v);
        j += json_quote(k_en) + ":" + vs + "," + json_quote(k_zh) + ":" + vs;
        first = false;
    };

    if (!host.empty()) addKV("host", u8"\u4E3B\u673A", host);
    if (!portS.empty()) addKV("port", u8"\u7AEF\u53E3", portS);
    if (!dbname.empty()) addKV("dbname", u8"\u6570\u636E\u5E93", dbname);
    if (!user.empty()) addKV("user", u8"\u7528\u6237", user);
    if (!password.empty()) addKV("password", u8"\u5BC6\u7801", password);
    if (!role2.empty()) addKV("role", u8"\u89D2\u8272", role2);
    if (!fullNet.empty()) addKV("network_table", u8"\u7F51\u7EDC\u8868", fullNet);
    if (!fullOD.empty()) addKV("od_table", u8"OD\u8868", fullOD);
    if (!scenarioPrefix.empty()) addKV("scenario_prefix", u8"\u573A\u666F\u524D\u7F00", scenarioPrefix);
    if (conv > 0) addKVD("conv", u8"\u6536\u655B\u9608\u503C", conv);
    if (max_iter > 0) addKVI("max_iter", u8"\u6700\u5927\u8FED\u4EE3", max_iter);
    if (!lpf.empty()) addKV("lpf", u8"\u6027\u80FD\u51FD\u6570", lpf);
    if (cost_scalar > 0) addKVD("cost_scalar", u8"\u6210\u672C\u7F29\u653E", cost_scalar);
    if (cost_coef_time > 0) addKVD("cost_coef_time", u8"\u65F6\u95F4\u7CFB\u6570", cost_coef_time);
    j += "}";
    return j;
}

// Build bilingual summary JSON: include avg_vc (if available), iter, oversaturated count (if available)
static string BuildBilingualSummaryJSON(const string& scenarioPrefix, int iter)
{
    // locate summary file to probe extra metrics
    HMODULE __hMod = reinterpret_cast<HMODULE>(&__ImageBase);
    char __dllPath[MAX_PATH]; std::string __dllDir; DWORD __n = GetModuleFileNameA(__hMod, __dllPath, MAX_PATH);
    if (__n > 0) {
        std::string __full(__dllPath, __dllPath + __n);
        size_t __p = __full.find_last_of("\\/");
        __dllDir = (__p == std::string::npos ? std::string(".") : __full.substr(0, __p));
    }
    string s = scenarioPrefix; string t = s; trim_inplace(t); while(!t.empty() && t.back()=='_') t.pop_back();
    string summary_file = (!__dllDir.empty()? (__dllDir + "\\") : std::string("")) + t + "_saturation_summary.json";
    string summary_json_str = ReadFileToString(summary_file);
    // Prefer nested object under key 'summary'
    string sum_obj = extract_json_object_value(summary_json_str, "summary");
    const string& src = sum_obj.empty() ? summary_json_str : sum_obj;

    double avg_vc = extract_json_double_value(src, "avg_vc");
    if (avg_vc <= 0) avg_vc = extract_json_double_value(src, "avg_saturation");
    if (avg_vc <= 0) avg_vc = extract_json_double_value(src, u8"\u5E73\u5747v/c"); // 平均v/c

    int over_cnt = extract_json_int_value(src, "oversaturated_links");
    if (over_cnt <= 0) over_cnt = extract_json_int_value(src, "oversaturated_count");
    if (over_cnt <= 0) over_cnt = extract_json_int_value(src, u8"\u8FC7\u9971\u548C\u8DEF\u6BB5\u6570"); // 过饱和路段数

    // Fallback: compute from histogram CSV if still missing
    if (avg_vc <= 0 || over_cnt <= 0) {
        string histogram_file = (!__dllDir.empty()? (__dllDir + "\\") : std::string("")) + t + "_saturation_histogram.csv";
        string hist_csv = ReadFileToString(histogram_file);
        if (!hist_csv.empty()) {
            std::stringstream ss(hist_csv);
            string line; bool header_read=false; int idx_left=-1, idx_right=-1, idx_count=-1;
            long long tot_cnt = 0; double sum_mid = 0.0; long long over_sum = 0;
            while (std::getline(ss, line)) {
                trim_inplace(line); if (line.empty()) continue;
                vector<string> cols = ParseCsvLine(line);
                if (!header_read) {
                    // scenario_id,bin_label,bin_left,bin_right,count,ratio
                    for (size_t i=0;i<cols.size();++i) {
                        string h = cols[i];
                        if (h == "bin_left") idx_left = (int)i;
                        else if (h == "bin_right") idx_right = (int)i;
                        else if (h == "count") idx_count = (int)i;
                    }
                    if (idx_left<0 || idx_right<0 || idx_count<0) {
                        // assume default positions
                        idx_left = 2; idx_right = 3; idx_count = 4;
                    }
                    header_read = true; continue;
                }
                auto to_d = [&](const string& s)->double{ char* p=nullptr; double v=strtod(s.c_str(), &p); return v; };
                auto to_i = [&](const string& s)->long long{ char* p=nullptr; long long v=strtoll(s.c_str(), &p, 10); return v; };
                if ((int)cols.size() <= std::max({idx_left, idx_right, idx_count})) continue;
                double bl = to_d(cols[idx_left]);
                double br = to_d(cols[idx_right]);
                long long c = to_i(cols[idx_count]);
                if (c < 0) continue;
                // oversaturated threshold: left bound >= 1.0
                if (bl >= 1.0) over_sum += c;
                // midpoint; cap extremely large bin_right (e.g., 999) to bl+0.2 for averaging
                double r = br; if (!std::isfinite(r) || r > bl + 10) r = bl + 0.2;
                double mid = (bl + r) * 0.5;
                tot_cnt += c; sum_mid += mid * (double)c;
            }
            if (avg_vc <= 0 && tot_cnt > 0) avg_vc = sum_mid / (double)tot_cnt;
            if (over_cnt <= 0 && over_sum > 0) over_cnt = (int)over_sum;
        }
    }

    string j = "{";
    // iter (both)
    j += json_quote("iter") + ":" + std::to_string(iter) + "," + json_quote(u8"\u8FED\u4EE3\u6B21\u6570") + ":" + std::to_string(iter);
    // avg_vc (if any)
    if (avg_vc > 0) {
        j += "," + json_quote("avg_vc") + ":" + std::to_string(avg_vc) + "," + json_quote(u8"\u5E73\u5747v/c") + ":" + std::to_string(avg_vc);
    }
    // oversaturated count (if any)
    if (over_cnt > 0) {
        j += "," + json_quote("oversaturated_links") + ":" + std::to_string(over_cnt) + "," + json_quote(u8"\u8FC7\u9971\u548C\u8DEF\u6BB5\u6570") + ":" + std::to_string(over_cnt);
    }
    j += "}";
    return j;
}


static inline void trim_inplace(string& s) {
    if (s.empty()) return;
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), is_not_space));
    if (!s.empty()) {
        s.erase(std::find_if(s.rbegin(), s.rend(), is_not_space).base(), s.end());
    }
}

// 辅助函数：转换为大写
static inline string to_upper(string s) {
    for (string::iterator it = s.begin(); it != s.end(); ++it) {
        *it = (char)std::toupper((unsigned char)*it);
    }
    return s;
}

// 辅助函数：对 JSON 字符串做最基本的转义（仅用于 message 等文本字段）
static inline string json_escape(const string& s) {
    string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                // 以 \u00XX 输出控制字符
                sprintf(buf, "\\u%04X", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

// Helper function to read a file into a string
static string ReadFileToString(const string& filename) {
    ifstream file(filename.c_str());
    if (!file.is_open()) {
        return "";
    }
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Helper function to convert CSV string to a JSON array of objects
static vector<string> ParseCsvLine(const string& line) {
    vector<string> result;
    string cell;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            result.push_back(cell);
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    result.push_back(cell);
    for (size_t i = 0; i < result.size(); ++i) {
        string &s = result[i];
        trim_inplace(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
            string t; t.reserve(s.size());
            for (size_t k = 0; k < s.size(); ++k) {
                if (s[k] == '"' && k + 1 < s.size() && s[k + 1] == '"') { t.push_back('"'); ++k; }
                else { t.push_back(s[k]); }
            }
            s.swap(t);
        }
    }
    return result;
}

static string CsvToJson(const string& csv_data) {
    if (csv_data.empty()) {
        return "[]";
    }

    stringstream ss(csv_data);
    string line;
    vector<string> headers;
    bool header_read = false;
    string json_array = "[";

    while (getline(ss, line)) {
        trim_inplace(line);
        if (line.empty()) continue;
        vector<string> values = ParseCsvLine(line);

        if (!header_read) {
            headers = values;
            header_read = true;
        } else {
            if (!json_array.empty() && json_array != "[") {
                json_array += ",";
            }
            json_array += "{";
            for (size_t i = 0; i < headers.size(); ++i) {
                if (i > 0) {
                    json_array += ",";
                }
                json_array += "\"" + json_escape(headers[i]) + "\":";
                if (i < values.size()) {
                    // Attempt to parse as a number, otherwise treat as a string
                    char* p;
                    strtod(values[i].c_str(), &p);
                    if (*p == 0) { // It's a number
                        json_array += values[i];
                    } else { // It's a string
                        json_array += "\"" + json_escape(values[i]) + "\"";
                    }
                } else {
                    json_array += "null";
                }
            }
            json_array += "}";
        }
    }

    json_array += "]";
    return json_array;
}


// Reads the output files generated by WritePG and formats them into a single JSON string.
static string ReadOutputFilesToJson(const string& scenarioPrefix) {
    if (scenarioPrefix.empty()) {
        return "{}";
    }
    string s = scenarioPrefix; trim_inplace(s); while(!s.empty() && s.back()=='_') s.pop_back();
    string histogram_file = s + "_saturation_histogram.csv";
    string congested_links_file = s + "_top20_congested_links.csv";
    string summary_file = s + "_saturation_summary.json";
    
    HMODULE __hMod = reinterpret_cast<HMODULE>(&__ImageBase);
    char __dllPath[MAX_PATH];
    std::string __dllDir;
    DWORD __n = GetModuleFileNameA(__hMod, __dllPath, MAX_PATH);
    if (__n > 0) {
        std::string __full(__dllPath, __dllPath + __n);
        size_t __p = __full.find_last_of("\\/");
        __dllDir = (__p == std::string::npos ? std::string(".") : __full.substr(0, __p));
    }
    if (!__dllDir.empty()) {
        histogram_file = __dllDir + "\\" + histogram_file;
        congested_links_file = __dllDir + "\\" + congested_links_file;
        summary_file = __dllDir + "\\" + summary_file;
    }

    string histogram_csv = ReadFileToString(histogram_file);
    string congested_links_csv = ReadFileToString(congested_links_file);
    string summary_json_str = ReadFileToString(summary_file);

    string histogram_json = CsvToJson(histogram_csv);
    string congested_links_json = CsvToJson(congested_links_csv);

    if (summary_json_str.empty()) {
        summary_json_str = "{}";
    }

    // Try to extract nested objects 'summary' and 'params' from saturation_summary
    string summary_obj = extract_json_object_value(summary_json_str, "summary");
    string params_obj  = extract_json_object_value(summary_json_str, "params");

    string data_json = "{";
    data_json += "\"saturation_histogram\": " + histogram_json + ",";
    data_json += "\"top20_congested_links\": " + congested_links_json + ",";
    if (!summary_obj.empty()) {
        data_json += "\"summary\": " + summary_obj + ",";
    }
    if (!params_obj.empty()) {
        data_json += "\"params\": " + params_obj + ",";
    }
    data_json += "\"saturation_summary\": " + summary_json_str;
    data_json += "}";

    return data_json;
}

// 构造统一的 REST 返回：{"status":<int>,"message":"<text>","data":<json或"">}
static inline string BuildJsonResponse(int status, const string& message, const string& dataJson) {
    string msg = message;
    string notes = TNM_GetMessageNotes();
    trim_inplace(notes);
    if (!notes.empty()) { if (!msg.empty()) msg.append("\n"); msg.append(notes); }
    string resp = "{";
    resp += "\"status\":"; resp += std::to_string(status); resp += ",";
    resp += "\"message\":\""; resp += json_escape(msg); resp += "\",";
    resp += "\"data\":";
    if (dataJson.empty()) {
        resp += "\"\""; // 空字符串
    } else {
        resp += dataJson;   // 直接拼入 JSON 片段
    }
    resp += "}";
    return resp;
}

static int SafeSolveNoUnwind(TAP_Greedy_dijk* ofw, unsigned long* seh_code_out) {
    int r = 0;
    unsigned long code = 0;
    __try {
        int not_converged = (ofw->Solve() != ConvergeTerm);
        r = not_converged ? 2 : 1;
    }
    __except((code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
        r = -1;
    }
    if (seh_code_out) { *seh_code_out = code; }
    return r;
}

// 辅助函数：解析布尔值
// 注意：当前TestGreedy_dijkWithPostgreSQL接口不需要布尔类型参数，此函数为将来扩展保留
static inline bool parse_bool(const string& v, bool* ok = NULL) {
    string s = to_upper(v);
    if (s == "1" || s == "TRUE" || s == "YES" || s == "ON")  { if (ok) *ok = true; return true; }
    if (s == "0" || s == "FALSE"|| s == "NO"  || s == "OFF") { if (ok) *ok = false; return false; }
    if (ok) *ok = false; return false;
}

// Simple JSON parsing functions using traditional string operations
static string extract_json_string_value(const string& json, const string& key) {
    if (json.empty() || key.empty()) return "";
    
    string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return "";
    
    pos = json.find(":", pos);
    if (pos == string::npos) return "";
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length()) return "";
    
    pos = json.find("\"", pos);
    if (pos == string::npos) return "";
    pos++; // Skip opening quote
    
    if (pos >= json.length()) return "";
    
    size_t end_pos = json.find("\"", pos);
    if (end_pos == string::npos) return "";
    
    if (end_pos <= pos) return "";
    
    return json.substr(pos, end_pos - pos);
}

static double extract_json_double_value(const string& json, const string& key) {
    if (json.empty() || key.empty()) return 0.0;
    
    string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return 0.0;
    
    pos = json.find(":", pos);
    if (pos == string::npos) return 0.0;
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length()) return 0.0;
    
    size_t end_pos = pos;
    while (end_pos < json.length() && (isdigit(json[end_pos]) || json[end_pos] == '.' || 
           json[end_pos] == '-' || json[end_pos] == '+' || json[end_pos] == 'e' || json[end_pos] == 'E')) {
        end_pos++;
    }
    
    if (end_pos > pos) {
        string value_str = json.substr(pos, end_pos - pos);
        return atof(value_str.c_str());
    }
    return 0.0;
}

static int extract_json_int_value(const string& json, const string& key) {
    if (json.empty() || key.empty()) return 0;
    
    string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return 0;
    
    pos = json.find(":", pos);
    if (pos == string::npos) return 0;
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length()) return 0;
    
    size_t end_pos = pos;
    while (end_pos < json.length() && isdigit(json[end_pos])) {
        end_pos++;
    }
    
    if (end_pos > pos) {
        string value_str = json.substr(pos, end_pos - pos);
        return atoi(value_str.c_str());
    }
    return 0;
}

static bool extract_json_bool_value(const string& json, const string& key) {
    if (json.empty() || key.empty()) return false;
    
    string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return false;
    
    pos = json.find(":", pos);
    if (pos == string::npos) return false;
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length()) return false;
    
    if (pos + 4 <= json.length() && json.substr(pos, 4) == "true") return true;
    if (pos + 5 <= json.length() && json.substr(pos, 5) == "false") return false;
    return false;
}

// Extract a JSON object value for a given key at the current level, returning the raw substring (including braces)
static string extract_json_object_value(const string& json, const string& key) {
    if (json.empty() || key.empty()) return "";
    string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return "";
    pos = json.find(":", pos);
    if (pos == string::npos) return "";
    pos++;
    while (pos < json.length() && isspace((unsigned char)json[pos])) pos++;
    if (pos >= json.length() || json[pos] != '{') return "";
    int brace_count = 0;
    size_t start = pos;
    while (pos < json.length()) {
        char c = json[pos];
        if (c == '{') brace_count++;
        else if (c == '}') {
            brace_count--;
            if (brace_count == 0) {
                // include the closing brace
                return json.substr(start, pos - start + 1);
            }
        }
        // naive string skipping to avoid braces inside strings
        if (c == '"') {
            pos++;
            while (pos < json.length()) {
                if (json[pos] == '\\') { pos += 2; continue; }
                if (json[pos] == '"') { break; }
                pos++;
            }
        }
        pos++;
    }
    return "";
}

// Extract nested object values (for algorithm_params)
static string extract_nested_json_string_value(const string& json, const string& parent_key, const string& child_key) {
    if (json.empty() || parent_key.empty() || child_key.empty()) return "";
    
    string search_key = "\"" + parent_key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return "";
    
    pos = json.find(":", pos);
    if (pos == string::npos) return "";
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length() || json[pos] != '{') return "";
    pos++; // Skip opening brace
    
    // Find matching closing brace
    int brace_count = 1;
    size_t start_pos = pos;
    while (pos < json.length() && brace_count > 0) {
        if (json[pos] == '{') brace_count++;
        else if (json[pos] == '}') brace_count--;
        pos++;
    }
    
    if (brace_count == 0 && pos > start_pos) {
        string nested_obj = json.substr(start_pos, pos - start_pos - 1);
        return extract_json_string_value(nested_obj, child_key);
    }
    return "";
}

static double extract_nested_json_double_value(const string& json, const string& parent_key, const string& child_key) {
    if (json.empty() || parent_key.empty() || child_key.empty()) return 0.0;
    
    string search_key = "\"" + parent_key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return 0.0;
    
    pos = json.find(":", pos);
    if (pos == string::npos) return 0.0;
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length() || json[pos] != '{') return 0.0;
    pos++; // Skip opening brace
    
    // Find matching closing brace
    int brace_count = 1;
    size_t start_pos = pos;
    while (pos < json.length() && brace_count > 0) {
        if (json[pos] == '{') brace_count++;
        else if (json[pos] == '}') brace_count--;
        pos++;
    }
    
    if (brace_count == 0 && pos > start_pos) {
        string nested_obj = json.substr(start_pos, pos - start_pos - 1);
        return extract_json_double_value(nested_obj, child_key);
    }
    return 0.0;
}

static int extract_nested_json_int_value(const string& json, const string& parent_key, const string& child_key) {
    if (json.empty() || parent_key.empty() || child_key.empty()) return 0;
    
    string search_key = "\"" + parent_key + "\"";
    size_t pos = json.find(search_key);
    if (pos == string::npos) return 0;
    
    pos = json.find(":", pos);
    if (pos == string::npos) return 0;
    pos++; // Skip colon
    
    // Skip whitespace
    while (pos < json.length() && isspace(json[pos])) pos++;
    
    if (pos >= json.length() || json[pos] != '{') return 0;
    pos++; // Skip opening brace
    
    // Find matching closing brace
    int brace_count = 1;
    size_t start_pos = pos;
    while (pos < json.length() && brace_count > 0) {
        if (json[pos] == '{') brace_count++;
        else if (json[pos] == '}') brace_count--;
        pos++;
    }
    
    if (brace_count == 0 && pos > start_pos) {
        string nested_obj = json.substr(start_pos, pos - start_pos - 1);
        return extract_json_int_value(nested_obj, child_key);
    }
    return 0;
}

/*
 * Parse JSON configuration and apply parameters to TAP_Greedy_dijk object
 * Supports the following JSON structure:
 * {
 *   "db_conn_str": {
 *     "host": "127.0.0.1",
 *     "port": "54321",
 *     "dbname": "test2",
 *     "user": "postgres",
 *     "password": "configured-outside-source-control",
 *     "scenario_prefix": "morning_peak_scenario"
 *   },
 *   "algorithm_params": {
 *     "conv": 1e-06,
 *     "max_iter": 500,
 *     "lpf": "TNM_LINKTYPE: BPRLK",
 *     "cost_scalar": 60.0,
 *     "cost_coef_time": 1
 *   },
 */
static void ApplyConfigFromJSON(TAP_Greedy_dijk* ofw, const string& jsonConfig, string& dbConnStr, string& scenarioPrefix) {
    if (jsonConfig.empty()) {
        cout << "[INFO] JSON configuration is empty, using default values." << endl;
        return;
    }
    
    cout << "[INFO] Parsing JSON configuration..." << endl;
    
    // Extract database connection parameters from nested db_conn_str object
    string host = extract_nested_json_string_value(jsonConfig, "db_conn_str", "host");
    string port = extract_nested_json_string_value(jsonConfig, "db_conn_str", "port");
    if (!port.empty()) {
        bool digits_only = true;
        for (size_t i = 0; i < port.size(); ++i) { if (!isdigit((unsigned char)port[i])) { digits_only = false; break; } }
        if (!digits_only) { port.clear(); }
    }
    if (port.empty()) {
        int port_int = extract_nested_json_int_value(jsonConfig, "db_conn_str", "port");
        if (port_int > 0) { port = std::to_string(port_int); }
    }
    string dbname = extract_nested_json_string_value(jsonConfig, "db_conn_str", "dbname");
    string user = extract_nested_json_string_value(jsonConfig, "db_conn_str", "user");
    string password = extract_nested_json_string_value(jsonConfig, "db_conn_str", "password");
    string role = extract_nested_json_string_value(jsonConfig, "db_conn_str", "role");
    
    // Set default values if not provided
    if (host.empty()) host = "127.0.0.1";
    if (port.empty()) port = "54321";
    if (dbname.empty()) dbname = "test2";
    if (user.empty()) user = "postgres";
    
    // Construct database connection string
    dbConnStr = "host=" + host + " port=" + port + " dbname=" + dbname + " user=" + user + " password=" + password;
    if (!role.empty()) {
        dbConnStr += " options='-c role=" + role + "'";
        cout << "Database connection string constructed with role via options parameter." << endl;
    } else {
        cout << "Database connection string constructed from JSON parameters (no role)." << endl;
    }
    
    // Extract scenario prefix from db_conn_str
    scenarioPrefix = extract_nested_json_string_value(jsonConfig, "db_conn_str", "scenario_prefix");
    if (!scenarioPrefix.empty()) {
        cout << "Scenario prefix extracted: " << scenarioPrefix << endl;
    }
    
    // Extract algorithm parameters with default values
    double conv = extract_nested_json_double_value(jsonConfig, "algorithm_params", "conv");
    if (conv > 0) {
        ofw->SetConv(conv);
        cout << "Convergence criterion set to: " << conv << endl;
    }
    
    int max_iter = extract_nested_json_int_value(jsonConfig, "algorithm_params", "max_iter");
    if (max_iter > 0) {
        ofw->SetMaxIter(max_iter);
        cout << "Max iterations set to: " << max_iter << endl;
    }
    
    string lpf = extract_nested_json_string_value(jsonConfig, "algorithm_params", "lpf");
    if (!lpf.empty()) {
        string lpf_upper = to_upper(lpf);
        if (lpf_upper.find("BPRLK") != string::npos || lpf_upper.find("BPR") != string::npos) {
            ofw->SetLPF(BPRLK);
            cout << "Link performance function set to: BPRLK" << endl;
        }
    }
    
    double cost_scalar = extract_nested_json_double_value(jsonConfig, "algorithm_params", "cost_scalar");
    if (cost_scalar > 0) {
        ofw->SetCostScalar(cost_scalar);
        cout << "[INFO] Cost scalar set to: " << cost_scalar << endl;
    }
    
    double cost_coef_time = extract_nested_json_double_value(jsonConfig, "algorithm_params", "cost_coef_time");
    if (cost_coef_time > 0) {
        ofw->SetCostCoef(cost_coef_time, 0.0); // Distance coefficient set to 0
        cout << "[INFO] Cost coefficient (time) set to: " << cost_coef_time << endl;
    }
    
    // Extract report parameters with default values
    bool report_iter_history = extract_json_bool_value(jsonConfig, "report_iter_history");
    ofw->reportIterHistory = report_iter_history;
    cout << "[INFO] Report iteration history set to: " << (report_iter_history ? "true" : "false") << endl;
    
    bool report_link_detail = extract_json_bool_value(jsonConfig, "report_link_detail");
    ofw->reportLinkDetail = report_link_detail;
    cout << "[INFO] Report link detail set to: " << (report_link_detail ? "true" : "false") << endl;
    
    bool report_path_detail = extract_json_bool_value(jsonConfig, "report_path_detail");
    ofw->reportPathDetail = report_path_detail;
    cout << "[INFO] Report path detail set to: " << (report_path_detail ? "true" : "false") << endl;
    
    cout << "[INFO] JSON configuration parsing completed." << endl;
}

// Disabled per request: ApplyConfigFromAlg is not needed now
#if 0
static void ApplyConfigFromAlg(TAP_Greedy_dijk* ofw, std::string& dbConnStr) {
    const char* kAlgPath = "..\\..\\..\\alg.conf"; // 上上级目录
    ifstream algifs(kAlgPath);
    if (!algifs.is_open()) {
        cout << "[INFO] Configuration file not found, using default values." << endl;
        return;
    } else {
        cout << "[WARNING] Configuration file found." << endl;
    }

    map<string, string> cfg;
    string line;
    while (std::getline(algifs, line)) {
        trim_inplace(line);
        if (line.empty()) continue;
        // 行首注释：#  ;  //
        if (line[0] == '#' || line[0] == ';') continue;
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') continue;

        size_t pos = line.find('=');
        if (pos == string::npos) continue;

        string key = line.substr(0, pos);
        string val = line.substr(pos + 1);
        trim_inplace(key); trim_inplace(val);
        if (!key.empty()) cfg[key] = val;
    }

    // ---------- DB 连接串 ----------
    if (cfg.find("db_conn_string") != cfg.end()) {
        dbConnStr = cfg["db_conn_string"];
    }
    // 如果存在独立的角色配置，则为连接串追加 options 参数
    if (cfg.find("role") != cfg.end()) {
        string role = cfg["role"];
        trim_inplace(role);
        if (!role.empty() && dbConnStr.find("options=") == string::npos) {
            dbConnStr += " options='-c role=" + role + "'";
        }
    }

    // 设置收敛准则
    if (cfg.find("conv") != cfg.end()) {
        try { ofw->SetConv(atof(cfg["conv"].c_str())); } catch (...) {}
    }

    // 设置最大迭代次数
    if (cfg.find("max_iter") != cfg.end()) {
        try { ofw->SetMaxIter(atoi(cfg["max_iter"].c_str())); } catch (...) {}
    }

    // 设置路段性能函数类型
    if (cfg.find("lpf") != cfg.end()) {
        string lpf = to_upper(cfg["lpf"]);
        if (lpf == "BPRLK" || lpf == "BPR") {
            ofw->SetLPF(BPRLK);
        }
    }

    // 设置成本缩放因子
    if (cfg.find("cost_scalar") != cfg.end()) {
        try { ofw->SetCostScalar(atof(cfg["cost_scalar"].c_str())); } catch (...) {}
    }

    // 设置成本系数
    if (cfg.find("cost_coef_time") != cfg.end()) {
        try {
            double time_coef = atof(cfg["cost_coef_time"].c_str());
            ofw->SetCostCoef(time_coef, 0.0); // 距离系数设为0
        } catch (...) {}
    }

    // ---------- 报告开关 ----------
    if (cfg.find("report_iter_history") != cfg.end()) {
        bool ok = false; bool v = parse_bool(cfg["report_iter_history"], &ok);
        if (ok) ofw->reportIterHistory = v;
    }
    if (cfg.find("report_link_detail") != cfg.end()) {
        bool ok = false; bool v = parse_bool(cfg["report_link_detail"], &ok);
        if (ok) ofw->reportLinkDetail = v;
    }
    if (cfg.find("report_path_detail") != cfg.end()) {
        bool ok = false; bool v = parse_bool(cfg["report_path_detail"], &ok);
        if (ok) ofw->reportPathDetail = v;
    }
}
#endif

// TestGreedy_dijk interface using PostgreSQL database with JSON configuration
extern "C" __declspec(dllexport)
const char* TestGreedy_dijkWithPostgreSQL(const char* jsonConfig, const char* networkTableName, const char* odTableName) {
    static std::string jsonResponse; // Static to ensure the C-string remains valid after return

    // reset per-run message notes
    TNM_ResetMessageNotes();

    if (!jsonConfig || strlen(jsonConfig) == 0) {
        jsonResponse = BuildJsonResponse(-101, "Invalid Input: jsonConfig is null or empty.", "");
        return jsonResponse.c_str();
    }
    cout << "=== Starting TestGreedy_dijkWithPostgreSQL ===" << endl;
    
    // Basic parameter logging
    cout << "[INFO] Parameter validation:" << endl;
    cout << "[INFO] jsonConfig pointer: " << (void*)jsonConfig << endl;
    cout << "[INFO] networkTableName pointer: " << (void*)networkTableName << endl;
    cout << "[INFO] odTableName pointer: " << (void*)odTableName << endl;

    if (jsonConfig) {
        cout << "[INFO] jsonConfig length: " << strlen(jsonConfig) << endl;
    } else {
        cout << "[WARNING] jsonConfig: NULL" << endl;
    }
    if (jsonConfig) {
        cout << "[INFO] jsonConfig content: " << jsonConfig << endl;
    }

    TAP_Greedy_dijk* ofw = NULL;
    try {
        // Step 1: Initialize
        TNM_FloatFormat::SetFormat(18, 6);
        ofw = new TAP_Greedy_dijk;

        // Step 2: Default parameters
        ofw->SetConv(1.00E-06);
        ofw->SetMaxIter(500);
        ofw->SetLPF(BPRLK);
        ofw->SetCostScalar(60);
        ofw->SetCostCoef(1, 0.00);

        // Default reports
        ofw->reportIterHistory = true;
        ofw->reportLinkDetail = true;
        ofw->reportPathDetail = true;

        // Step 3: Parse JSON to construct dbConnStr and scenarioPrefix
        string dbConnStr;
        string scenarioPrefix;
        if (jsonConfig && strlen(jsonConfig) > 0) {
            ApplyConfigFromJSON(ofw, string(jsonConfig), dbConnStr, scenarioPrefix);
        } else {
            cout << "[WARNING] No JSON provided. Aborting since DB connection string is required." << endl;
            delete ofw; ofw = NULL; 
            jsonResponse = BuildJsonResponse(-101, "Invalid Input: jsonConfig is null or empty.", "");
            return jsonResponse.c_str();
        }

        // Step 4: Table names
        string role = extract_nested_json_string_value(string(jsonConfig), "db_conn_str", "role");

        string baseNetworkTableName = (networkTableName && strlen(networkTableName) > 0)
                                      ? string(networkTableName)
                                      : string("road_way");
        string baseODTableName = (odTableName && strlen(odTableName) > 0)
                                 ? string(odTableName)
                                 : string("other_od");

        string fullNetworkTableName = baseNetworkTableName;
        if (!role.empty() && baseNetworkTableName.find(role + ".") == string::npos) {
            fullNetworkTableName = role + "." + baseNetworkTableName;
        }

        string fullODTableName = baseODTableName;
        if (!role.empty() && baseODTableName.find(role + ".") == string::npos) {
            fullODTableName = role + "." + baseODTableName;
        }

        cout << "[DEBUG] Using input table names:" << endl;
        cout << "[DEBUG] Network table: " << fullNetworkTableName << endl;
        cout << "[DEBUG] OD table: " << fullODTableName << endl;
        cout << "[DEBUG] Database connection string: " << (dbConnStr.empty() ? "EMPTY" : dbConnStr) << endl;

        if (dbConnStr.empty()) {
            cout << "[Error] Database connection string is empty!" << endl;
            delete ofw; ofw = NULL; 
            jsonResponse = BuildJsonResponse(-101, "Invalid Input: dbConnStr is empty after parsing JSON.", "");
            return jsonResponse.c_str();
        }

        // Step 5: Build from PostgreSQL
        // Pass scenario_prefix to lower-level BuildPostgreSQL via environment variable
        _putenv_s("PG_SCENARIO_PREFIX", scenarioPrefix.c_str());
        // Reset OD mode marker for this run
        _putenv_s("TNA_OD_MODE", "");
        int buildResult = ofw->Build("", "", NETPOSTGRESQL, dbConnStr.c_str(),
                                     fullNetworkTableName.c_str(), fullODTableName.c_str());
        if (buildResult != 0)
        {
            cout << "[Error] Failed to build network from PostgreSQL. code=" << buildResult << endl;
            std::string lastErrDetail; int lastErrCode = 0;
            if (ofw && ofw->network) {
                lastErrCode = ofw->network->GetLastErrorCode();
                lastErrDetail = ofw->network->GetLastErrorDetail();
            }
            std::string msg = std::string("Network Build Failed (code=") + std::to_string(buildResult) + ") for net='" + fullNetworkTableName + "' od='" + fullODTableName + "'.";
            if (lastErrCode != 0 || !lastErrDetail.empty()) {
                msg += std::string(" last_error_code=") + std::to_string(lastErrCode) + " last_error_detail=" + lastErrDetail;
                cout << "[BuildPostgreSQL][LastError] code=" << lastErrCode << " detail=" << lastErrDetail << endl;
            }
            delete ofw; ofw = NULL; 
            // msg = std::string("Network Build Failed (code=") + std::to_string(buildResult) + ") for net='" + fullNetworkTableName + "' od='" + fullODTableName + "'.";
            jsonResponse = BuildJsonResponse(-103, msg, "");
            return jsonResponse.c_str();
        }

        ofw->SetTollType(TT_NOTOLL);
        ofw->SetCentroidsBlocked(false);

        // Step 6: Solve
        if (ofw->Solve() != ConvergeTerm)
        {
            cout << "[Warning]  Algorithm did not converge. Check network data or algorithm parameters." << endl;
            // Note: Even if it doesn't converge, we proceed to generate reports.
            // The final status will reflect the overall success of the operation.
        }

        cout << "OFV=" << ofw->OFV << endl;

        // Step 7: Write results back to PG (tables created/truncated inside)
        _putenv_s("PG_TABLE_OD", fullODTableName.c_str());
        ofw->WritePG(dbConnStr.c_str(), fullNetworkTableName.c_str(), scenarioPrefix.c_str());
        // Progress reaches 100% only after DB write completes
        cout << "[PROGRESS] iter=" << ofw->curIter << " precision=" << (double)ofw->GetConvIndicator() << " percent=100" << endl;

        string dataJson = ReadOutputFilesToJson(scenarioPrefix);
        {
            const char* m = getenv("TNA_OD_MODE");
            std::string mode = (m && *m) ? std::string(m) : std::string();
            if (mode.empty()) mode = "unknown";
            string t = dataJson; trim_inplace(t);
            if (!t.empty() && t.back()=='}') {
                t.pop_back();
                if (t.size()>1) t += ",";
                t += std::string("\"od_mode\":\"") + json_escape(mode) + "\"}";
                dataJson.swap(t);
            }
        }
        {
            string notes = TNM_GetMessageNotes();
            trim_inplace(notes);
            if (!notes.empty()) {
                string t = dataJson; trim_inplace(t);
                if (!t.empty() && t.back()=='}') {
                    t.pop_back();
                    if (t.size()>1) t += ",";
                    string esc = json_escape(notes);
                    t += std::string("\"warnings\":\"") + esc + std::string("\",\"\\u8b66\\u544a\":\"") + esc + "\"}";
                    dataJson.swap(t);
                }
            }
        }
        // Merge bilingual summary and params into data
        {
            string paramsBi = BuildBilingualParamsJSON(string(jsonConfig), role, fullNetworkTableName, fullODTableName, scenarioPrefix);
            string summaryBi = BuildBilingualSummaryJSON(scenarioPrefix, ofw->curIter);
            cout << "[DEBUG] scenario_prefix(raw): " << scenarioPrefix << endl;
            cout << "[DEBUG] params_bilingual(raw): " << paramsBi << endl;
            cout << "[DEBUG] summary_bilingual(raw): " << summaryBi << endl;
            string t = dataJson; trim_inplace(t);
            if (!t.empty() && t.back()=='}') {
                t.pop_back();
                if (t.size()>1) t += ",";
                t += std::string("\"summary\": ") + summaryBi + std::string(",\"params\": ") + paramsBi + "}";
                dataJson.swap(t);
            }
            string finalSummary = extract_json_object_value(dataJson, "summary");
            string finalParams  = extract_json_object_value(dataJson, "params");
            cout << "[DEBUG] final_data.summary: " << (finalSummary.empty()? string("<EMPTY>") : finalSummary) << endl;
            cout << "[DEBUG] final_data.params: "  << (finalParams.empty()?  string("<EMPTY>") : finalParams)  << endl;
        }

        // Step 9: Clean up and return
        delete ofw; ofw = NULL;
        cout << "=== TestGreedy_dijkWithPostgreSQL Finished Successfully ===" << endl;

        jsonResponse = BuildJsonResponse(1, "Success", dataJson);
        return jsonResponse.c_str();
    } catch (const std::exception& e) {
        cout << "[ERROR] Exception occurred: " << e.what() << endl;
        if (ofw) { delete ofw; ofw = NULL; }
        jsonResponse = BuildJsonResponse(0, std::string("Exception: ") + e.what(), "");
        return jsonResponse.c_str();
    } catch (...) {
        cout << "[ERROR] An unknown error occurred in TestGreedy_dijkWithPostgreSQL." << endl;
        if (ofw) { delete ofw; ofw = NULL; }
        jsonResponse = BuildJsonResponse(0, "Unknown Error: An unhandled exception occurred.", "");
        return jsonResponse.c_str();
    }
}

// Extended API: supports progress callback (iter, precision, percent 0..100)
extern "C" __declspec(dllexport)
const char* TestGreedy_dijkWithPostgreSQLEx(TNADriver_ProgressCallback progressFunc, const char* jsonConfig, const char* networkTableName, const char* odTableName) {
    static std::string jsonResponse;
    // reset per-run message notes
    TNM_ResetMessageNotes();
    if (!jsonConfig || strlen(jsonConfig) == 0) {
        jsonResponse = BuildJsonResponse(-101, "Invalid Input: jsonConfig is null or empty.", "");
        return jsonResponse.c_str();
    }
    cout << "=== Starting TestGreedy_dijkWithPostgreSQLEx ===" << endl;
    cout << "[INFO] progressFunc: " << (void*)progressFunc << endl;
    cout << "[INFO] raw networkTableName ptr: " << (void*)networkTableName << " value: " << (networkTableName?networkTableName:"<NULL>") << endl;
    cout << "[INFO] raw odTableName ptr: " << (void*)odTableName << " value: " << (odTableName?odTableName:"<NULL>") << endl;

    TAP_Greedy_dijk* ofw = NULL;
    try {
        // Init
        TNM_FloatFormat::SetFormat(18, 6);
        ofw = new TAP_Greedy_dijk;
        ofw->SetConv(1.00E-06);
        ofw->SetMaxIter(500);
        ofw->SetLPF(BPRLK);
        ofw->SetCostScalar(60);
        ofw->SetCostCoef(1, 0.00);
        ofw->reportIterHistory = true;
        ofw->reportLinkDetail = true;
        ofw->reportPathDetail = true;
        if (progressFunc) {
            ofw->SetProgressCallback(progressFunc);
        }

        // Parse JSON
        string dbConnStr; string scenarioPrefix;
        ApplyConfigFromJSON(ofw, string(jsonConfig), dbConnStr, scenarioPrefix);
        string role = extract_nested_json_string_value(string(jsonConfig), "db_conn_str", "role");

        string baseNetworkTableName = (networkTableName && strlen(networkTableName) > 0) ? string(networkTableName) : string("road_way");
        string baseODTableName = (odTableName && strlen(odTableName) > 0) ? string(odTableName) : string("other_od");
        string fullNetworkTableName = baseNetworkTableName; if (!role.empty() && baseNetworkTableName.find(role + ".") == string::npos) fullNetworkTableName = role + "." + baseNetworkTableName;
        string fullODTableName = baseODTableName; if (!role.empty() && baseODTableName.find(role + ".") == string::npos) fullODTableName = role + "." + baseODTableName;
        cout << "[INFO] Resolved table names:" << endl;
        cout << "[INFO] Network table: " << fullNetworkTableName << endl;
        cout << "[INFO] OD table: " << fullODTableName << endl;

        if (dbConnStr.empty()) {
            delete ofw; ofw = NULL;
            jsonResponse = BuildJsonResponse(-101, "Invalid Input: dbConnStr is empty after parsing JSON.", "");
            return jsonResponse.c_str();
        }

        // Build
        _putenv_s("PG_SCENARIO_PREFIX", scenarioPrefix.c_str());
        _putenv_s("TNA_OD_MODE", "");
        int buildResult = ofw->Build("", "", NETPOSTGRESQL, dbConnStr.c_str(), fullNetworkTableName.c_str(), fullODTableName.c_str());
        if (buildResult != 0) {
            std::string lastErrDetail; int lastErrCode = 0;
            if (ofw && ofw->network) { lastErrCode = ofw->network->GetLastErrorCode(); lastErrDetail = ofw->network->GetLastErrorDetail(); }
            int topCode = -103;
            std::string msg;
            if (lastErrCode == 9 || lastErrCode == 8) {
                msg = "OD\u6570\u636e\u6709\u95ee\u9898\u8bf7\u68c0\u67e5";
            } else {
                msg = std::string("Network Build Failed (code=") + std::to_string(buildResult) + ") for net='" + fullNetworkTableName + "' od='" + fullODTableName + "'.";
                if (lastErrCode != 0 || !lastErrDetail.empty()) { msg += std::string(" last_error_code=") + std::to_string(lastErrCode) + " last_error_detail=" + lastErrDetail; }
            }

            std::string dataJson = "{";
            dataJson += "\"tables\":{\"network\":\"" + json_escape(fullNetworkTableName) + "\",\"od\":\"" + json_escape(fullODTableName) + "\"},";
            dataJson += "\"build_result\":" + std::to_string(buildResult) + ",";
            dataJson += "\"last_error\":{\"code\":" + std::to_string(lastErrCode) + ",\"detail\":\"" + json_escape(lastErrDetail) + "\"}";
            dataJson += "}";

            delete ofw; ofw = NULL; jsonResponse = BuildJsonResponse(topCode, msg, dataJson); return jsonResponse.c_str();
        }

        ofw->SetTollType(TT_NOTOLL);
        ofw->SetCentroidsBlocked(false);

        // Solve
        unsigned long __seh_code = 0;
        int __solve_ret = SafeSolveNoUnwind(ofw, &__seh_code);
        if (__solve_ret == 2) {
            cout << "[Warning]  Algorithm did not converge." << endl;
        } else if (__solve_ret == -1) {
            cout << "[ERROR] SEH exception occurred during Solve(). code=" << __seh_code << endl;
            std::string msg = std::string("SEH Exception during Solve: code=") + std::to_string((unsigned)__seh_code) + ". Possibly invalid network/OD data.";
            std::string dataJson = "{";
            dataJson += "\"tables\": {\"network\": \"" + json_escape(fullNetworkTableName) + "\", \"od\": \"" + json_escape(fullODTableName) + "\"}";
            if (ofw && ofw->network) {
                dataJson += ",\"net_stats\": {";
                dataJson += "\"nodes\": " + std::to_string(ofw->network->numOfNode) + ",";
                dataJson += "\"links\": " + std::to_string(ofw->network->numOfLink) + ",";
                dataJson += "\"origins\": " + std::to_string(ofw->network->numOfOrigin) + ",";
                dataJson += "\"ods\": " + std::to_string(ofw->network->numOfOD) + "}";
                int lastErrCode = ofw->network->GetLastErrorCode();
                std::string lastErrDetail = ofw->network->GetLastErrorDetail();
                if (lastErrCode != 0 || !lastErrDetail.empty()) {
                    dataJson += ",\"last_error\": {\"code\": " + std::to_string(lastErrCode) + ",\"detail\": \"" + json_escape(lastErrDetail) + "\"}";
                }
            }
            dataJson += "}";
            delete ofw; ofw = NULL;
            jsonResponse = BuildJsonResponse(-104, msg, dataJson);
            return jsonResponse.c_str();
        }

        // Write & gather outputs
        _putenv_s("PG_TABLE_OD", fullODTableName.c_str());
        ofw->WritePG(dbConnStr.c_str(), fullNetworkTableName.c_str(), scenarioPrefix.c_str());
        // Progress reaches 100% only after DB write completes
        cout << "[PROGRESS] iter=" << ofw->curIter << " precision=" << (double)ofw->GetConvIndicator() << " percent=100" << endl;
        if (progressFunc) { progressFunc(ofw->curIter, (double)ofw->GetConvIndicator(), 100); }

        string dataJson = ReadOutputFilesToJson(scenarioPrefix);
        {
            const char* m = getenv("TNA_OD_MODE");
            std::string mode = (m && *m) ? std::string(m) : std::string();
            if (mode.empty()) mode = "unknown";
            string t = dataJson; trim_inplace(t);
            if (!t.empty() && t.back()=='}') {
                t.pop_back();
                if (t.size()>1) t += ",";
                t += std::string("\"od_mode\":\"") + json_escape(mode) + "\"}";
                dataJson.swap(t);
            }
        }
        {
            string notes = TNM_GetMessageNotes();
            trim_inplace(notes);
            if (!notes.empty()) {
                string t = dataJson; trim_inplace(t);
                if (!t.empty() && t.back()=='}') {
                    t.pop_back();
                    if (t.size()>1) t += ",";
                    string esc = json_escape(notes);
                    t += std::string("\"warnings\":\"") + esc + std::string("\",\"\\u8b66\\u544a\":\"") + esc + "\"}";
                    dataJson.swap(t);
                }
            }
        }
        {
            std::string tablesJson = TNM_GetTouchedTables();
            trim_inplace(tablesJson);
            if (tablesJson.empty()) tablesJson = "[]";
            string t = dataJson; trim_inplace(t);
            if (!t.empty() && t.back()=='}') {
                t.pop_back();
                if (t.size()>1) t += ",";
                t += std::string("\"writepg_tables\":") + tablesJson + "}";
                dataJson.swap(t);
            }
        }
        // Merge bilingual summary and params into data
        {
            string paramsBi = BuildBilingualParamsJSON(string(jsonConfig), role, fullNetworkTableName, fullODTableName, scenarioPrefix);
            string summaryBi = BuildBilingualSummaryJSON(scenarioPrefix, ofw->curIter);
            // Debug print raw bilingual objects
            cout << "[DEBUG] scenario_prefix(raw): " << scenarioPrefix << endl;
            cout << "[DEBUG] params_bilingual(raw): " << paramsBi << endl;
            cout << "[DEBUG] summary_bilingual(raw): " << summaryBi << endl;
            string t = dataJson; trim_inplace(t);
            if (!t.empty() && t.back()=='}') {
                t.pop_back();
                if (t.size()>1) t += ",";
                t += "\"summary\": " + summaryBi + ",\"params\": " + paramsBi + "}";
                dataJson.swap(t);
            }
            // Debug print final extracted segments from dataJson
            string finalSummary = extract_json_object_value(dataJson, "summary");
            string finalParams  = extract_json_object_value(dataJson, "params");
            cout << "[DEBUG] final_data.summary: " << (finalSummary.empty()? string("<EMPTY>") : finalSummary) << endl;
            cout << "[DEBUG] final_data.params: "  << (finalParams.empty()?  string("<EMPTY>") : finalParams)  << endl;
        }

        delete ofw; ofw = NULL;
        jsonResponse = BuildJsonResponse(1, "Success", dataJson);
        return jsonResponse.c_str();
    } catch (const std::exception& e) {
        if (ofw) { delete ofw; ofw = NULL; }
        jsonResponse = BuildJsonResponse(0, std::string("Exception: ") + e.what(), "");
        return jsonResponse.c_str();
    } catch (...) {
        if (ofw) { delete ofw; ofw = NULL; }
        jsonResponse = BuildJsonResponse(0, "Unknown Error: An unhandled exception occurred.", "");
        return jsonResponse.c_str();
    }
}

  int main()
 {
     // This main function is a placeholder for the DLL and is not intended to be run directly.
     return 0;
 }
