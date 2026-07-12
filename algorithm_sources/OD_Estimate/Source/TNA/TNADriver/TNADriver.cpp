#define _TNM_DLL_LOADED
#include "..\TNA\header\stdafx.h"
#include <algorithm>
#include "..\TNA\header\TNM_Algorithm.h"
#include "..\TNA\header\TNM_Net.h"
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include "../../../Include/postgresql/libpq-fe.h"
#include <sstream>
#include <string>
#include <unordered_map>
#include <map>
#include <utility>
#include <vector>
#include <cstdlib>
// #include <libpq-fe.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

static inline string TNM_AcpToUtf8(const string& s)
{
#ifdef _WIN32
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, NULL, 0);
    if (wlen <= 0) return s;
    wstring wbuf; wbuf.resize(wlen);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &wbuf[0], wlen);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, NULL, 0, NULL, NULL);
    if (u8len <= 0) return s;
    string out; out.resize(u8len - 1);
    WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), -1, &out[0], u8len, NULL, NULL);
    return out;
#else
    return s;
#endif
}

static inline bool TNM_IsValidUtf8(const string& s)
{
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size();
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t need = 0;
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return false;
        if (i + need > n) return false;
        for (size_t j = 1; j < need; ++j)
        {
            if ((p[i + j] & 0xC0) != 0x80) return false;
        }
        i += need;
    }
    return true;
}

static inline string TNM_ToUtf8Smart(const string& s)
{
#ifdef _WIN32
    if (s.empty()) return s;
    if (TNM_IsValidUtf8(s)) return s;
    return TNM_AcpToUtf8(s);
#else
    return s;
#endif
}

#ifdef _WIN32
static void print_utf8_to_console(const string& s, bool to_stderr)
{
    HANDLE h = GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
    {
        (to_stderr ? cerr : cout) << s;
        (to_stderr ? cerr : cout).flush();
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode))
    {
        // 非控制台（管道/重定向）时直接输出 UTF-8 字节，便于上游按 UTF-8 解码
        (to_stderr ? cerr : cout) << s;
        (to_stderr ? cerr : cout).flush();
        return;
    }
    if (s.empty()) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (wlen <= 0)
    {
        (to_stderr ? cerr : cout) << s;
        (to_stderr ? cerr : cout).flush();
        return;
    }
    wstring wbuf; wbuf.resize(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &wbuf[0], wlen);
    DWORD written = 0;
    WriteConsoleW(h, wbuf.c_str(), (DWORD)wbuf.size(), &written, NULL);
}
#else
static void print_utf8_to_console(const string& s, bool to_stderr)
{
    (to_stderr ? cerr : cout) << s;
    (to_stderr ? cerr : cout).flush();
}
#endif

static void log_text_message(const string& msg)
{
    // 将源字符串（可能为 ACP 编码的字面量）转换为 UTF-8 后输出
    print_utf8_to_console(TNM_AcpToUtf8(msg), false);
}

static inline void trim_inplace(string& s) {
    // 注意：对 isspace 传入 unsigned char，避免非 ASCII 字符导致 UB
    auto notsp = [](int ch){ return !std::isspace((unsigned char)ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notsp));
    s.erase(std::find_if(s.rbegin(), s.rend(), notsp).base(), s.end());
}
static inline string to_upper(string s) {
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
static inline bool parse_bool(const string& v, bool* ok = nullptr) {
    string s = to_upper(v);
    if (s == "1" || s == "TRUE" || s == "YES" || s == "ON")  { if (ok) *ok = true; return true; }
    if (s == "0" || s == "FALSE"|| s == "NO"  || s == "OFF") { if (ok) *ok = true; return false; }
    if (ok) *ok = false; return false;
}

// 去除外层成对引号，便于把 db_conn_string = "" 识别为空值
static inline string strip_wrapping_quotes(string s) {
    trim_inplace(s);
    if (s.size() >= 2) {
        char l = s.front(), r = s.back();
        if ((l == '"' && r == '"') || (l == '\'' && r == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

// 判断值是否需要加引号（包含空白或特殊字符时）
static inline bool needs_quoting(const string& v) {
    return v.find_first_of(" \t\r\n'\"\\") != string::npos;
}

// 按 libpq 规则对单引号和反斜杠转义，并用单引号包裹
static inline string quote_pg_value(const string& v) {
    if (!needs_quoting(v)) return v;
    string out; out.reserve(v.size() + 2);
    out.push_back('\'');
    for (char c : v) {
        if (c == '\'' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static string escape_json_text(const string& s)
{
    string out; out.reserve(s.size() + 8);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '\\': out.push_back('\\'); out.push_back('\\'); break;
        case '"':  out.push_back('\\'); out.push_back('"'); break;
        case '\b': out.push_back('\\'); out.push_back('b'); break;
        case '\f': out.push_back('\\'); out.push_back('f'); break;
        case '\n': out.push_back('\\'); out.push_back('n'); break;
        case '\r': out.push_back('\\'); out.push_back('r'); break;
        case '\t': out.push_back('\\'); out.push_back('t'); break;
        default:
            if (c < 0x20)
            {
                out.push_back('\\');
                out.push_back('u');
                out.push_back('0');
                out.push_back('0');
                out.push_back(hex[(c >> 4) & 0x0F]);
                out.push_back(hex[c & 0x0F]);
            }
            else
            {
                out.push_back((char)c);
            }
            break;
        }
    }
    return out;
}

// 解析 key=value 形式的连接串
// 说明（增强实现）：支持 key[=]value 两侧空格、支持单/双引号与反斜杠转义；
// 能正确解析诸如 "host = localhost port=5432 password='p ss\'\\'" 等形式
static unordered_map<string,string> parse_conn_string(const string& s) {
    unordered_map<string,string> m;

    enum State { S_WS, S_KEY, S_BEQ, S_AEQ, S_VAL, S_SQ, S_DQ, S_ESC_SQ, S_ESC_DQ } st = S_WS;
    string key, val;

    auto push_kv = [&](){
        if (!key.empty()) { m[key] = val; }
        key.clear(); val.clear();
    };

    for (size_t i = 0; i <= s.size(); ++i) {
        char c = (i == s.size()) ? ' ' : s[i]; // 结尾用空格触发 flush
        switch (st) {
            case S_WS:
                if (!std::isspace((unsigned char)c)) { st = S_KEY; --i; }
                break;
            case S_KEY:
                if (c == '=') st = S_AEQ;
                else if (std::isspace((unsigned char)c)) st = S_BEQ;
                else key.push_back(c);
                break;
            case S_BEQ:
                if (c == '=') st = S_AEQ;
                else if (!std::isspace((unsigned char)c)) { key.clear(); st = S_WS; --i; } // 非法片段，丢弃该键
                break;
            case S_AEQ:
                if (std::isspace((unsigned char)c)) break;
                if (c == '\'') st = S_SQ;
                else if (c == '"') st = S_DQ;
                else { val.push_back(c); st = S_VAL; }
                break;
            case S_VAL:
                if (std::isspace((unsigned char)c)) { push_kv(); st = S_WS; }
                else val.push_back(c);
                break;
            case S_SQ:
                if (c == '\\') st = S_ESC_SQ;
                else if (c == '\'') { push_kv(); st = S_WS; }
                else val.push_back(c);
                break;
            case S_ESC_SQ:
                val.push_back(c); st = S_SQ; break;
            case S_DQ:
                if (c == '\\') st = S_ESC_DQ;
                else if (c == '"') { push_kv(); st = S_WS; }
                else val.push_back(c);
                break;
            case S_ESC_DQ:
                val.push_back(c); st = S_DQ; break;
        }
    }
    return m;
}

// 将映射重新拼接为连接串
// 说明（增强实现）：对包含空白或特殊字符的值自动加引号并转义
static string build_conn_string(const unordered_map<string,string>& m) {
    string s;
    for (auto &kv : m) {
        if (!s.empty()) s += " ";
        s += kv.first + "=" + quote_pg_value(kv.second);
    }
    return s;
}

static void remove_utf8_bom(string& s)
{
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
}

struct JSONValue
{
    enum Type { Null, Bool, Number, String, Object } type;
    bool boolValue;
    double numberValue;
    string stringValue;
    map<string, JSONValue> objectValue;

    JSONValue() : type(Null), boolValue(false), numberValue(0.0) {}
};

static void skip_ws(const string& s, size_t& pos)
{
    while (pos < s.size() && isspace((unsigned char)s[pos])) ++pos;
}

static void append_utf8(string& out, unsigned code)
{
    if (code <= 0x7F) {
        out.push_back((char)code);
    }
    else if (code <= 0x7FF) {
        out.push_back((char)(0xC0 | ((code >> 6) & 0x1F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    }
    else if (code <= 0xFFFF) {
        out.push_back((char)(0xE0 | ((code >> 12) & 0x0F)));
        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    }
    else {
        out.push_back((char)(0xF0 | ((code >> 18) & 0x07)));
        out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    }
}

static bool parse_value(const string& s, size_t& pos, JSONValue& out, string& err);

static bool parse_string_token(const string& s, size_t& pos, string& out, string& err)
{
    if (pos >= s.size() || s[pos] != '"') {
        err = "期望字符串";
        return false;
    }
    ++pos;
    string result;
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') {
            out = result;
            return true;
        }
        if (c == '\\') {
            if (pos >= s.size()) {
                err = "非法转义";
                return false;
            }
            char esc = s[pos++];
            switch (esc) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > s.size()) {
                    err = "Unicode 转义长度不足";
                    return false;
                }
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = s[pos++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    else {
                        err = "Unicode 转义包含非法字符";
                        return false;
                    }
                }
                append_utf8(result, code);
                break;
            }
            default:
                err = "不支持的转义序列";
                return false;
            }
        }
        else {
            if ((unsigned char)c < 0x20) {
                err = "字符串包含非法控制字符";
                return false;
            }
            result.push_back(c);
        }
    }
    err = "字符串缺少结束引号";
    return false;
}

static bool parse_number_token(const string& s, size_t& pos, JSONValue& out, string& err)
{
    size_t start = pos;
    if (s[pos] == '-') ++pos;
    if (pos >= s.size() || !isdigit((unsigned char)s[pos])) {
        err = "数字格式错误";
        return false;
    }
    while (pos < s.size() && isdigit((unsigned char)s[pos])) ++pos;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        if (pos >= s.size() || !isdigit((unsigned char)s[pos])) {
            err = "数字小数部分缺失";
            return false;
        }
        while (pos < s.size() && isdigit((unsigned char)s[pos])) ++pos;
    }
    if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
        ++pos;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
        if (pos >= s.size() || !isdigit((unsigned char)s[pos])) {
            err = "指数部分缺失";
            return false;
        }
        while (pos < s.size() && isdigit((unsigned char)s[pos])) ++pos;
    }
    string numStr = s.substr(start, pos - start);
    out.type = JSONValue::Number;
    out.stringValue = numStr;
    out.numberValue = strtod(numStr.c_str(), NULL);
    return true;
}

static bool parse_literal(const string& s, size_t& pos, JSONValue& out, string& err)
{
    if (s.compare(pos, 4, "null") == 0) {
        out.type = JSONValue::Null;
        pos += 4;
        return true;
    }
    if (s.compare(pos, 4, "true") == 0) {
        out.type = JSONValue::Bool;
        out.boolValue = true;
        pos += 4;
        return true;
    }
    if (s.compare(pos, 5, "false") == 0) {
        out.type = JSONValue::Bool;
        out.boolValue = false;
        pos += 5;
        return true;
    }
    err = "无法识别的字面量";
    return false;
}

static bool parse_object(const string& s, size_t& pos, JSONValue& out, string& err)
{
    if (s[pos] != '{') {
        err = "期望对象起始";
        return false;
    }
    ++pos;
    out.type = JSONValue::Object;
    out.objectValue.clear();
    skip_ws(s, pos);
    if (pos < s.size() && s[pos] == '}') {
        ++pos;
        return true;
    }
    while (pos < s.size()) {
        skip_ws(s, pos);
        string key;
        if (!parse_string_token(s, pos, key, err)) return false;
        skip_ws(s, pos);
        if (pos >= s.size() || s[pos] != ':') {
            err = "对象缺少冒号";
            return false;
        }
        ++pos;
        skip_ws(s, pos);
        JSONValue value;
        if (!parse_value(s, pos, value, err)) return false;
        out.objectValue[key] = value;
        skip_ws(s, pos);
        if (pos >= s.size()) {
            err = "对象缺少结束符";
            return false;
        }
        if (s[pos] == ',') {
            ++pos;
            continue;
        }
        if (s[pos] == '}') {
            ++pos;
            return true;
        }
        err = "对象分隔符非法";
        return false;
    }
    err = "对象缺少结束符";
    return false;
}

static bool parse_value(const string& s, size_t& pos, JSONValue& out, string& err)
{
    if (pos >= s.size()) {
        err = "意外结束";
        return false;
    }
    char c = s[pos];
    if (c == '{') return parse_object(s, pos, out, err);
    if (c == '"') {
        string str;
        if (!parse_string_token(s, pos, str, err)) return false;
        out.type = JSONValue::String;
        out.stringValue = str;
        return true;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number_token(s, pos, out, err);
    }
    if (c == 't' || c == 'f' || c == 'n') {
        return parse_literal(s, pos, out, err);
    }
    err = "不支持的 JSON 值";
    return false;
}

static bool parse_json(const string& text, JSONValue& out, string& err)
{
    size_t pos = 0;
    skip_ws(text, pos);
    if (!parse_value(text, pos, out, err)) return false;
    skip_ws(text, pos);
    if (pos != text.size()) {
        err = "存在多余的 JSON 片段";
        return false;
    }
    if (out.type != JSONValue::Object) {
        err = "顶层必须是对象";
        return false;
    }
    return true;
}

static const JSONValue* json_get(const JSONValue& root, const vector<string>& path)
{
    const JSONValue* cur = &root;
    for (size_t i = 0; i < path.size(); ++i) {
        if (cur == NULL || cur->type != JSONValue::Object) return NULL;
        map<string, JSONValue>::const_iterator it = cur->objectValue.find(path[i]);
        if (it == cur->objectValue.end()) return NULL;
        cur = &it->second;
    }
    return cur;
}

// VS2012 对统一初始化支持有限，提供辅助函数构造 JSON 路径
static vector<string> json_path(const char* a, const char* b = 0, const char* c = 0, const char* d = 0)
{
    vector<string> path;
    if (a && *a) path.push_back(a);
    if (b && *b) path.push_back(b);
    if (c && *c) path.push_back(c);
    if (d && *d) path.push_back(d);
    return path;
}

static bool json_get_string(const JSONValue& root, const vector<string>& path, string& out)
{
    const JSONValue* v = json_get(root, path);
    if (v == NULL) return false;
    if (v->type == JSONValue::String) {
        out = v->stringValue;
        return true;
    }
    if (v->type == JSONValue::Number) {
        out = v->stringValue;
        return true;
    }
    if (v->type == JSONValue::Bool) {
        out = v->boolValue ? "true" : "false";
        return true;
    }
    return false;
}

static bool json_get_double(const JSONValue& root, const vector<string>& path, double& out)
{
    const JSONValue* v = json_get(root, path);
    if (v == NULL) return false;
    if (v->type == JSONValue::Number) {
        out = v->numberValue;
        return true;
    }
    if (v->type == JSONValue::String) {
        string tmp = v->stringValue;
        trim_inplace(tmp);
        if (tmp.empty()) return false;
        char* endp = NULL;
        out = strtod(tmp.c_str(), &endp);
        if (endp != NULL && *endp == '\0') return true;
        return false;
    }
    if (v->type == JSONValue::Bool) {
        out = v->boolValue ? 1.0 : 0.0;
        return true;
    }
    return false;
}

static bool json_get_int(const JSONValue& root, const vector<string>& path, int& out)
{
    double v = 0.0;
    if (!json_get_double(root, path, v)) return false;
    out = (int)(v);
    return true;
}

static bool json_get_bool(const JSONValue& root, const vector<string>& path, bool& out)
{
    const JSONValue* v = json_get(root, path);
    if (v == NULL) return false;
    if (v->type == JSONValue::Bool) {
        out = v->boolValue;
        return true;
    }
    if (v->type == JSONValue::Number) {
        out = (v->numberValue != 0.0);
        return true;
    }
    if (v->type == JSONValue::String) {
        string tmp = v->stringValue;
        trim_inplace(tmp);
        bool ok = false;
        bool parsed = parse_bool(tmp, &ok);
        if (ok) {
            out = parsed;
            return true;
        }
    }
    return false;
}

static bool ApplyConfigFromJSON(
    OD_ESTIMATION* odes,
    const string& jsonConfig,
    string& dbConnStr,
    string& roleFromCfg,
    string& scenarioPrefix,
    string& networkTableFromCfg,
    string& odTableFromCfg,
    string& observedTableFromCfg,
    string& errorMessage)
{
    errorMessage.clear();
    if (jsonConfig.empty()) {
        log_text_message("JSON 配置为空，继续使用默认参数。\n");
        return true;
    }

    string text = jsonConfig;
    remove_utf8_bom(text);

    JSONValue root;
    string err;
    log_text_message("正在解析 JSON 配置...\n");
    if (!parse_json(text, root, err)) {
        string utf8Msg = string("JSON 解析失败: ") + err + "\n";
        log_text_message(utf8Msg);
        errorMessage = string("JSON 解析失败: ") + err;
        return false;
    }
    log_text_message("JSON 配置解析完成。\n");

    string fullConn;
    if (!json_get_string(root, json_path("db_conn_str", "connection_string"), fullConn)) {
        json_get_string(root, json_path("db_conn_str", "db_conn_string"), fullConn);
    }
    if (fullConn.empty()) {
        json_get_string(root, json_path("db_conn_string"), fullConn);
    }
    trim_inplace(fullConn);
    bool useFullConn = false;
    if (!fullConn.empty()) {
        dbConnStr = fullConn;
        useFullConn = true;
    }

    string host; json_get_string(root, json_path("db_conn_str", "host"), host); trim_inplace(host);
    string port; json_get_string(root, json_path("db_conn_str", "port"), port); trim_inplace(port);
    string dbname; json_get_string(root, json_path("db_conn_str", "dbname"), dbname); trim_inplace(dbname);
    string user; json_get_string(root, json_path("db_conn_str", "user"), user); trim_inplace(user);
    string password; json_get_string(root, json_path("db_conn_str", "password"), password); trim_inplace(password);

    if (!useFullConn) {
        unordered_map<string,string> dbParts;
        bool parsedOK = false;
        bool isURI = false;
        if (!dbConnStr.empty()) {
            dbParts = parse_conn_string(dbConnStr);
            parsedOK = !dbParts.empty();
            string lower = dbConnStr;
            for (size_t i = 0; i < lower.size(); ++i) {
                lower[i] = (char)tolower((unsigned char)lower[i]);
            }
            if (lower.rfind("postgres://", 0) == 0 || lower.rfind("postgresql://", 0) == 0) {
                isURI = true;
            }
        }

        unordered_map<string,string> overlay;
        if (!host.empty()) overlay["host"] = host;
        if (!port.empty()) overlay["port"] = port;
        if (!dbname.empty()) overlay["dbname"] = dbname;
        if (!user.empty()) overlay["user"] = user;
        if (!password.empty()) overlay["password"] = password;

        if (!isURI) {
            if (parsedOK || dbConnStr.empty()) {
                for (auto &kv : overlay) dbParts[kv.first] = kv.second;
                if (!dbParts.empty() && (parsedOK || !overlay.empty())) {
                    dbConnStr = build_conn_string(dbParts);
                }
            }
        }

        if (dbConnStr.empty()) {
            if (host.empty()) host = "localhost";
            if (port.empty()) port = "5432";
            if (dbname.empty()) dbname = "test";
            if (user.empty()) user = "postgres";
            unordered_map<string,string> defaults;
            defaults["host"] = host;
            defaults["port"] = port;
            defaults["dbname"] = dbname;
            defaults["user"] = user;
            defaults["password"] = password;
            dbConnStr = build_conn_string(defaults);
        }
    }

    json_get_string(root, json_path("db_conn_str", "scenario_prefix"), scenarioPrefix);
    trim_inplace(scenarioPrefix);

    json_get_string(root, json_path("db_conn_str", "role"), roleFromCfg);
    trim_inplace(roleFromCfg);
    if (roleFromCfg.empty()) {
        json_get_string(root, json_path("db_conn_str", "role_name"), roleFromCfg);
        trim_inplace(roleFromCfg);
    }
    if (roleFromCfg.empty()) {
        json_get_string(root, json_path("role"), roleFromCfg);
        trim_inplace(roleFromCfg);
    }
    if (roleFromCfg.empty()) {
        json_get_string(root, json_path("role_name"), roleFromCfg);
        trim_inplace(roleFromCfg);
    }
    json_get_string(root, json_path("db_conn_str", "network_table"), networkTableFromCfg);
    trim_inplace(networkTableFromCfg);
    if (networkTableFromCfg.empty()) {
        json_get_string(root, json_path("db_conn_str", "networkTable"), networkTableFromCfg);
        trim_inplace(networkTableFromCfg);
    }
    if (networkTableFromCfg.empty()) {
        json_get_string(root, json_path("network_table"), networkTableFromCfg);
        trim_inplace(networkTableFromCfg);
    }
    if (networkTableFromCfg.empty()) {
        json_get_string(root, json_path("networkTable"), networkTableFromCfg);
        trim_inplace(networkTableFromCfg);
    }

    json_get_string(root, json_path("db_conn_str", "od_table"), odTableFromCfg);
    trim_inplace(odTableFromCfg);
    if (odTableFromCfg.empty()) {
        json_get_string(root, json_path("db_conn_str", "odTable"), odTableFromCfg);
        trim_inplace(odTableFromCfg);
    }
    if (odTableFromCfg.empty()) {
        json_get_string(root, json_path("od_table"), odTableFromCfg);
        trim_inplace(odTableFromCfg);
    }
    if (odTableFromCfg.empty()) {
        json_get_string(root, json_path("odTable"), odTableFromCfg);
        trim_inplace(odTableFromCfg);
    }

    json_get_string(root, json_path("db_conn_str", "observed_flow_table"), observedTableFromCfg);
    trim_inplace(observedTableFromCfg);
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("db_conn_str", "observedFlowTable"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("db_conn_str", "observed_table"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("db_conn_str", "observedTable"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("observed_flow_table"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("observedFlowTable"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("observed_table"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("observedTable"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }

    if (networkTableFromCfg.empty()) {
        json_get_string(root, json_path("network_table_name"), networkTableFromCfg);
        trim_inplace(networkTableFromCfg);
    }
    if (odTableFromCfg.empty()) {
        json_get_string(root, json_path("od_table_name"), odTableFromCfg);
        trim_inplace(odTableFromCfg);
    }
    if (observedTableFromCfg.empty()) {
        json_get_string(root, json_path("observed_flow_table_name"), observedTableFromCfg);
        trim_inplace(observedTableFromCfg);
    }

    int formatPrecision = 0, formatScale = 0;
    bool hasPrecision = json_get_int(root, json_path("algorithm_params", "format_precision"), formatPrecision);
    bool hasScale = json_get_int(root, json_path("algorithm_params", "format_scale"), formatScale);
    if (hasPrecision || hasScale) {
        if (!hasPrecision) formatPrecision = 18;
        if (!hasScale) formatScale = 6;
        TNM_FloatFormat::SetFormat(formatPrecision, formatScale);
    }

    bool boolValue = false;
    if (json_get_bool(root, json_path("algorithm_params", "use_external_observed_flow"), boolValue)) {
        odes->SetUseExternalObservedFlow(boolValue);
        {
            string msg = string("算法参数: use_external_observed_flow = ") + (boolValue ? "true" : "false") + "\n";
            log_text_message(msg);
        }
    } else {
        // 未提供时，按产品约定默认开启外部观测流
        odes->SetUseExternalObservedFlow(true);
        {
            string msg = string("算法参数: use_external_observed_flow 未提供，使用默认值 = true\n");
            log_text_message(msg);
        }
    }

    // 解析 use_roadway_observed（是否从 roadway 表作为观测流来源）
    if (json_get_bool(root, json_path("algorithm_params", "use_roadway_observed"), boolValue)) {
        odes->SetUseRoadwayObserved(boolValue);
        {
            string msg = string("算法参数: use_roadway_observed = ") + (boolValue ? "true" : "false") + "\n";
            log_text_message(msg);
        }
    }

    double dValue = 0.0;
    if (json_get_double(root, json_path("algorithm_params", "gamma"), dValue)) {
        odes->SetGamma((floatType)dValue);
    }
    double gamma1Value = 0.0;
    bool hasGamma1 = json_get_double(root, json_path("algorithm_params", "gamma1"), gamma1Value);
    double gamma2Value = 0.0;
    bool hasGamma2 = json_get_double(root, json_path("algorithm_params", "gamma2"), gamma2Value);
    if (hasGamma1 && gamma1Value >= 0.0 && gamma1Value <= 1.0) {
        odes->gamma1 = (floatType)gamma1Value;
        if (!hasGamma2) {
            double complement = 1.0 - gamma1Value;
            if (complement >= 0.0 && complement <= 1.0) {
                odes->gamma2 = (floatType)complement;
            }
        }
    }
    if (hasGamma2 && gamma2Value >= 0.0 && gamma2Value <= 1.0) {
        odes->gamma2 = (floatType)gamma2Value;
        if (!hasGamma1) {
            double complement = 1.0 - gamma2Value;
            if (complement >= 0.0 && complement <= 1.0) {
                odes->gamma1 = (floatType)complement;
            }
        }
    }
    if (json_get_double(root, json_path("algorithm_params", "oblink_ratio"), dValue)) {
        odes->SetOblink_ratio((floatType)dValue);
    }
    int iValue = 0;
    if (json_get_int(root, json_path("algorithm_params", "mainloop_maxiter"), iValue) && iValue > 0) {
        odes->SetMainloop_maxiter(iValue);
    }
    if (json_get_double(root, json_path("algorithm_params", "mainloop_conv"), dValue) && dValue > 0) {
        odes->SetMainloop_conv((floatType)dValue);
    }
    if (json_get_int(root, json_path("algorithm_params", "armijo_maxiter"), iValue) && iValue > 0) {
        odes->SetArmijo_maxiter(iValue);
    }
    if (json_get_double(root, json_path("algorithm_params", "armijo_stopcriterion"), dValue) && dValue > 0) {
        odes->SetArmijo_stopcriterion((floatType)dValue);
    }
    if (json_get_double(root, json_path("algorithm_params", "armijo_coefficient"), dValue) && dValue > 0) {
        odes->SetArmijo_coefficient((floatType)dValue);
    }
    if (json_get_double(root, json_path("algorithm_params", "armijo_alphamax"), dValue) && dValue > 0) {
        odes->SetArmijo_alphamax((floatType)dValue);
    }

    if (json_get_double(root, json_path("algorithm_params", "conv"), dValue) && dValue > 0) {
        odes->SetConv((floatType)dValue);
    }
    if (!json_get_int(root, json_path("algorithm_params", "max_iter"), iValue)) {
        json_get_int(root, json_path("algorithm_params", "maxiter"), iValue);
    }
    if (iValue > 0) {
        odes->SetMaxIter(iValue);
    }

    string lpf;
    if (json_get_string(root, json_path("algorithm_params", "lpf"), lpf)) {
        trim_inplace(lpf);
        string lpfUpper = to_upper(lpf);
        if (lpfUpper.find("BPRLK") != string::npos || lpfUpper.find("BPR") != string::npos) {
            odes->SetLPF(BPRLK);
        }
    }

    if (!json_get_double(root, json_path("algorithm_params", "cost_scalar"), dValue)) {
        json_get_double(root, json_path("algorithm_params", "costscalar"), dValue);
    }
    if (dValue > 0) {
        odes->SetCostScalar((floatType)dValue);
    }

    // 新增：观测流量放大倍数（默认为 1.0）
    if (json_get_double(root, json_path("algorithm_params", "observed_flow_scale"), dValue)) {
        if (dValue <= 0) dValue = 1.0;
        odes->SetObservedFlowScale(dValue);
        {
            ostringstream oss; oss << "算法参数: observed_flow_scale = " << dValue << "\n";
            log_text_message(oss.str());
        }
    }

    // 新增：OFW 不可达 OD 对 warnings 示例条数（0 表示仅输出汇总不列示例）
    {
        int warnN = 0;
        if (json_get_int(root, json_path("algorithm_params", "ofw_unreachable_warning_examples"), warnN))
        {
            if (warnN < 0) warnN = 0;
            TNM_SetOfwUnreachableWarningExamples(warnN);
            {
                ostringstream oss; oss << "算法参数: ofw_unreachable_warning_examples = " << warnN << "\n";
                log_text_message(oss.str());
            }
        }
    }

    double costTime = 0.0, costDist = 0.0;
    bool hasTime = json_get_double(root, json_path("algorithm_params", "cost_coef_time"), costTime);
    if (!hasTime) hasTime = json_get_double(root, json_path("algorithm_params", "costcoef_time"), costTime);
    bool hasDist = json_get_double(root, json_path("algorithm_params", "cost_coef_distance"), costDist);
    if (!hasDist) hasDist = json_get_double(root, json_path("algorithm_params", "costcoef_distance"), costDist);
    if (hasTime || hasDist) {
        if (!hasTime) costTime = 1.0;
        if (!hasDist) costDist = 0.0;
        odes->SetCostCoef(costTime, costDist);
    }

    if (json_get_bool(root, json_path("report_iter_history"), boolValue)) {
        odes->reportIterHistory = boolValue;
    }
    if (json_get_bool(root, json_path("report_link_detail"), boolValue)) {
        odes->reportLinkDetail = boolValue;
    }
    if (json_get_bool(root, json_path("report_path_detail"), boolValue)) {
        odes->reportPathDetail = boolValue;
    }
    if (json_get_bool(root, json_path("report_demand_detail"), boolValue)) {
        odes->reportDemandDetail = boolValue;
    }
    if (json_get_bool(root, json_path("report_linkinfo_detail"), boolValue)) {
        odes->reportlinkinforDetail = boolValue;
    }
    if (json_get_bool(root, json_path("report_upper_objective"), boolValue)) {
        odes->reportUpperObjective = boolValue;
    }

    return true;
}

/* -------------------- JSON 配置驱动入口 -------------------- */
static TNM_ProgressCallback g_progressCbUser = NULL;
static double g_lastProgress = 0.0;
static bool g_useRoadwayObservedOverride = false; // 由 WithProgress 接口布尔参数驱动

// 仅做回调转发的安全封装：不得创建带析构的对象，避免与 C++ 异常展开模型冲突
static __declspec(noinline) void SafeInvokeProgressCb(TNM_ProgressCallback cb, int iter, double conv)
{
    if (!cb) return;
#ifdef _WIN32
    __try {
        cb(iter, conv);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // 吞掉外部回调异常（如 AccessViolation），保障算法不中断
    }
#else
    cb(iter, conv);
#endif
}
static void TNM_CDECL progress_wrapper(int iter, double conv)
{
    if (g_progressCbUser)
    {
        double capped = conv;
        if (capped < 0.0) capped = 0.0;
        if (capped > 98.0) capped = 98.0;
        if (capped < g_lastProgress) capped = g_lastProgress; else g_lastProgress = capped;
        // 通过不含对象的安全封装调用，避免在当前函数内直接使用 __try
        SafeInvokeProgressCb(g_progressCbUser, iter, capped);
    }
}

extern "C" __declspec(dllexport) const char* TestOD_Estimate(const char* jsonConfig, const char* networkTableNameInput, const char* odTableNameInput, const char* observedflowTableNameInput)
{
    TNM_FloatFormat::SetFormat(18,6);
    OD_ESTIMATION *odes = new OD_ESTIMATION;
    TNM_ResetLastError();

    // Windows 控制台切换到 UTF-8，避免中文输出乱码
    unsigned int prevOutCP = 0, prevInCP = 0; bool cpSaved = false;
#ifdef _WIN32
    {
        prevOutCP = GetConsoleOutputCP();
        prevInCP  = GetConsoleCP();
        cpSaved = true;
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
    }
#endif

    // 将当前工作目录切换到 DLL 所在目录，确保所有相对路径（alg.json、report_local_analysis 等）均以 DLL 目录为基准
    string moduleDir;
    string prevDir;
    bool   prevDirSaved = false;
#ifdef _WIN32
    {
        char modulePath[MAX_PATH] = {0};
        HMODULE hModule = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&TestOD_Estimate), &hModule)) {
            DWORD len = GetModuleFileNameA(hModule, modulePath, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                moduleDir.assign(modulePath, modulePath + len);
                size_t pos = moduleDir.find_last_of("\\/");
                if (pos != string::npos) moduleDir.resize(pos);

                char buf[MAX_PATH] = {0};
                DWORD curLen = GetCurrentDirectoryA(MAX_PATH, buf);
                if (curLen > 0 && curLen < MAX_PATH) {
                    prevDir.assign(buf, buf + curLen);
                    prevDirSaved = true;
                }
                SetCurrentDirectoryA(moduleDir.c_str());
            }
        }
    }
#endif

    static string responseBuffer;
    auto composeResponse = [&](int status, const string& message, const string& data, bool dataIsJson) -> const char*
    {
        ostringstream oss;
        string messageUtf8 = TNM_ToUtf8Smart(message);
        oss << "{\"status\": " << status << ", \"message\": \"" << escape_json_text(messageUtf8) << "\", \"data\": ";
        if (dataIsJson)
        {
            oss << data;
        }
        else
        {
            oss << "\"" << escape_json_text(data) << "\"";
        }
        oss << "}";
        responseBuffer = oss.str();
        return responseBuffer.c_str();
    };

    auto finalize = [&](int status, const string& message, const string& data, bool dataIsJson) -> const char*
    {
        // 恢复控制台代码页
#ifdef _WIN32
        if (cpSaved) {
            if (prevOutCP) SetConsoleOutputCP(prevOutCP);
            if (prevInCP)  SetConsoleCP(prevInCP);
        }
#endif
        // 恢复进入前的工作目录
#ifdef _WIN32
        if (prevDirSaved) {
            SetCurrentDirectoryA(prevDir.c_str());
        }
#endif
        if (odes != NULL)
        {
            delete odes;
            odes = NULL;
        }
        return composeResponse(status, message, data, dataIsJson);
    };

    // 默认算法参数
    odes->SetGamma(0.5);
    odes->SetOblink_ratio(0.3);
    odes->SetMainloop_maxiter(300);
    odes->SetMainloop_conv(1e-4);
    odes->SetArmijo_maxiter(5);
    odes->SetArmijo_stopcriterion(100);
    odes->SetArmijo_coefficient(10.0);
    odes->SetArmijo_alphamax(1000.0);
    odes->SetConv(1e-6);
    odes->SetMaxIter(500);
    odes->SetLPF(BPRLK);
    odes->SetCostScalar(60);
    odes->SetCostCoef(1.0, 0.0);

    // 重置本次运行的提示/预检查消息收集
    TNM_ResetMessageNotes();

    string dbConnStr;
    odes->SetDbConnStr(dbConnStr.c_str());
    odes->SetUseExternalObservedFlow(false);

    odes->reportDemandDetail     = true;
    odes->reportlinkinforDetail  = true;
    odes->reportPathDetail       = true;
    odes->reportUpperObjective   = true;

    // 注入进度回调（若通过 WithProgress 设置了全局回调）
    if (g_progressCbUser) {
        g_lastProgress = 0.0;
        odes->SetProgressCallback(&progress_wrapper);
    }

    string jsonText;
    bool jsonLoaded = false;

    int status = 1;
    string message = "OK";
    string data;
    bool dataIsJson = false;

    try
    {
        do
        {
            if (jsonConfig != NULL)
            {
                string rawInput = jsonConfig;
                string trimmed = rawInput;
                trim_inplace(trimmed);
                if (trimmed.empty())
                {
                    status = -101;
                    message = "jsonConfig 为空";
                    break;
                }
                jsonText = rawInput;
                jsonLoaded = true;
            }
            else
            {
                vector<string> jsonCandidates;
                // 优先查找 DLL 同目录下的 alg.json
#ifdef _WIN32
                if (!moduleDir.empty()) {
                    jsonCandidates.push_back(moduleDir + "\\alg.json");
                }
#endif
                jsonCandidates.push_back("alg.json");

                // 兼容旧目录结构的候选路径（向上回溯）
#ifdef _WIN32
                if (!moduleDir.empty()) {
                    jsonCandidates.push_back(moduleDir + "\\..\\..\\alg.json");
                    jsonCandidates.push_back(moduleDir + "\\..\\..\\..\\alg.json");
                }
#endif

                for (size_t i = 0; i < jsonCandidates.size(); ++i) {
                    const string& path = jsonCandidates[i];
                    if (path.empty()) {
                        continue;
                    }
                    ifstream in(path.c_str());
                    if (!in.is_open()) {
                        continue;
                    }
                    log_text_message(string("读取配置文件: ") + path + "\n");
                    ostringstream oss;
                    oss << in.rdbuf();
                    jsonText = oss.str();
                    jsonLoaded = true;
                    break;
                }
            }

            if (!jsonLoaded) {
                log_text_message("未找到 JSON 配置文件，继续使用默认参数。\n");
            }

            string roleFromCfg;
            string scenarioPrefix;
            string networkTableFromCfg;
            string odTableFromCfg;
            string observedTableFromCfg;
            string configError;
            if (!ApplyConfigFromJSON(odes, jsonText, dbConnStr, roleFromCfg, scenarioPrefix, networkTableFromCfg, odTableFromCfg, observedTableFromCfg, configError))
            {
                status = -101;
                message = configError.empty() ? string("JSON 解析失败") : configError;
                break;
            }
            odes->SetDbConnStr(dbConnStr.c_str());

            if (dbConnStr.empty())
            {
                status = -101;
                message = "数据库连接串为空";
                break;
            }

            auto chooseTableName = [](const char* input) -> string {
                if (input == NULL || input[0] == '\0') {
                    return string();
                }
                return string(input);
            };

            string networkTable = chooseTableName(networkTableNameInput);
            string odTable = chooseTableName(odTableNameInput);
            string observedTable = chooseTableName(observedflowTableNameInput);

            if (networkTable.empty() && !networkTableFromCfg.empty()) networkTable = networkTableFromCfg;
            if (odTable.empty() && !odTableFromCfg.empty()) odTable = odTableFromCfg;
            if (observedTable.empty() && !observedTableFromCfg.empty()) observedTable = observedTableFromCfg;

            string role = roleFromCfg;
            if (role.empty()) role = TNM_DEFAULT_ROLE;
            odes->SetRoleName(role);

            if (networkTable.empty()) networkTable = TNM_DefaultNetworkTable(role);
            if (odTable.empty()) odTable = TNM_DefaultODTable(role);
            if (observedTable.empty()) observedTable = TNM_DefaultObservedTable(role);

            odes->SetNetworkTableName(networkTable);
            odes->SetODTableName(odTable);
            odes->SetObservedTableName(observedTable);

            log_text_message(string("数据库角色: ") + role + "\n");

            log_text_message(string("网络表: ") + networkTable + "\n");

            log_text_message(string("OD 表: ") + odTable + "\n");

            log_text_message(string("观测流量表: ") + observedTable + "\n");

            // 接口覆盖：当 DLL 接口 use_capacity_business=true 时，强制：
            // 1) 启用从网络表读取观测流（use_roadway_observed=true）
            // 2) 关闭外部表观测流（use_external_observed_flow=false）
            if (g_useRoadwayObservedOverride) {
                odes->SetUseRoadwayObserved(true);
                odes->SetUseExternalObservedFlow(false);
                log_text_message(string("接口参数: use_capacity_business=true, use_roadway_observed=true\n"));
                log_text_message(string("接口参数: use_capacity_business=true, use_external_observed_flow=false\n"));
            }

            if (!scenarioPrefix.empty()) {
                odes->SetResultTablePrefix(scenarioPrefix);
            }

            PGconn* testConn = PQconnectdb(dbConnStr.c_str());
            if (testConn == NULL || PQstatus(testConn) != CONNECTION_OK)
            {
                string errMsg;
                if (testConn != NULL)
                {
                    errMsg = PQerrorMessage(testConn);
                    PQfinish(testConn);
                }
                if (errMsg.empty()) errMsg = "数据库连接失败";
                trim_inplace(errMsg);
                status = -102;
                message = string("数据库连接失败: ") + errMsg;
                break;
            }
            PQfinish(testConn);

            auto to_string_local = [](int v) -> string {
                ostringstream oss;
                oss << v;
                return oss.str();
            };

            int buildRet = odes->Build("", "", NETPOSTGRESQL);
            if (buildRet != 0)
            {
                status = -103;
                string lastError = TNM_GetLastError();
                trim_inplace(lastError);
                if (!lastError.empty())
                {
                    message = lastError;
                }
                else
                {
                    message = string("构建网络失败，错误码 ") + to_string_local(buildRet);
                }
                break;
            }

            TNM_ResetLastError();
            odes->overall_process();
            string solveError = TNM_GetLastError();
            trim_inplace(solveError);
            if (odes->ReachError() || !solveError.empty())
            {
                status = -201;
                if (!solveError.empty())
                {
                    message = solveError;
                }
                else
                {
                    message = "算法求解失败";
                }
                break;
            }

            int reportRet = odes->ReportPG();
            if (reportRet != 0)
            {
                status = -102;
                message = "结果写入数据库失败";
                break;
            }

#ifdef _WIN32
            string analysisDirPath = "report_local_analysis";
            string pathSeparator = "\\";
#else
            string analysisDirPath = "report_local_analysis";
            string pathSeparator = "/";
#endif

            string filePrefix = scenarioPrefix.empty() ? "scenario" : scenarioPrefix;
            vector<string> reportFiles;
            reportFiles.push_back(analysisDirPath + pathSeparator + filePrefix + "top20_link_flow.csv");
            reportFiles.push_back(analysisDirPath + pathSeparator + filePrefix + "top_vc_ratio_links.csv");

            auto read_file_to_string = [](const string& filePath, string& content) -> bool
            {
                ifstream in(filePath.c_str(), ios::in | ios::binary);
                if (!in)
                {
                    return false;
                }
                ostringstream buffer;
                buffer << in.rdbuf();
                content = buffer.str();
                return true;
            };

            string csvTop20, csvTopVC;
            for (size_t i = 0; i < reportFiles.size(); ++i)
            {
                const string& path = reportFiles[i];
                string fileContent;
                if (read_file_to_string(path, fileContent))
                {
                    if (path.find("top20_link_flow.csv") != string::npos) csvTop20 = fileContent;
                    else if (path.find("top_vc_ratio_links.csv") != string::npos) csvTopVC = fileContent;
                }
                else
                {
                    cerr << TNM_AcpToUtf8("无法读取本地分析文件: ") << path << endl;
                }
            }

            auto json_escape = [&](const string& s)->string { return escape_json_text(s); };
            auto d2s = [&](double v)->string { ostringstream oss; oss.setf(ios::fmtflags(0), ios::floatfield); oss << setprecision(numeric_limits<double>::max_digits10) << v; return oss.str(); };
            auto get_last_or_zero = [&](const vector<floatType>& vec)->string {
                if (!vec.empty()) { return d2s((double)vec.back()); }
                return string("0");
            };

            // 终止类型
            string termStr = "Unknown";
            TERMFLAGS tf = odes->TerminationType();
            if (tf == ConvergeTerm) termStr = "Converged";
            else if (tf == MaxIterTerm) termStr = "MaxIteration";
            else if (tf == UserTerm) termStr = "User";
            else if (tf == ErrorTerm) termStr = "Error";

            // 表名，与 ReportPG 一致的命名规则：scenarioPrefix 与 suffix 之间保证有且只有一个下划线
            auto composeTableName = [&](const string& role, const string& suffix)->string {
                string p = scenarioPrefix;
                string s = suffix;
                trim_inplace(p);
                trim_inplace(s);
                if (p.empty())
                {
                    return TNM_ComposeTableName(role, s);
                }
                while (!p.empty() && p.back() == '_') p.pop_back();
                while (!s.empty() && s.front() == '_') s.erase(0, 1);
                return TNM_ComposeTableName(role, p + "_" + s);
            };
            string roleForTable = (roleFromCfg.empty() ? TNM_DEFAULT_ROLE : roleFromCfg);
            string odTableOut   = composeTableName(roleForTable, "od_estimation_results");
            string linkTableOut = composeTableName(roleForTable, "link_flow_results");
            string iterTableOut = composeTableName(roleForTable, "iteration_record");

            // 统计 v/c 指标
            double avg_vc = 0.0; int vc_cnt = 0; int overs_cnt = 0;
            if (odes->network != NULL)
            {
                TNM_SNET* net = odes->network;
                for (int i = 0; i < net->numOfLink; ++i)
                {
                    TNM_SLINK* lk = net->linkVector[i];
                    if (!lk) continue;
                    double cap = (double)lk->capacity;
                    if (cap > 0.0)
                    {
                        double vc = (double)lk->volume / cap;
                        avg_vc += vc; ++vc_cnt;
                        if (vc > 0.9) ++overs_cnt;
                    }
                }
            }
            if (vc_cnt > 0) avg_vc /= (double)vc_cnt; else avg_vc = 0.0;

            // 最终 100% 进度（若有回调）
            if (g_progressCbUser) {
                int final_iter = (int)odes->up_obj_vec.size();
                if (final_iter > 0) final_iter -= 1;
                SafeInvokeProgressCb(g_progressCbUser, final_iter, 100.0);
            }

            // 结构化 JSON data（键采用“英文|中文”形式，中文以 \uXXXX 形式转义）
            ostringstream ds;
            ds << '{';
            // summary（结果字段）
            int iter_cnt = (int)odes->up_obj_vec.size();
            if (iter_cnt > 0) iter_cnt -= 1;
            ds << "\"summary\": {"
               << "\"iteration_count\": " << iter_cnt << ','
               << "\"迭代次数\": " << iter_cnt << ','
               << "\"last_upper_objective\": " << get_last_or_zero(odes->up_obj_vec) << ','
               << "\"最后上层目标值\": " << get_last_or_zero(odes->up_obj_vec) << ','
               << "\"last_rmse\": " << get_last_or_zero(odes->RMSE_vec) << ','
               << "\"最后RMSE\": " << get_last_or_zero(odes->RMSE_vec) << ','
               << "\"last_relative_gap\": " << get_last_or_zero(odes->RGP_vec) << ','
               << "\"最后相对间隙\": " << get_last_or_zero(odes->RGP_vec) << ','
               << "\"total_time_sec\": " << d2s((double)odes->cpuTime) << ','
               << "\"总耗时秒\": " << d2s((double)odes->cpuTime) << ','
               << "\"termination\": \"" << termStr << "\"," 
               << "\"终止类型\": \"" << termStr << "\"," 
               << "\"avg_vc\": " << d2s(avg_vc) << ','
               << "\"平均v_c\": " << d2s(avg_vc) << ','
               << "\"oversaturated_count_vc_gt_0_9\": " << overs_cnt << ','
               << "\"v_c超过0.9数量\": " << overs_cnt
               << "},";

            // params
            string lpfStr = (odes->lpf == BPRLK ? string("BPRLK") : string("Unknown"));
            ds << "\"params\": {"
               << "\"max_main_iter\": " << (int)odes->L << ','
               << "\"最大主循环迭代数\": " << (int)odes->L << ','
               << "\"conv_criterion\": " << d2s((double)odes->conv_Criterion) << ','
               << "\"收敛阈值\": " << d2s((double)odes->conv_Criterion) << ','
               << "\"use_external_observed_flow\": " << (odes->useExternalObservedFlow ? "true" : "false") << ','
               << "\"使用外部观测流\": " << (odes->useExternalObservedFlow ? "true" : "false") << ','
               << "\"gamma1\": " << d2s((double)odes->gamma1) << ','
               << "\"gamma1（OD 差异项权重）\": " << d2s((double)odes->gamma1) << ','
               << "\"gamma2\": " << d2s((double)odes->gamma2) << ','
               << "\"gamma2（观测流量差异项权重）\": " << d2s((double)odes->gamma2) << ','
               << "\"lpf\": \"" << lpfStr << "\"," 
               << "\"链路阻抗函数\": \"" << lpfStr << "\"," 
               << "\"cost_scalar\": " << d2s((double)odes->costScalar) << ','
               << "\"成本尺度\": " << d2s((double)odes->costScalar) << ','
               << "\"oblink_ratio\": " << d2s((double)odes->oblink_ratio) << ','
               << "\"观测占比\": " << d2s((double)odes->oblink_ratio) << ','
               << "\"armijo_maxiter\": " << (int)odes->ls_J << ','
               << "\"Armijo最大迭代\": " << (int)odes->ls_J << ','
               << "\"armijo_stopcriterion\": " << d2s((double)odes->epsilon_2) << ','
               << "\"Armijo停止准则\": " << d2s((double)odes->epsilon_2) << ','
               << "\"armijo_coefficient\": " << d2s((double)odes->Theta) << ','
               << "\"Armijo系数\": " << d2s((double)odes->Theta) << ','
               << "\"armijo_alphamax\": " << d2s((double)odes->alpha_max) << ','
               << "\"Armijo步长上界\": " << d2s((double)odes->alpha_max)
               << "},";
            // 简要历史（末尾最多 10 条）
            auto emit_tail_array = [&](const vector<floatType>& vec, int k, const char* name){
                ds << "\"" << name << "\":[";
                int n = (int)vec.size();
                int b = (n > k) ? (n - k) : 0;
                for (int i = b; i < n; ++i) {
                    if (i > b) ds << ',';
                    ds << d2s((double)vec[i]);
                }
                ds << "],";
            };
            ds << "\"history_tail\": {";
            emit_tail_array(odes->up_obj_vec, 10, "up_objective");
            emit_tail_array(odes->RMSE_vec,   10, "rmse");
            emit_tail_array(odes->RGP_vec,    10, "relative_gap");
            emit_tail_array(odes->Time_vec,   10, "iter_time_sec");
            // 去掉最后一个逗号：覆盖写入空对象修正
            ds.seekp(-1, ios_base::cur); ds << "},";

            ds << "\"tables\": {"
               << "\"od_estimation_results\": \"" << odTableOut << "\"," 
               << "\"link_flow_results\": \"" << linkTableOut << "\"," 
               << "\"iteration_record\": \"" << iterTableOut << "\"" 
               << "},";

            ds << "\"reports\": {"
               << "\"top20_link_flow_csv\": \"" << json_escape(csvTop20) << "\"," 
               << "\"top_vc_ratio_links_csv\": \"" << json_escape(csvTopVC) << "\"" 
               << "}";

            // warnings：将算法侧累计的告警信息（TNM_AppendMessageNote）返回到 JSON，供上层结构化解析
            {
                string notes = TNM_GetMessageNotes();
                trim_inplace(notes);
                ds << ",\"warnings\": \"" << json_escape(notes) << "\",";// 
                ds << "\"\u8b66\u544a\": \"" << json_escape(notes) << "\"";// 
            }
            ds << '}';

            data = TNM_ToUtf8Smart(ds.str());
            dataIsJson = true;
        }
        while(false);
    }
    catch (...)
    {
        status = 0;
        message = "出现未知异常";
    }

#ifdef _WIN32
    if (status == 1)
    {
        // system("PAUSE");
    }
#endif
    // 将预检查阶段累计的警告信息拼接到最终 message 中
    {
        string notes = TNM_GetMessageNotes();
        trim_inplace(notes);
        if (!notes.empty())
        {
            if (!message.empty()) message.append("\n");
            message.append(notes);
        }
    }
    return finalize(status, message, data, dataIsJson);
}

// 带进度回调的导出接口：第一个参数为回调指针 (int iter, double conv)
extern "C" __declspec(dllexport) const char* TestOD_Estimate_WithProgress(
    TNM_ProgressCallback progressCb,
    const char* jsonConfig,
    const char* networkTableNameInput,
    const char* odTableNameInput,
    const char* observedflowTableNameInput,
    int use_capacity_business)
{
    g_progressCbUser = progressCb;
    g_useRoadwayObservedOverride = (use_capacity_business != 0);
    const char* ret = TestOD_Estimate(jsonConfig, networkTableNameInput, odTableNameInput, observedflowTableNameInput);
    g_useRoadwayObservedOverride = false;
    g_progressCbUser = NULL;
    return ret;
}

static void TNM_CDECL progress_logger(int iter, double conv)
{
    static int last_bucket = -1;
    int bucket = (int)conv / 10; // 0..10
    if (bucket != last_bucket)
    {
        last_bucket = bucket;
        int shown = bucket * 10;
        if (shown < 0) shown = 0;
        if (shown > 100) shown = 100;
        cout << "[progress] " << shown << "%" << endl;
    }
}

int main(int argc, char** argv)
{
    const char* jsonArg = NULL;
    string jsonText;
    if (argc > 1)
    {
        ifstream in(argv[1], ios::in | ios::binary);
        if (in)
        {
            ostringstream oss; oss << in.rdbuf();
            jsonText = oss.str();
            jsonArg = jsonText.c_str();
        }
    }
    // 若未提供配置文件，则使用内嵌的标准示例配置
    if (jsonArg == NULL)
    {
        jsonText = 
            "{\n"
            "  \"db_conn_str\": {\n"
            "    \"connection_string\": \"\",\n"
            "    \"host\": \"211.149.178.166\",\n"
            "    \"port\": 5432,\n"
            "    \"dbname\": \"urban\",\n"
            "    \"user\": \"postgres\",\n"
            "    \"password\": \"mysecurepassword\",\n"
            "    \"scenario_prefix\": \"project29_user1_\",\n"
            "    \"network_table\": \"user_project.project29_user1_road_way\",\n"
            "    \"od_table\": \"user_project.project29_user1_other_od\",\n"
            "    \"observed_flow_table\": \"user_project.project29_user1_other_observation\"\n"
            "  },\n"
            "  \"algorithm_params\": {\n"
            "    \"use_external_observed_flow\": true,\n"
            "    \"format_precision\": 18,\n"
            "    \"format_scale\": 6,\n"
            "    \"gamma\": 0.5,\n"
            "    \"gamma1\": 0.5,\n"
            "    \"gamma2\": 0.5,\n"
            "    \"oblink_ratio\": 0.3,\n"
            "    \"mainloop_maxiter\": 300,\n"
            "    \"mainloop_conv\": 0.0001,\n"
            "    \"armijo_maxiter\": 5,\n"
            "    \"armijo_stopcriterion\": 100,\n"
            "    \"armijo_coefficient\": 10.0,\n"
            "    \"armijo_alphamax\": 1000.0,\n"
            "    \"conv\": 1e-6,\n"
            "    \"max_iter\": 500,\n"
            "    \"lpf\": \"BPRLK\",\n"
            "    \"cost_scalar\": 60,\n"
            "    \"cost_coef_time\": 1.0,\n"
            "    \"cost_coef_distance\": 0.0\n"
            "  },\n"
            "  \"report_iter_history\": true,\n"
            "  \"report_link_detail\": true,\n"
            "  \"report_path_detail\": true,\n"
            "  \"report_demand_detail\": true,\n"
            "  \"report_linkinfo_detail\": true,\n"
            "  \"report_upper_objective\": true\n"
            "}\n";
        jsonArg = jsonText.c_str();
    }
    const char* ret1 = TestOD_Estimate_WithProgress(&progress_logger, jsonArg, "", "", "", 0);
    print_utf8_to_console(string(ret1), false);
    const char* ret2 = TestOD_Estimate_WithProgress(&progress_logger, jsonArg, "", "", "", 0);
    print_utf8_to_console(string(ret2), false);
    return 0;
}
