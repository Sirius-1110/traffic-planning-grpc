#include "TNM_utility.h"
//#include <direct.h>
//#include <atlstr.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>

#ifndef _WIN32
#include <dlfcn.h>
#endif

struct pg_conn;
struct pg_result;
typedef unsigned int Oid;
enum ConnStatusType { CONNECTION_OK, CONNECTION_BAD };
enum ExecStatusType { PGRES_EMPTY_QUERY = 0, PGRES_COMMAND_OK, PGRES_TUPLES_OK, PGRES_COPY_OUT, PGRES_COPY_IN, PGRES_BAD_RESPONSE, PGRES_NONFATAL_ERROR, PGRES_FATAL_ERROR };

namespace
{
    typedef pg_conn* (*TNM_PQconnectdb)(const char *);
    typedef void (*TNM_PQfinish)(pg_conn *);
    typedef ConnStatusType (*TNM_PQstatus)(const pg_conn *);
    typedef char* (*TNM_PQerrorMessage)(const pg_conn *);
    typedef pg_result* (*TNM_PQexec)(pg_conn *, const char *);
    typedef ExecStatusType (*TNM_PQresultStatus)(const pg_result *);
    typedef char* (*TNM_PQresultErrorMessage)(const pg_result *);
    typedef int (*TNM_PQntuples)(const pg_result *);
    typedef int (*TNM_PQnfields)(const pg_result *);
    typedef char* (*TNM_PQgetvalue)(const pg_result *, int, int);
    typedef int (*TNM_PQgetisnull)(const pg_result *, int, int);
    typedef void (*TNM_PQclear)(pg_result *);

    struct TNM_PG_API
    {
        void *module;
        TNM_PQconnectdb PQconnectdb;
        TNM_PQfinish PQfinish;
        TNM_PQstatus PQstatus;
        TNM_PQerrorMessage PQerrorMessage;
        TNM_PQexec PQexec;
        TNM_PQresultStatus PQresultStatus;
        TNM_PQresultErrorMessage PQresultErrorMessage;
        TNM_PQntuples PQntuples;
        TNM_PQnfields PQnfields;
        TNM_PQgetvalue PQgetvalue;
        TNM_PQgetisnull PQgetisnull;
        TNM_PQclear PQclear;
        TNM_PG_API() : module(NULL), PQconnectdb(NULL), PQfinish(NULL), PQstatus(NULL), PQerrorMessage(NULL), PQexec(NULL), PQresultStatus(NULL), PQresultErrorMessage(NULL), PQntuples(NULL), PQnfields(NULL), PQgetvalue(NULL), PQgetisnull(NULL), PQclear(NULL) {}
    };

    TNM_PG_API g_pgApi;

    string TNM_TrimCopy(const string &value)
    {
        string result = value;
        while(!result.empty() && isspace(static_cast<unsigned char>(result.front()))) result.erase(result.begin());
        while(!result.empty() && isspace(static_cast<unsigned char>(result.back()))) result.erase(result.end() - 1);
        return result;
    }

    string TNM_ToLowerCopy(string value)
    {
        transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return static_cast<char>(tolower(c)); });
        return value;
    }

    string TNM_GetEnv(const char *name)
    {
        string value;
#ifdef _WIN32
        char *buffer = NULL;
        size_t len = 0;
        if(_dupenv_s(&buffer, &len, name) == 0 && buffer != NULL)
        {
            value = buffer;
            free(buffer);
        }
#else
        const char *buffer = getenv(name);
        if(buffer != NULL)
        {
            value = buffer;
        }
#endif
        return value;
    }

    void TNM_ParseKeyValueSpec(const string &spec, map<string, string> &values)
    {
        string payload = spec;
        if(TNM_ToLowerCopy(payload).find("db:") == 0)
        {
            payload = payload.substr(3);
        }
        vector<string> parts;
        TNM_GetWordsFromLine(payload, parts, ';');
        for(size_t i = 0; i < parts.size(); ++i)
        {
            string item = TNM_TrimCopy(parts[i]);
            if(item.empty()) continue;
            size_t pos = item.find('=');
            if(pos == string::npos) continue;
            string key = TNM_ToLowerCopy(TNM_TrimCopy(item.substr(0, pos)));
            string value = TNM_TrimCopy(item.substr(pos + 1));
            if(!key.empty()) values[key] = value;
        }
    }

    bool TNM_IsIdentifierToken(const string &value)
    {
        if(value.empty()) return false;
        for(size_t i = 0; i < value.size(); ++i)
        {
            char c = value[i];
            if(!(isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        }
        return true;
    }

    string TNM_QuoteIdentifierPart(const string &value)
    {
        string quoted = "\"";
        for(size_t i = 0; i < value.size(); ++i)
        {
            if(value[i] == '\"') quoted += "\"\"";
            else quoted += value[i];
        }
        quoted += "\"";
        return quoted;
    }

    bool TNM_LoadPgApi(string &errorMessage)
    {
        if(g_pgApi.module != NULL) return true;
#ifdef _WIN32
        g_pgApi.module = LoadLibraryA("libpq.dll");
#else
        g_pgApi.module = dlopen("libpq.so", RTLD_LAZY);
        if(g_pgApi.module == NULL) g_pgApi.module = dlopen("libpq.so.5", RTLD_LAZY);
#endif
        if(g_pgApi.module == NULL)
        {
            errorMessage = "Cannot load libpq library";
            return false;
        }
#ifdef _WIN32
        g_pgApi.PQconnectdb = reinterpret_cast<TNM_PQconnectdb>(GetProcAddress((HMODULE)g_pgApi.module, "PQconnectdb"));
        g_pgApi.PQfinish = reinterpret_cast<TNM_PQfinish>(GetProcAddress((HMODULE)g_pgApi.module, "PQfinish"));
        g_pgApi.PQstatus = reinterpret_cast<TNM_PQstatus>(GetProcAddress((HMODULE)g_pgApi.module, "PQstatus"));
        g_pgApi.PQerrorMessage = reinterpret_cast<TNM_PQerrorMessage>(GetProcAddress((HMODULE)g_pgApi.module, "PQerrorMessage"));
        g_pgApi.PQexec = reinterpret_cast<TNM_PQexec>(GetProcAddress((HMODULE)g_pgApi.module, "PQexec"));
        g_pgApi.PQresultStatus = reinterpret_cast<TNM_PQresultStatus>(GetProcAddress((HMODULE)g_pgApi.module, "PQresultStatus"));
        g_pgApi.PQresultErrorMessage = reinterpret_cast<TNM_PQresultErrorMessage>(GetProcAddress((HMODULE)g_pgApi.module, "PQresultErrorMessage"));
        g_pgApi.PQntuples = reinterpret_cast<TNM_PQntuples>(GetProcAddress((HMODULE)g_pgApi.module, "PQntuples"));
        g_pgApi.PQnfields = reinterpret_cast<TNM_PQnfields>(GetProcAddress((HMODULE)g_pgApi.module, "PQnfields"));
        g_pgApi.PQgetvalue = reinterpret_cast<TNM_PQgetvalue>(GetProcAddress((HMODULE)g_pgApi.module, "PQgetvalue"));
        g_pgApi.PQgetisnull = reinterpret_cast<TNM_PQgetisnull>(GetProcAddress((HMODULE)g_pgApi.module, "PQgetisnull"));
        g_pgApi.PQclear = reinterpret_cast<TNM_PQclear>(GetProcAddress((HMODULE)g_pgApi.module, "PQclear"));
#else
        g_pgApi.PQconnectdb = reinterpret_cast<TNM_PQconnectdb>(dlsym(g_pgApi.module, "PQconnectdb"));
        g_pgApi.PQfinish = reinterpret_cast<TNM_PQfinish>(dlsym(g_pgApi.module, "PQfinish"));
        g_pgApi.PQstatus = reinterpret_cast<TNM_PQstatus>(dlsym(g_pgApi.module, "PQstatus"));
        g_pgApi.PQerrorMessage = reinterpret_cast<TNM_PQerrorMessage>(dlsym(g_pgApi.module, "PQerrorMessage"));
        g_pgApi.PQexec = reinterpret_cast<TNM_PQexec>(dlsym(g_pgApi.module, "PQexec"));
        g_pgApi.PQresultStatus = reinterpret_cast<TNM_PQresultStatus>(dlsym(g_pgApi.module, "PQresultStatus"));
        g_pgApi.PQresultErrorMessage = reinterpret_cast<TNM_PQresultErrorMessage>(dlsym(g_pgApi.module, "PQresultErrorMessage"));
        g_pgApi.PQntuples = reinterpret_cast<TNM_PQntuples>(dlsym(g_pgApi.module, "PQntuples"));
        g_pgApi.PQnfields = reinterpret_cast<TNM_PQnfields>(dlsym(g_pgApi.module, "PQnfields"));
        g_pgApi.PQgetvalue = reinterpret_cast<TNM_PQgetvalue>(dlsym(g_pgApi.module, "PQgetvalue"));
        g_pgApi.PQgetisnull = reinterpret_cast<TNM_PQgetisnull>(dlsym(g_pgApi.module, "PQgetisnull"));
        g_pgApi.PQclear = reinterpret_cast<TNM_PQclear>(dlsym(g_pgApi.module, "PQclear"));
#endif
        if(g_pgApi.PQconnectdb == NULL || g_pgApi.PQfinish == NULL || g_pgApi.PQstatus == NULL || g_pgApi.PQerrorMessage == NULL || g_pgApi.PQexec == NULL || g_pgApi.PQresultStatus == NULL || g_pgApi.PQresultErrorMessage == NULL || g_pgApi.PQntuples == NULL || g_pgApi.PQnfields == NULL || g_pgApi.PQgetvalue == NULL || g_pgApi.PQgetisnull == NULL || g_pgApi.PQclear == NULL)
        {
            errorMessage = "libpq library does not expose required symbols";
#ifdef _WIN32
            FreeLibrary((HMODULE)g_pgApi.module);
#else
            dlclose(g_pgApi.module);
#endif
            g_pgApi = TNM_PG_API();
            return false;
        }
        return true;
    }
}

int TNM_FloatFormat::sDigit   = 2;
int TNM_FloatFormat::sWidth   = 10;
int TNM_IntFormat::sWidth     = 8;

TNM_DB_IO_CONFIG::TNM_DB_IO_CONFIG()
{
    enabled = false;
}

bool TNM_IsDbIOSpec(const string &spec)
{
    return TNM_ToLowerCopy(TNM_TrimCopy(spec)).find("db:") == 0;
}

bool TNM_ParseDbIOConfig(const string &inSpec, const string &outSpec, TNM_DB_IO_CONFIG &config, string &errorMessage)
{
    config = TNM_DB_IO_CONFIG();
    config.conninfo = TNM_GetEnv("TNA_DB_CONNINFO");
    config.defaultSchema = TNM_GetEnv("TNA_DB_SCHEMA");
    config.nodeTable = TNM_GetEnv("TNA_DB_NODE_TABLE");
    config.linkTable = TNM_GetEnv("TNA_DB_LINK_TABLE");
    config.demandTable = TNM_GetEnv("TNA_DB_MAT_TABLE");
    config.stopTable = TNM_GetEnv("TNA_DB_STOP_TABLE");
    config.routeTable = TNM_GetEnv("TNA_DB_ROUTE_TABLE");
    config.shapeTable = TNM_GetEnv("TNA_DB_SHAPE_TABLE");
    config.transitTable = TNM_GetEnv("TNA_DB_TRANSIT_TABLE");
    config.walkTable = TNM_GetEnv("TNA_DB_WALK_TABLE");
    config.tripTable = TNM_GetEnv("TNA_DB_TRIP_TABLE");
    config.linkResultTable = TNM_GetEnv("TNA_DB_LINK_RESULT_TABLE");
    config.pathResultTable = TNM_GetEnv("TNA_DB_PATH_RESULT_TABLE");
    config.iterResultTable = TNM_GetEnv("TNA_DB_LOG_TABLE");
    config.summaryResultTable = TNM_GetEnv("TNA_DB_SUMMARY_TABLE");
    map<string, string> values;
    if(TNM_IsDbIOSpec(inSpec))
    {
        config.enabled = true;
        TNM_ParseKeyValueSpec(inSpec, values);
    }
    if(TNM_IsDbIOSpec(outSpec))
    {
        config.enabled = true;
        TNM_ParseKeyValueSpec(outSpec, values);
    }
    if(!config.enabled && (!config.conninfo.empty() || !config.nodeTable.empty() || !config.linkTable.empty() || !config.demandTable.empty()))
    {
        config.enabled = true;
    }
    if(values.find("conninfo") != values.end()) config.conninfo = values["conninfo"];
    if(values.find("schema") != values.end()) config.defaultSchema = values["schema"];
    if(values.find("node_table") != values.end()) config.nodeTable = values["node_table"];
    if(values.find("link_table") != values.end()) config.linkTable = values["link_table"];
    if(values.find("demand_table") != values.end()) config.demandTable = values["demand_table"];
    if(values.find("mat_table") != values.end()) config.demandTable = values["mat_table"];
    if(values.find("stop_table") != values.end()) config.stopTable = values["stop_table"];
    if(values.find("route_table") != values.end()) config.routeTable = values["route_table"];
    if(values.find("shape_table") != values.end()) config.shapeTable = values["shape_table"];
    if(values.find("transit_table") != values.end()) config.transitTable = values["transit_table"];
    if(values.find("walk_table") != values.end()) config.walkTable = values["walk_table"];
    if(values.find("trip_table") != values.end()) config.tripTable = values["trip_table"];
    if(values.find("link_result_table") != values.end()) config.linkResultTable = values["link_result_table"];
    if(values.find("path_result_table") != values.end()) config.pathResultTable = values["path_result_table"];
    if(values.find("iter_result_table") != values.end()) config.iterResultTable = values["iter_result_table"];
    if(values.find("log_table") != values.end()) config.iterResultTable = values["log_table"];
    if(values.find("summary_table") != values.end()) config.summaryResultTable = values["summary_table"];
    if(!config.enabled) return true;
    if(config.conninfo.empty())
    {
        errorMessage = "Missing PostgreSQL conninfo. Provide conninfo=... or TNA_DB_CONNINFO.";
        return false;
    }
    bool hasTrafficInput = !config.nodeTable.empty() && !config.linkTable.empty() && !config.demandTable.empty();
    bool hasTransitInput = !config.stopTable.empty() && !config.routeTable.empty() && !config.shapeTable.empty() && !config.transitTable.empty() && !config.tripTable.empty();
    if(!hasTrafficInput && !hasTransitInput)
    {
        errorMessage = "Missing input tables. Provide either node_table, link_table, and demand_table; or stop_table, route_table, shape_table, transit_table, and trip_table.";
        return false;
    }
    return true;
}

bool TNM_ResolveTableName(const string &tableName, const string &defaultSchema, string &qualifiedName, string &errorMessage)
{
    string value = TNM_TrimCopy(tableName);
    if(value.empty())
    {
        errorMessage = "Table name is empty";
        return false;
    }
    string schemaPart;
    string tablePart;
    size_t dot = value.find('.');
    if(dot == string::npos)
    {
        tablePart = value;
        schemaPart = TNM_TrimCopy(defaultSchema);
    }
    else
    {
        if(value.find('.', dot + 1) != string::npos)
        {
            errorMessage = "Only schema.table or table is supported for PostgreSQL table parameters";
            return false;
        }
        schemaPart = TNM_TrimCopy(value.substr(0, dot));
        tablePart = TNM_TrimCopy(value.substr(dot + 1));
    }
    if(!TNM_IsIdentifierToken(tablePart))
    {
        errorMessage = "Invalid table identifier: " + tablePart;
        return false;
    }
    qualifiedName = TNM_QuoteIdentifierPart(tablePart);
    if(!schemaPart.empty())
    {
        if(!TNM_IsIdentifierToken(schemaPart))
        {
            errorMessage = "Invalid schema identifier: " + schemaPart;
            return false;
        }
        qualifiedName = TNM_QuoteIdentifierPart(schemaPart) + "." + qualifiedName;
    }
    return true;
}

string TNM_QuotePgLiteral(const string &value)
{
    string escaped = "'";
    for(size_t i = 0; i < value.size(); ++i)
    {
        if(value[i] == '\'') escaped += "''";
        else escaped += value[i];
    }
    escaped += "'";
    return escaped;
}

TNM_PGDB::TNM_PGDB()
{
    m_conn = NULL;
}

TNM_PGDB::~TNM_PGDB()
{
    Close();
}

bool TNM_PGDB::Open(const string &conninfo, string &errorMessage)
{
    Close();
    if(!TNM_LoadPgApi(errorMessage)) return false;
    m_conn = g_pgApi.PQconnectdb(conninfo.c_str());
    if(m_conn == NULL)
    {
        errorMessage = "PQconnectdb failed";
        return false;
    }
    if(g_pgApi.PQstatus(reinterpret_cast<pg_conn*>(m_conn)) != CONNECTION_OK)
    {
        errorMessage = g_pgApi.PQerrorMessage(reinterpret_cast<pg_conn*>(m_conn));
        Close();
        return false;
    }
    return true;
}

bool TNM_PGDB::Execute(const string &sql, string &errorMessage)
{
    if(m_conn == NULL)
    {
        errorMessage = "Database connection is not open";
        return false;
    }
    pg_result *result = g_pgApi.PQexec(reinterpret_cast<pg_conn*>(m_conn), sql.c_str());
    if(result == NULL)
    {
        errorMessage = g_pgApi.PQerrorMessage(reinterpret_cast<pg_conn*>(m_conn));
        return false;
    }
    ExecStatusType status = g_pgApi.PQresultStatus(result);
    bool ok = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
    if(!ok)
    {
        errorMessage = g_pgApi.PQresultErrorMessage(result);
    }
    g_pgApi.PQclear(result);
    return ok;
}

bool TNM_PGDB::Query(const string &sql, vector<vector<string> > &rows, string &errorMessage)
{
    rows.clear();
    if(m_conn == NULL)
    {
        errorMessage = "Database connection is not open";
        return false;
    }
    pg_result *result = g_pgApi.PQexec(reinterpret_cast<pg_conn*>(m_conn), sql.c_str());
    if(result == NULL)
    {
        errorMessage = g_pgApi.PQerrorMessage(reinterpret_cast<pg_conn*>(m_conn));
        return false;
    }
    if(g_pgApi.PQresultStatus(result) != PGRES_TUPLES_OK)
    {
        errorMessage = g_pgApi.PQresultErrorMessage(result);
        g_pgApi.PQclear(result);
        return false;
    }
    int tupleCount = g_pgApi.PQntuples(result);
    int fieldCount = g_pgApi.PQnfields(result);
    for(int rowIndex = 0; rowIndex < tupleCount; ++rowIndex)
    {
        vector<string> row;
        for(int colIndex = 0; colIndex < fieldCount; ++colIndex)
        {
            if(g_pgApi.PQgetisnull(result, rowIndex, colIndex)) row.push_back("");
            else row.push_back(g_pgApi.PQgetvalue(result, rowIndex, colIndex));
        }
        rows.push_back(row);
    }
    g_pgApi.PQclear(result);
    return true;
}

void TNM_PGDB::Close()
{
    if(m_conn != NULL && g_pgApi.PQfinish != NULL)
    {
        g_pgApi.PQfinish(reinterpret_cast<pg_conn*>(m_conn));
    }
    m_conn = NULL;
}

ifstream &TNM_SkipString(ifstream &in, int count)
{
    string skip;
    for (int i = 0;i<count;i++)
        in>>skip;
    return in;
}

void TNM_GetWordsFromLine(const string &line, vector<string> &words, const char dim, const char exception)
{
    int count = 0;
    for (int i = 0; i < line.size(); i++)
        if (line[i] == dim) count++;
    if(!words.empty()) words.clear();
    string curWord;
    istringstream pstr(line);
    if(exception == ' ')
    {
        while(getline(pstr, curWord, dim))
        {
            const int strBegin = curWord.find_first_not_of(" \t");
            if (strBegin == std::string::npos)
                curWord = " ";

            const int strEnd = curWord.find_last_not_of(" \t");
            const int strRange = strEnd - strBegin + 1;
            if(strBegin == std::string::npos && strEnd == std::string::npos)
                words.push_back(curWord.substr(0, strRange));
            else
                words.push_back(curWord.substr(strBegin, strRange));
        }
    }
    else
    {
        bool readbefore = false;
        while(getline(pstr, curWord,exception))
        {
            if(!readbefore)
            {
                readbefore = true;
                if(!curWord.empty())
                {
                    istringstream tstr(curWord);
                    string pword;
                    if(getline(tstr, pword, dim))
                    {
                        if(!pword.empty())
                        {
                            words.push_back(pword);
                        }
                    }
                    while(getline(tstr, pword, dim))
                    {
                        words.push_back(pword);
                    }
                }
            }
            else
            {
                words.push_back(curWord);
                for (int i = 0; i < curWord.size(); i++)
                    if (curWord[i] == dim) count--;

                readbefore = false;
            }
        }
    }
    if(words.size() == count)
        words.push_back("  ");
}

void TNM_GetWordsFromLine(string &pstr, vector<string> &words)
{
    if(!words.empty()) words.clear();
    istringstream x(pstr.c_str());
    copy( istream_iterator< string >( x ), istream_iterator<string>(),  back_inserter( words ) );
}

floatType TNM_Position::GetDist(TNM_Position *pos, char unit)
{
    floatType theta, dist;
    theta = m_lon - pos->GetLongitude();
    dist = sin(deg2rad(m_lat)) * sin(deg2rad(pos->GetLatitude())) + cos(deg2rad(m_lat)) * cos(deg2rad(pos->GetLatitude())) * cos(deg2rad(theta));
    if (dist<-1||dist>1)
        dist = 0;
    else
        dist = acos(dist);

    dist = rad2deg(dist);
    dist = dist * 60 * 1.1515;
    switch(unit) {
    case 'M':
      break;
    case 'K':
      dist = dist * 1.609344;
      break;
    case 'N':
      dist = dist * 0.8684;
      break;
  }
  return (dist);
}

ostream& operator <<(ostream& os, const TNM_FloatFormat& m)
{
    return m.print(os);
}

TNM_FloatFormat::TNM_FloatFormat(const floatType x, int w, int d)
{
    num   = x;
    width = w;
    digit = d;
}
TNM_FloatFormat::TNM_FloatFormat(const floatType x, int w)
{
    num   = x;
    width = w;
    digit = sDigit;
}
TNM_FloatFormat::TNM_FloatFormat(const floatType x)
{
    num   = x;
    width = sWidth;
    digit = sDigit;
}

ostream& TNM_FloatFormat::print(ostream& os) const {
    int ad;
    if (num >=0)  ad = 2;
    else          ad = 3;
    if (width - ad - digit >0)
    {
        if(fabs(num)<pow(10.0, 1.0*(width-ad-digit)))
        {
            if(fabs(num) > pow(10.0, -digit-1.0)) os<<setw(width)<<setprecision(digit)<<setiosflags(ios::fixed)<<num;
            else
            {
                os.unsetf(ios::fixed);
                os<<setw(width)<<setprecision(digit)<<setiosflags(ios::scientific)<<num;
                os.unsetf(ios::scientific);
            }
        }
        else
        {
            if (width - digit - 5 - ad >0)
            {
                os.unsetf(ios::fixed);
                os<<setw(width)<<setprecision(digit)<<setiosflags(ios::scientific)<<num;
                os.unsetf(ios::scientific);
            }
            else
                os<<" "<<setprecision(digit)<<setiosflags(ios::fixed)<<num;
        }
    }
    else
    {
        if(fabs(num) > pow(10.0, -digit-1.0)) os<<" "<<setprecision(digit)<<setiosflags(ios::fixed)<<num;
        else
        {
            os.unsetf(ios::fixed);
            os<<setw(width)<<setprecision(digit)<<setiosflags(ios::scientific)<<num;
            os.unsetf(ios::scientific);
        }
    }
    return os;
}

ostream& operator <<(ostream& os, const TNM_IntFormat& m)
{
return m.print(os);
}

TNM_IntFormat::TNM_IntFormat(const int x, int w)
{
    num   = x;
    width = w;
}

TNM_IntFormat::TNM_IntFormat(const int x)
{
    num   = x;
    width = sWidth;
}
ostream& TNM_IntFormat::print(ostream& os) const {
    int ad;
    if (num >=0)  ad = 1;
    else          ad = 2;
    if (width - ad >0)
    {
        if(fabs(num)<pow(10.0, 1.0*(width-ad)))
        {
            os<<setw(width)<<setiosflags(ios::fixed)<<setprecision(0)<<num;
        }
        else
        {
            os<<" "<<setiosflags(ios::fixed)<<setprecision(0)<<num;
        }
    }
    else
    {
        os<<" "<<setiosflags(ios::fixed)<<setprecision(0)<<num;
    }
    return os;
}

bool TNM_OpenInFile(ifstream &in, const string &file)
{
    in.open(file.c_str(),ios::in);
    if(!in)
    {
        cout<<"\n\tCannot open file "<<file<<" to read."<<endl;
        return false;
    }
    return true;
}

bool TNM_OpenOutFile(ofstream &out, const string &file)
{
    out.open(file.c_str(),ios::out);
    if(!out)
    {
        cout<<"\n\tCannot open file "<<file<<" to write."<<endl;
        return false;
    }
    return true;
}

TNM_MyDateTime::TNM_MyDateTime()
{
   time_t ct = time(NULL);
   Initialize(ct);
}

TNM_MyDateTime::TNM_MyDateTime(TNM_MyDateTime &rhs)
{
    struct tm* ptm = rhs.GetTime();
    m_tm = *ptm;
}

TNM_MyDateTime::~TNM_MyDateTime()
{
}

bool TNM_MyDateTime::ChangTime(long seconds)
{
    time_t pt = mktime(&m_tm);
    pt+= seconds;
    return Initialize(pt);
}

long TNM_MyDateTime::DiffTime(TNM_MyDateTime *rhs)
{
    return GetUnixTime() - rhs->GetUnixTime();
}
bool TNM_MyDateTime::Initialize(time_t t)
{    
#ifdef _WIN32
    if(localtime_s(&m_tm, &t) !=0)   
#else
    if(localtime_r(&t, &m_tm) == NULL)
#endif
    {
        cout<<"\tFailed to initialize MyDateTime object."<<endl;
        return false;
    }
    else    return true;
    
}

bool TNM_MyDateTime::InitializeCompactAll(const string &inf)
{
    //string inf(str);
   // trim(inf);
    if(inf.size()!=14)
    {
        cout<<"\tinvalid format: 14 digit expected, "<<inf.size()<<" found"<<endl;
        return false;
    }
    else
    {
        string string1 = inf.substr(0,8);
        string string2 = inf.substr(8, 6);
        if(!InitializeCompact(string1, true)) return false;
        return InitializeCompact(string2, false);
    }
}
bool TNM_MyDateTime::InitializeExpandAll(const string &inf)
{
    //string inf(str);
    //trim(inf);
    if(inf.size()!=19 && inf.size()!=18) //allows the format 06:00:00 and 6:00:00.
    {
        cout<<"\tinvalide format: 18 or 19 digit expected, "<<inf.size()<<" found"<<endl;
        return false;
    }
    else
    {
        string string1 = inf.substr(0,10);
        //cout<<string1<<endl;
        string string2 = inf.substr(11, inf.size() - 11);//
        if(string2.size() == 7) string2 = "0" + string2;
       // cout<<string2<<endl;
        if(!InitializeExpand(string1, true)) return false;
        return InitializeExpand(string2, false);
    }
}

string TNM_MyDateTime::GetDateStringMonthFirst()
{
    char buffer [12];
    strftime (buffer,12,"%m/%d/%Y",&m_tm);
    string pout(buffer);
    return pout;
}
string TNM_MyDateTime::GetDateString(bool shortformat)
{
    if(shortformat)
    {
        char buffer [12];
        strftime (buffer,12,"%Y-%m-%d",&m_tm);
        string pout(buffer);
        return pout;
    }
    else
    {
        char buffer[40];
        strftime (buffer,40,"%B %d, %Y, %A",&m_tm);
        string pout(buffer);
        return pout;
    }
}
string TNM_MyDateTime::GetDateTimeString(bool shortformat)
{
    ostringstream pstr;
    pstr<<GetDateString(shortformat)<<" "<<GetTimeString(shortformat);
    return pstr.str();
}
string TNM_MyDateTime::GetTimeString(bool shortformat)
{
//    %a	Abbreviated weekday name *	Thu
//%A	Full weekday name * 	Thursday
//%b	Abbreviated month name *	Aug
//%B	Full month name *	August
//%c	Date and time representation *	Thu Aug 23 14:55:02 2001
//%d	Day of the month (01-31)	23
//%H	Hour in 24h format (00-23)	14
//%I	Hour in 12h format (01-12)	02
//%j	Day of the year (001-366)	235
//%m	Month as a decimal number (01-12)	08
//%M	Minute (00-59)	55
//%p	AM or PM designation	PM
//%S	Second (00-61)	02
//%U	Week number with the first Sunday as the first day of week one (00-53)	33
//%w	Weekday as a decimal number with Sunday as 0 (0-6)	4
//%W	Week number with the first Monday as the first day of week one (00-53)	34
//%x	Date representation *	08/23/01
//%X	Time representation *	14:55:02
//%y	Year, last two digits (00-99)	01
//%Y	Year	2001
//%Z	Timezone name or abbreviation	CDT
//%%	A % sign	%
    if(shortformat)
    {
        char buffer [12];
        strftime (buffer,12,"%H:%M:%S",&m_tm);
        string pout(buffer);
        return pout;
    }
    else
    {
        char buffer[20];
        strftime (buffer,20,"%I:%M:%S %p",&m_tm);
        string pout(buffer);
        return pout;
    }
}


bool TNM_MyDateTime::InitializeExpand(const string &inf, bool date)
{
    //string inf(str);
    //trim(inf);
    if(date)
    {
        if(inf.size()!=10) 
        {
            cout<<"\tinvalide format: 10 digit expected, "<<inf.size()<<" found"<<endl;
            return false;
        }
    }
    else
    {
        if(inf.size()!=8 ) 
        {
            cout<<"\tinvalide format:  8 digit expected, "<<inf.size()<<" found"<<endl;
            return false;
        }
    }

    vector<string> words;
     string newinf;

    if(date)
    {
        TNM_GetWordsFromLine(inf,words,'-');            
    }
    else
    {
        TNM_GetWordsFromLine(inf,words,':');

    }
    int formatStatus = 0;
    if(words.size()!=3)
    {
        if(date)
        {
            TNM_GetWordsFromLine(inf, words, '/');
           // cout<<inf<<endl;
            if(words.size()==3)
            {
                formatStatus = 1;
            }
            else
                formatStatus = 2;
        }
        else
        {
                formatStatus = 3;
        }
        
    }
 //   cout<<"invalid DateTime input."<<endl;
   // return false;
    switch(formatStatus)
    {
    case 0:
        for(int i = 0 ;i < 3;i++) 
        {
           newinf+=words[i];
        }
        break;
    case 1:
        newinf+=words[2];
        for(int i = 0;i<=1;i++) newinf+=words[i];
        break;
    case 2:        
    case 3:
        cout<<"invalid DateTime input. Format status = "<<formatStatus<<endl;
        return false;

    }

    

    return InitializeCompact(newinf, date);
}


bool TNM_MyDateTime::InitializeCompact(const string &str, bool date)
{
   // string str(inf);
   // trim(str);
    if(date)
    {
        if(str.size()!=8) 
        {
            cout<<"\tinvalid format: 8 digit expected, "<<str.size()<<" found"<<endl;
            return false;
        }
    }
   
    else 
    {
        if(str.size()!=6) 
        {
            cout<<"\tinvalid format: 6 digit expected, "<<str.size()<<" found"<<endl;
            return false;
        }
    }
       /*tm_sec	seconds after the minute	0-61*
        tm_min	minutes after the hour	0-59
        tm_hour	hours since midnight	0-23
        tm_mday	day of the month	1-31
        tm_mon	months since January	0-11
        tm_year	years since 1900	
        tm_wday	days since Sunday	0-6
        tm_yday	days since January 1	0-365
        tm_isdst	Daylight Saving Time flag*/
        int a, b, c;              
        if(date)
        {
            if(!TNM_FromString(a, str.substr(0,4), std::dec)) return false;
            if(!TNM_FromString(b, str.substr(4,2), std::dec)) return false;
            if(!TNM_FromString(c, str.substr(6,2), std::dec)) return false;  
            //cout<<"year = "<<a<<" month = "<<b<<" day = "<<c<<endl;
            m_tm.tm_year  = a  - 1900;
	        m_tm.tm_mon   = b - 1; //month range between 
	        m_tm.tm_mday  = c;
        }
        else
        {
            if(!TNM_FromString(a, str.substr(0,2), std::dec)) return false;
            if(!TNM_FromString(b, str.substr(2,2), std::dec)) return false;
            if(!TNM_FromString(c, str.substr(4,2), std::dec)) return false;  
            m_tm.tm_hour = a;
            m_tm.tm_min  = b;
            m_tm.tm_sec  = c;
        }
        mktime(&m_tm);//this function set tm_wday, tm_yday. 
       // cout<<this->GetDateString(false)<<endl;
        return true;

}

string TNM_MyDateTime::GetCompactString(bool dateonly)
{
    if(dateonly)
    {
        char buffer [10];
        strftime (buffer,10,"%Y%m%d",&m_tm);
        string pout(buffer);
        return pout;   
    }
    else
    {
        char buffer [16];
        strftime (buffer,16,"%Y%m%d%H%M%S",&m_tm);
        string pout(buffer);
        return pout;

    }
}

//implementation of IDManager
IDManager::IDManager()
{
}

void IDManager::RegisterID(int id)
{
	idSet.insert(id);
	 
	//if(result.second
	if(!idCache.empty()) idCache.erase(id);
}


bool IDManager::FindID(int id)
{
	INT_SET::const_iterator p = idSet.find(id);
	if(p!=idSet.end()) return true;
	else return false;
}
void IDManager::UnRegisterID(int id)
{
	if(!idSet.empty()) idSet.erase(id);
	//idCache.insert(id);
}

int IDManager::SelectANewID()
{
	if(idSet.empty()) return 1;
	int maxVal = *(idSet.rbegin()), minVal = *(idSet.begin());
	int bandWidth =  maxVal - minVal + 1;
	if (bandWidth == idSet.size())
		return maxVal + 1;
	else
	{
		INT_SET::const_iterator p;
		if(!idCache.empty())
		{
			p = idCache.begin();
			return *p;
		}
		{
			int curVal = minVal;
			for (p = idSet.begin(); p!=idSet.end(); p++)
			{
				if(curVal != *p) 
				{
					idCache.insert(curVal);
					p--;
				}
				curVal++;
			}
		}
		if(!idCache.empty())
		{
			p = idCache.begin();
			return *p;
		}
		else
		{
			return -999999;
		}

	}
}