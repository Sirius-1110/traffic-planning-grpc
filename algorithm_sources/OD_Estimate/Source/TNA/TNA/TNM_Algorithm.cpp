#include "header/stdafx.h"
#include <math.h>
#include <stack>
#include <set>
#include <algorithm>
#include <errno.h>
#include <fstream>
#include <iomanip>
#include <libpq-fe.h>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include "header/TNM_Net.h"
#include <cstring>

static void TNM_PGNoticeProcessor(void* arg, const char* message)
{
    if (message == NULL) return;
    string text = message;
    if (text.find("already exists") != string::npos) return;
    cerr << "PostgreSQL 提示: " << text;
}

void OD_ESTIMATION::SetUseRoadwayObserved(bool v)
{
    useRoadwayObserved = v;
}

void OD_ESTIMATION::SetObservedFlowScale(double s)
{
    observedFlowScale = s;
}

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

static inline bool TNM_IsFinite(double v)
{
    return (v == v) && (v != numeric_limits<double>::infinity()) && (v != -numeric_limits<double>::infinity());
}

static inline long long TNM_MakeODKey(int o, int d)
{
    return ((long long)o << 32) ^ (unsigned int)d;
}

static unordered_map<long long, int> g_ofw_null_via_count;
static unordered_map<long long, int> g_ofw_null_via_first_node;
static unordered_set<long long> g_ofw_null_via_printed;
static size_t g_ofw_null_via_last_reported_size = (size_t)(-1);
static size_t g_ofw_unreachable_warning_examples = 30;

TNM_EXT_CLASS void TNM_AppendMessageNote(const string& note);

TNM_EXT_CLASS void TNM_SetOfwUnreachableWarningExamples(int n)
{
	if (n < 0) n = 0;
	g_ofw_unreachable_warning_examples = (size_t)n;
}

static inline void TNM_ResetOfwUnreachableStats()
{
	g_ofw_null_via_count.clear();
	g_ofw_null_via_first_node.clear();
	g_ofw_null_via_printed.clear();
	g_ofw_null_via_last_reported_size = (size_t)(-1);
}

static inline void TNM_AppendOfwUnreachableNotes(size_t maxExamples)
{
	if (g_ofw_null_via_count.empty()) return;
	vector<long long> keys;
	keys.reserve(g_ofw_null_via_count.size());
	for (unordered_map<long long, int>::const_iterator it = g_ofw_null_via_count.begin(); it != g_ofw_null_via_count.end(); ++it)
	{
		keys.push_back(it->first);
	}
	sort(keys.begin(), keys.end(), [](long long a, long long b) {
		int oa = (int)(a >> 32);
		unsigned int da = (unsigned int)(a & 0xFFFFFFFF);
		int ob = (int)(b >> 32);
		unsigned int db = (unsigned int)(b & 0xFFFFFFFF);
		if (oa != ob) return oa < ob;
		return da < db;
	});
	size_t showN = 0;
	if (maxExamples > 0)
	{
		showN = keys.size() < maxExamples ? keys.size() : maxExamples;
	}
	ostringstream oss;
	oss << "Warning: OFW 不可达 OD 对（via==NULL）共 " << (unsigned long long)g_ofw_null_via_count.size() << " 对";
	if (showN > 0)
	{
		oss << "；以下列出前 " << (unsigned long long)showN << " 对示例：";
		for (size_t i = 0; i < showN; ++i)
		{
			long long k = keys[i];
			int o = (int)(k >> 32);
			int d = (int)((unsigned int)k);
			int firstNode = 0;
			unordered_map<long long, int>::const_iterator fn = g_ofw_null_via_first_node.find(k);
			if (fn != g_ofw_null_via_first_node.end()) firstNode = fn->second;
			int hit = 0;
			unordered_map<long long, int>::const_iterator ct = g_ofw_null_via_count.find(k);
			if (ct != g_ofw_null_via_count.end()) hit = ct->second;
			oss << "\n  origin=" << o << ", dest=" << d << ", first_node=" << firstNode << ", hit=" << hit;
		}
	}
	TNM_AppendMessageNote(oss.str());
}

static string& TNM_LastErrorStorage()
{
    static string lastError;
    return lastError;
}

void TNM_ResetLastError()
{
    TNM_LastErrorStorage().clear();
}

void TNM_SetLastError(const string& message)
{
    TNM_LastErrorStorage() = message;
}

const string& TNM_GetLastError()
{
    return TNM_LastErrorStorage();
}

// 预检查/提示信息累积（返回给最终 JSON message 使用）
static string& TNM_MessageNotesStorage()
{
    static string notes;
    return notes;
}

TNM_EXT_CLASS void TNM_ResetMessageNotes()
{
    TNM_MessageNotesStorage().clear();
}

TNM_EXT_CLASS void TNM_AppendMessageNote(const string& note)
{
    if (note.empty()) return;
    string& s = TNM_MessageNotesStorage();
    if (!s.empty()) s.append("\n");
    s.append(note);
}

TNM_EXT_CLASS const string& TNM_GetMessageNotes()
{
    return TNM_MessageNotesStorage();
}


int  TNM_TAP::intWidth       = 0;
int  TNM_TAP::floatWidth     = 0;

TNM_TAP::TNM_TAP()
{
	network            = NULL; 
	costScalar         = 1.0;
	OFV                = pow(2.0, 52.0); //set the intial value to a fairly big number.
	cpuTime            = 0.0;
	termFlag           = InitTerm;    //this is an intial value, which menas Solve has not been executed yet.
	numLineSearch      = 0;
	maxMainIter        = 100;   // maximum allowed iteration number
	convCriterion      = 0.001; // convergence criterion
	stepSize           = 1.0;     // current step size
	convIndicator      = 1.0;     // convergence indicator
	objectID           = GEN_IA_ID;
	//m_storeResult	   = false;
	m_resetNetworkOnSolve = true;
	yPath			   = new TNM_SPATH;
        reportIterHistory  =false;
        reportLinkDetail   =false;
        reportPathDetail   =false;
        timeCostCoefficient = 1.0;
        distCostCoefficient = 0.0;
        roleName = TNM_DEFAULT_ROLE;
        networkTableName = TNM_DefaultNetworkTable(roleName);
        odTableName = TNM_DefaultODTable(roleName);
        observedTableName = TNM_DefaultObservedTable(roleName);
	m_progressCb = NULL;
	m_progressWindowActive = false;
	m_progressWindowIterHint = 0;
	m_progressWindowStart = 0.0;
	m_progressWindowEnd = 0.0;
}

void TNM_TAP::ConfigureProgressWindow(int iterHint, double startProgress, double endProgress)
{
	m_progressWindowActive = true;
	m_progressWindowIterHint = iterHint;
	m_progressWindowStart = startProgress;
	m_progressWindowEnd = endProgress;
}

void TNM_TAP::ClearProgressWindow()
{
	m_progressWindowActive = false;
}

void TNM_TAP::EmitProgressWindowHeartbeat()
{
	if (!m_progressWindowActive || m_progressCb == NULL) return;
	double progress = m_progressWindowStart;
	if (maxMainIter > 0)
	{
		double frac = static_cast<double>(curIter) / static_cast<double>(maxMainIter);
		if (frac < 0.0) frac = 0.0;
		if (frac > 1.0) frac = 1.0;
		progress = m_progressWindowStart + (m_progressWindowEnd - m_progressWindowStart) * frac;
	}
	if (progress < m_progressWindowStart) progress = m_progressWindowStart;
	if (progress > m_progressWindowEnd) progress = m_progressWindowEnd;
	m_progressCb(m_progressWindowIterHint, progress);
}

TNM_TAP::~TNM_TAP()
{
        ClearIterRecord();
        // 析构时主动回收路径与网络对象，避免 DLL 卸载阶段残留内存
        if (yPath != NULL)
        {
                delete yPath;
                yPath = NULL;
        }
        if (network != NULL)
        {
                delete network;
                network = NULL;
        }
}

void TNM_TAP::SetRoleName(const string& role)
{
        string oldDefaultNet = TNM_DefaultNetworkTable(roleName);
        string oldDefaultOd = TNM_DefaultODTable(roleName);
        string oldDefaultObserved = TNM_DefaultObservedTable(roleName);

        string newRole = role;
        if (newRole.empty())
        {
                newRole = TNM_DEFAULT_ROLE;
        }

        roleName = newRole;

        string newDefaultNet = TNM_DefaultNetworkTable(roleName);
        string newDefaultOd = TNM_DefaultODTable(roleName);
        string newDefaultObserved = TNM_DefaultObservedTable(roleName);

        if (networkTableName.empty() || networkTableName == oldDefaultNet)
        {
                networkTableName = newDefaultNet;
        }

        if (odTableName.empty() || odTableName == oldDefaultOd)
        {
                odTableName = newDefaultOd;
        }

        if (observedTableName.empty() || observedTableName == oldDefaultObserved)
        {
                observedTableName = newDefaultObserved;
        }
}

void TNM_TAP::SetNetworkTableName(const string& name)
{
        if (name.empty())
        {
                networkTableName = TNM_DefaultNetworkTable(roleName);
        }
        else
        {
                networkTableName = name;
        }
}

void TNM_TAP::SetODTableName(const string& name)
{
        if (name.empty())
        {
                odTableName = TNM_DefaultODTable(roleName);
        }
        else
        {
                odTableName = name;
        }
}

void TNM_TAP::SetObservedTableName(const string& name)
{
        if (name.empty())
        {
                observedTableName = TNM_DefaultObservedTable(roleName);
        }
        else
        {
                observedTableName = name;
        }
}

void TNM_TAP::SetConv(floatType tf)
{
        if(tf>MAXCONV || tf <MINCONV)
        {
                cout<<"\tAccuracy should range between "<<MINCONV<<" and "<<MAXCONV
			<<"\n\tThe default value "<<convCriterion<<" is retained."<<endl;
	}
	else
	{
		convCriterion = tf;
	}
}

void TNM_TAP::SetMaxLsIter(int ti)
{
	if(ti>MAXLSITER || ti <MINLSITER)
	{
		cout<<"\tMaximum allowed line search iterations should range between "<<MINLSITER<<" and "<<MAXLSITER
			<<"\n\tThe default value "<<maxLineSearchIter<<" is retained."<<endl;
	}
	else
	{
		maxLineSearchIter = ti;
	}
}

void TNM_TAP::SetMaxIter(int ti)
{
	if(ti>MAXMAXITER || ti <MINMAXITER)
	{
		cout<<"\tMaximum allowed iterations should range between "<<MINMAXITER<<" and "<<MAXMAXITER
			<<"\n\tThe default value "<<maxMainIter<<" is retained."<<endl;
	}
	else
	{
		maxMainIter = ti;
	}

}

int TNM_TAP::SetTollType(TNM_TOLLTYPE tl)
{
	m_tlType = tl;
	if(!network->CheckBuildStatus(false))
	{
		cout<<"\t cannot set toll type, please build network first!"<<endl;
		return 1;
	}
	else
	{
		if(tl != TT_MXTOLL) //all dummy links will not be set here.
		{
			network->SetLinkTollType(tl);
		}
	}
		return 0;
}

int TNM_TAP::Build(const string& inFile, const string& outFile, MATINFORMAT in)
{
        TNM_ResetLastError();
        inFileName = inFile;
        outFileName = outFile;
        if(network!=NULL)
        {
                delete network;
                network = NULL;
        }
        // 每次重建都重新分配网络对象，防止访问已释放的指针
        network = new TNM_SNET(inFile);
	network->SetLinkCostScalar(costScalar);
	network->InitializeCostCoef(timeCostCoefficient,distCostCoefficient);
	switch (in)
	{
	case NETTAPAS:
		/*Build the network using Hillel Bar-Gera's file format*/
		if(network->BuildTAPAS(true, lpf)!=0) 
		{
			cout<<"\tEncounter problems when building a network object!"<<endl;
			return 4;
		}
		break;
        case NETPOSTGRESQL:
                /*数据库接口构建网络*/
        {
                string netTable = networkTableName.empty() ? TNM_DefaultNetworkTable(roleName) : networkTableName;
                string odTable = odTableName.empty() ? TNM_DefaultODTable(roleName) : odTableName;
                auto pg_quote_ident = [](const string& name) -> string {
                        if (name.find('"') != string::npos) return name;
                        size_t dot = name.find('.');
                        if (dot != string::npos) return string("\"") + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
                        return string("\"") + name + "\"";
                };
                string qNetTable = pg_quote_ident(netTable);
                string qOdTable = pg_quote_ident(odTable);
                // 运行前校验：OD 起讫点 id 与小区 id 的匹配性（仅打印告警，不中断流程）
                if (!m_dbConnStr.empty())
                {
                        // 从 network 表名推导 area(road_community) 表名
                        string schema;
                        string name;
                        size_t dot = netTable.find('.');
                        if (dot != string::npos) { schema = netTable.substr(0, dot); name = netTable.substr(dot + 1); }
                        else { schema = roleName; name = netTable; }
                        string prefix = name;
                        const string suf = "road_way";
                        size_t pos = prefix.rfind(suf);
                        if (pos != string::npos && pos + suf.size() == prefix.size())
                                prefix = prefix.substr(0, pos);
                        string areaTable = schema + "." + (prefix + "road_community");
                        string qAreaTable = pg_quote_ident(areaTable);

                        PGconn* conn = PQconnectdb(m_dbConnStr.c_str());
                        if (PQstatus(conn) == CONNECTION_OK)
                        {
                                PQsetNoticeProcessor(conn, TNM_PGNoticeProcessor, NULL);
                                // 先检查小区面表是否存在
                                bool areaExists = true;
                                {
                                        string areaSchema = schema;
                                        string areaName = prefix + string("road_community");
                                        string chkSql = string("SELECT 1 FROM information_schema.tables WHERE table_schema='") + areaSchema + "' AND table_name='" + areaName + "' LIMIT 1";
                                        PGresult* rchk = PQexec(conn, chkSql.c_str());
                                        if (rchk && PQresultStatus(rchk) == PGRES_TUPLES_OK)
                                        {
                                                if (PQntuples(rchk) == 0)
                                                {
                                                        string warn = string("Warning: 未找到小区面表: ") + areaTable + TNM_AcpToUtf8("，请校验数据");
                                                        cerr << TNM_AcpToUtf8(warn) << endl;
                                                        TNM_AppendMessageNote(warn);
                                                        areaExists = false;
                                                }
                                        }
                                        if (rchk) PQclear(rchk);
                                }
                                if (!areaExists)
                                {
                                        PQfinish(conn);
                                }
                                else
                                {
                                        string sql =
                                        "WITH cz AS ("
                                        "  SELECT DISTINCT centroid_matched_node::int AS zone FROM " + qNetTable + " "
                                        "  WHERE type = 10 AND centroid_matched_node IS NOT NULL"
                                        "), od AS ("
                                        "  SELECT DISTINCT f_id::int AS zone FROM " + qOdTable + " WHERE demand > 0"
                                        "  UNION SELECT DISTINCT t_id::int AS zone FROM " + qOdTable + " WHERE demand > 0"
                                        "), ar AS ("
                                        "  SELECT DISTINCT area_id::int AS zone FROM " + qAreaTable +
                                        ") SELECT"
                                        "  (SELECT COUNT(*) FROM cz) AS centroid_zone_cnt,"
                                        "  (SELECT COUNT(*) FROM od) AS od_zone_cnt,"
                                        "  (SELECT COUNT(*) FROM ar) AS area_zone_cnt,"
                                        "  (SELECT COUNT(*) FROM (SELECT zone FROM od EXCEPT SELECT zone FROM cz) s)  AS od_not_in_centroid_cnt,"
                                        "  (SELECT COUNT(*) FROM (SELECT zone FROM cz EXCEPT SELECT zone FROM od) s)  AS centroid_not_in_od_cnt,"
                                        "  (SELECT COUNT(*) FROM (SELECT zone FROM od EXCEPT SELECT zone FROM ar) s)  AS od_not_in_area_cnt,"
                                        "  (SELECT COUNT(*) FROM (SELECT zone FROM ar EXCEPT SELECT zone FROM cz) s)  AS area_not_in_centroid_cnt;";
                                        PGresult* rs = PQexec(conn, sql.c_str());
                                        if (rs && PQresultStatus(rs) == PGRES_TUPLES_OK && PQntuples(rs) >= 1 && PQnfields(rs) >= 7)
                                        {
                                                long long c0 = atoll(PQgetvalue(rs, 0, 0)); // centroid_zone_cnt
                                                long long c1 = atoll(PQgetvalue(rs, 0, 1)); // od_zone_cnt
                                                long long c2 = atoll(PQgetvalue(rs, 0, 2)); // area_zone_cnt
                                                long long c3 = atoll(PQgetvalue(rs, 0, 3)); // od_not_in_centroid_cnt
                                                long long c4 = atoll(PQgetvalue(rs, 0, 4)); // centroid_not_in_od_cnt
                                                long long c5 = atoll(PQgetvalue(rs, 0, 5)); // od_not_in_area_cnt
                                                long long c6 = atoll(PQgetvalue(rs, 0, 6)); // area_not_in_centroid_cnt

                                                cout << TNM_AcpToUtf8("[预检查] 使用表: net=") << netTable
                                                     << ", od=" << odTable
                                                     << ", area=" << areaTable << endl;
                                                cout << TNM_AcpToUtf8("[预检查] 计数: centroid=") << c0
                                                     << ", od=" << c1
                                                     << ", area=" << c2
                                                     << ", od_not_in_centroid=" << c3
                                                     << ", centroid_not_in_od=" << c4
                                                     << ", od_not_in_area=" << c5
                                                     << ", area_not_in_centroid=" << c6 << endl;

                                                if (c5 > 0)
                                                {
                                                        ostringstream w;
                                                        w << "Warning: 小区id和OD起讫点id不匹配，请校核数据 (od_not_in_area_cnt=" << c5
                                                          << ", od_table=" << odTable
                                                          << ", area_table=" << areaTable << ")";
                                                        string warn = w.str();
                                                        cerr << TNM_AcpToUtf8(warn) << endl;
                                                        TNM_AppendMessageNote(warn);
                                                }
                                        }
                                        else
                                        {
                                                cerr << TNM_AcpToUtf8("[预检查] 被跳过：SQL 执行失败或结果不完整: ") << PQerrorMessage(conn);
                                        }
                                        if (rs) PQclear(rs);
                                        PQfinish(conn);
                                }
                        }
                        else
                        {
                                // 无法连接则跳过预检查
                                cerr << TNM_AcpToUtf8("[预检查] 被跳过：数据库连接失败: ") << PQerrorMessage(conn);
                                if (conn) PQfinish(conn);
                        }
                }
        if (network->BuildPostgreSQL(m_dbConnStr.c_str(), true, lpf, netTable, odTable) != 0)
        {
                cout << "\tEncounter problems when building a network object!" << endl;
                return 4;
        }
        }
        break;
	default:
		cout<<"\tUnrecognized network format. "<<endl;
		return 5;
	}
	network->ClearZeroDemandOD();
	return 0;
};

TERMFLAGS TNM_TAP::Solve()
{
	TNM_ResetOfwUnreachableStats();
	m_startRunTime = clock();//CPU time
	PreProcess();
	numLineSearch = 0;
	if(termFlag != ErrorTerm)
	{
		curIter = 0;//Current iterations
		/*Get an initial solution*/
		Initialize();
		EmitProgressWindowHeartbeat();
		/*Solve the problem iteratively*/
		if(!Terminate())
		{
			RecordCurrentIter();
			do 
			{
				curIter ++;
				MainLoop();
				EmitProgressWindowHeartbeat();
				RecordCurrentIter();
			}while (!Terminate());
		}
                PostProcess();
				TNM_AppendOfwUnreachableNotes(g_ofw_unreachable_warning_examples);
                termFlag = TerminationType();
                if (termFlag == ErrorTerm && TNM_GetLastError().empty())
                {
                        TNM_SetLastError("算法求解失败，终止条件返回 ErrorTerm");
                }
        }
        //cout<<"\n\n\tSolution process terminated"<<endl;
        cpuTime = 1.0*(clock() - m_startRunTime)/CLOCKS_PER_SEC;
        return termFlag;
}

void TNM_TAP::PreProcess()
{
	stepSize                  = 1.0;
	convIndicator             = 1e3;    
	OFV                       = pow(2.0, 52.0); //set the intial value to a fairly big number.
	numLineSearch             = 0;
	ClearIterRecord();
}

void TNM_TAP::ClearIterRecord()
{
	for (vector<ITERELEM*>::iterator pv = iterRecord.begin(); pv != iterRecord.end(); pv++)
	{
		delete *pv;
	}
	if(!iterRecord.empty()) iterRecord.clear();
}

ITERELEM * TNM_TAP::RecordCurrentIter()
{
	ITERELEM *iterElem;
	iterElem = new ITERELEM;
	iterElem->objID = objectID;
	iterElem->iter  = curIter;
	iterElem->ofv   = OFV;
	iterElem->step  = stepSize;
	iterElem->conv  = convIndicator;
	iterElem->time  = 1.0*(clock() - m_startRunTime)/CLOCKS_PER_SEC;
#ifdef IA_DEBUG
	cout<<*iterElem;
#endif
	iterRecord.push_back(iterElem); 
	return iterElem;
}

bool TNM_TAP::Terminate()
{
	return ReachAccuracy() || ReachMaxIter() || ReachError() || ReachUser();
}

TERMFLAGS TNM_TAP::TerminationType()
{
    // 优先按“达到最大迭代”判定为 MaxIteration，避免边界情况下同时满足两条件时被误判为 Converged
    if(ReachMaxIter())  return MaxIterTerm;
    if(ReachAccuracy()) return ConvergeTerm;
    if(ReachUser())     return UserTerm;
    return ErrorTerm;

}

void TNM_TAP::ColumnGeneration(TNM_SORIGIN* pOrg,TNM_SDEST* dest)
{

	yPath->path.clear();
	yPath->flow = 0;
	yPath->cost = 0;
	//
	TNM_SNODE* snode;
	TNM_SLINK* slink;
	snode = dest->dest;

	while(snode != pOrg->origin)
	{
		slink = snode->pathElem->via;
		if(slink == NULL) 
		{
			long long k = TNM_MakeODKey(pOrg->origin->id, dest->dest->id);
			g_ofw_null_via_count[k] += 1;
			if (g_ofw_null_via_first_node.find(k) == g_ofw_null_via_first_node.end()) g_ofw_null_via_first_node[k] = snode->id;
			if (g_ofw_null_via_printed.insert(k).second)
			{
				cout<<"OFW unreachable OD (skip): origin="<<pOrg->origin->id<<", dest="<<dest->dest->id<<", at node="<<snode->id<<endl;
			}
			yPath->path.clear();
			return;
		}
		
		snode = slink->tail;
		//save the new generated path in yPath for each dest
		yPath->path.push_back(slink);
		
	}

}

floatType TNM_TAP::RelativeGap(bool scale)
{
	TNM_SLINK *link;
	TNM_SORIGIN *org;
	TNM_SDEST *dest;
	floatType gap, tt =0.0, tmd = 0.0;

	for (int i = 0;i<network->numOfLink;i++)
	{
		link = network->linkVector[i];
		tt += link->volume * link->cost;
		//gap += link->volume * link->cost;
	}
	gap = tt;
	for(int i = 0;i<network->numOfOrigin;i++)
	{
		org = network->originVector[i];
		network->UpdateSP(org->origin);
		tmd += org->m_tdmd;
		for(int j = 0;j<org->numOfDest;j++)
		{
			dest = org->destVector[j];
			gap -= (dest->dest->pathElem->cost * dest->assDemand);
		}
	}
	
	if(scale) gap /= tt;
	
	return fabs(gap); //enforce postive
}

double TNM_TAP::ComputeBeckmannObj(bool toll)
{
    double ofv = 0.0;
	for (int i = 0;i<network->numOfLink;i++) ofv += network->linkVector[i]->GetIntCost(toll);
	return ofv;

}

void TNM_TAP::ComputeOFV()
{
	OFV  = ComputeBeckmannObj();
}

int TNM_TAP::Report()
{
	intWidth           = TNM_IntFormat::GetWidth();
	floatWidth         = TNM_FloatFormat::GetWidth();
	if(reportIterHistory)
	{
		string iteFileName = outFileName + ".ite"; //out file
		if (!TNM_OpenOutFile(iteFile, iteFileName))
		{
			cout<<"\n\tFail to build an algorithm object: cannot open .ite file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting the iteration history into file "<<(outFileName + ".ite")<<"..."<<endl;
			ReportIter(iteFile);
		}
	}
	if(reportLinkDetail)
	{
		string lfpFileName  = outFileName + ".lfp";
		if (!TNM_OpenOutFile(lfpFile, lfpFileName))
		{
			cout<<"\n\tFail to Initialize an algorithm object: Cannot open .lfp file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting link details into file "<<(outFileName + ".lfp")<<"..."<<endl;
			ReportLink(lfpFile); 
			
		}

	}
	if(reportPathDetail)
	{
		string pthFileName  = outFileName + ".pth";
		if (!TNM_OpenOutFile(pthFile, pthFileName))
		{
			cout<<"\n\tFail to Initialize an algorithm object: Cannot open .pth file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting path details into file "<<(outFileName + ".pth")<<"..."<<endl;
			ReportPath(pthFile); 
			
		}
	}

	if(iteFile.is_open()) iteFile.close();
	if(lfpFile.is_open()) lfpFile.close();
	if(pthFile.is_open()) pthFile.close();
	return 0;
}

void TNM_TAP::ReportPath(ofstream &out)
{
	int pthid =0;
	out<<setw(intWidth)<<"Path_id"
		<<setw(intWidth)<<"origin"
		<<setw(intWidth)<<"dest"
		<<setw(floatWidth)<<"Flow"
		<<setw(floatWidth)<<"Links"<<endl;

	for (int oi=0; oi<network->numOfOrigin; oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di=0; di< pOrg->numOfDest; di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];

			for (int pi=0; pi<sdest->pathSet.size(); pi++)
			{
				TNM_SPATH* path = sdest->pathSet[pi];
				if (path->flow > 1e-4)
				{
					//output the path 
					pthid ++;
					//

					out<<TNM_IntFormat(pthid)<<TNM_IntFormat(pOrg->origin->id)<<TNM_IntFormat(sdest->dest->id)<<TNM_FloatFormat(path->flow);
					for (int li=0; li<path->path.size();li++)
					{
						TNM_SLINK* link = path->path[li];
						out<<TNM_IntFormat(link->id);
					}
					out<<endl;
				}
			}
		}
	}
}

void TNM_TAP::ReportLink(ofstream &linkFile)
{
	linkFile<<setw(intWidth)<<"ID"
			<<setw(intWidth)<<"From"
			<<setw(intWidth)<<"To"
			<<setw(floatWidth)<<"Cap"
			<<setw(floatWidth)<<"Flow"
			<<setw(floatWidth)<<"Cost"
			<<setw(floatWidth)<<"Toll"<<endl;
	for (int i = 0;i<network->numOfLink;i++)
	{
		TNM_SLINK *link = network->linkVector[i];
		//link->fdCost = link->GetToll();
		link->fdCost = 0.;
		linkFile<<TNM_IntFormat(link->id)
				<<TNM_IntFormat(link->tail->id)
				<<TNM_IntFormat(link->head->id)
				<<TNM_FloatFormat(link->capacity)
				<<TNM_FloatFormat(link->volume,18,11)
				<<TNM_FloatFormat(link->cost)
				<<TNM_FloatFormat(link->fdCost)<<endl;
	}
}

void TNM_TAP::ReportIter(ofstream &out)
{
	if(!iterRecord.empty())
	{
		out<<setw(intWidth)<<"Iter"
			<<setw(floatWidth)<<"OFV"
			<<setw(floatWidth)<<"ConvIndc"
			<<setw(floatWidth)<<"Time"
			<<endl;
	 	for (vector<ITERELEM*>::iterator pv = iterRecord.begin();pv != iterRecord.end(); pv++)
		{
			out<<TNM_IntFormat((*pv)->iter)
				<<TNM_FloatFormat((*pv)->ofv)
				<<TNM_FloatFormat((*pv)->conv)
				<<TNM_FloatFormat((*pv)->time)<<endl;
		}
	}

}

void TAP_Greedy::PreProcess()
{
	TNM_TAP::PreProcess();
	if(m_resetNetworkOnSolve) 
	{
		network->Reset();
	}
}

void TAP_Greedy::Initialize()
{

	network->AllocateNodeBuffer(2);
	network->AllocateLinkBuffer(3);

	network->UpdateLinkCost();
	network->InitialSubNet3();//Implementing the all-or-nothing algorithm
	network->UpdateLinkCost();


	network->UpdateLinkCostDer();
	ComputeOFV(); //compute objective function value

	cout<<"The initial objective is : "<<OFV<<endl;
	
	for (int oi=0;oi<network->numOfOrigin;oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di=0;di<pOrg->numOfDest;di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];
            if(pOrg->origin == sdest->dest) continue;
            TNM_SPATH* spath = NULL;
            if (sdest->pathSet.empty())
            {
                // 回溯当前最短路补建一条初始路径
                network->UpdateSP(pOrg->origin);
                TNM_SPATH* addPath = new TNM_SPATH;
                TNM_SNODE* sn2 = sdest->dest;
                TNM_SLINK* sl2 = NULL;
                while (sn2 != pOrg->origin)
                {
                    sl2 = sn2->pathElem->via;
                    if (sl2 == NULL) break;
                    addPath->path.push_back(sl2);
                    sn2 = sl2->tail;
                }
                if (!addPath->path.empty())
                {
                    sdest->pathSet.push_back(addPath);
                    spath = addPath;
                }
                else
                {
                    TNM_SafeDeletePath(addPath);
                    continue;
                }
            }
            else
            {
                spath = sdest->pathSet.front();
            }
            nPath++;
            spath->id = nPath;
				
		}
	}

	cout<<"The num of links are"<<network->numOfLink<<endl;

	cout<<"end of the ini"<<endl;

}

void TAP_Greedy::PostProcess()
{
	network->UpdateLinkCost();
}

TAP_Greedy::TAP_Greedy()
{
	aveFlowChange = 1.0;
	convIndicator = 1.0;
	count = 0.0;
	//yPath = new TNM_SPATH;
}

TAP_Greedy::~TAP_Greedy()
{
	//delete yPath;
}

void TAP_Greedy::MainLoop()
{
	TotalFlowChange = 0.0;
	numOfPathChange =0;
	floatType oldOFV = OFV;
	totalShiftFlow = 0.0;
	maxPathGap = 0;
	count = 0;
	/*Loop over all OD pairs, update the path set and perform a flow shift*/
	TNM_SORIGIN *pOrg;
	for (int i = 0;i<network->numOfOrigin;i++)
	{
		pOrg = network->originVector[i];
		/*Updating the shortest path tree*/
		network->UpdateSP(pOrg->origin);
		for (int j=0;j<pOrg->numOfDest;j++)
		{
			TNM_SDEST* dest = pOrg->destVector[j];
			if(!dest->ifZeroDemand)
			{
				if(pOrg->origin == dest->dest) continue;
				dest->shiftFlow = 1.0;

				/*Updating the path set*/
				ColumnGeneration(pOrg,dest);
				columnG = !yPath->path.empty();
				if (!columnG)
				{
					dest->shiftFlow = 0.0;
					continue;
				}

				/*Perform a flow shift*/
				//UpdatePathFlowLazy(pOrg,dest);
				UpdatePathFlowGreedy(pOrg,dest);
			}
		}
	}
	{
		size_t curSize = g_ofw_null_via_count.size();
		if (curSize != 0 && curSize != g_ofw_null_via_last_reported_size)
		{
			cout<<"OFW unreachable OD pairs so far: "<<curSize<<endl;
			g_ofw_null_via_last_reported_size = curSize;
		}
	}
	
	/*Inner loop*/
	innerShiftFlow =1.0;
	int il;
	int numofPath;
	double preFlowPre =1e-10;
	maxPathGap = 0.0;
	numOfD =0;
	int numofD2 =0;
	int maxInIter = 500;
	for (il =0; il< maxInIter; il++)
	{
		innerShiftFlow = 0.0;
		
		numofPath = 0;
		numofD2 = 0;
		for (int i = 0;i<network->numOfOrigin;i++)
		{
			pOrg = network->originVector[i];
		
			for (int j=0;j<pOrg->numOfDest;j++)
			{
				TNM_SDEST* dest = pOrg->destVector[j];
				if(!dest->ifZeroDemand)
				{
					if(pOrg->origin == dest->dest) continue;
					if (il%(maxInIter/100) == 0)
					{
						dest->shiftFlow =1.0;
					}
					if (dest->shiftFlow> convIndicator/2.0)
					{
						columnG = false;
						/*Perform a flow shift*/
						//UpdatePathFlowLazy(pOrg,dest);
						UpdatePathFlowGreedy(pOrg,dest);
						numofD2++;
					
					}
					numofPath+=dest->pathSet.size();
				}
			}
		}
		
		if (innerShiftFlow < 1e-10)
		{
			break;
		}
	}
	
	/*Remove paths with zero flow*/
	int depathsetsize = dePathSet.size();
	TNM_SPATH* path;
	if (!dePathSet.empty())
	{
		std::set<TNM_SPATH*> seen;
		for (size_t pi = 0; pi < dePathSet.size(); ++pi)
		{
			path = dePathSet[pi];
			if (path && seen.insert(path).second)
			{
				TNM_SafeDeletePath(path);
			}
		}
		dePathSet.clear();
	}
	//
	convIndicator = RelativeGap();//compute Relative Gap
	aveFlowChange = TotalFlowChange/numOfPathChange;

	ComputeOFV(); //compute objective function value;


	//cout<<"The curIter is "<<curIter<<endl;
	//cout<<"The OFV is "<<OFV<<endl;
	//cout<<"The gap is "<<convIndicator<<endl;
	//cout<<"The total shifted Flow in this iteration is "<<totalShiftFlow<<endl;
	//cout<<"il is "<<il<<endl;
	//cout<<"innerFlowShift is "<<innerShiftFlow<<endl;
	//cout<<"The number of dest shifted is "<<numOfD<<endl;
	//cout<<"The number of searched OD pair is "<<numofD2<<endl;
	//cout<<"The max path cost gap is "<<maxPathGap<<endl;
	//cout<<"The number of Path is "<<numofPath<<endl;
	//cout<<"The size of dePathSet is "<<depathsetsize<<endl;
	//cout<<"The current time is "<<1.0*(clock() - m_startRunTime)/CLOCKS_PER_SEC<<endl;
	
}


void TAP_Greedy::UpdatePathFlowLazy(TNM_SORIGIN* pOrg,TNM_SDEST* dest)
{
	//check if the yPath is existed in the path set
	TNM_SPATH* ePath = NULL;
	bool find = true;
	TNM_SPATH* path;
	dest->shiftFlow =0.0;
	if (columnG)
	{
		for (vector<TNM_SPATH*>::iterator pv = dest->pathSet.begin(); pv != dest->pathSet.end();pv++)
		{
			find = true;
			path = *pv;
			if (path->path.size() == yPath->path.size())
			{

				for (int li=0; li< yPath->path.size();li++)
				{
					if (path->path[li] != yPath->path[li])
					{
						find = false;
						break;
					}
				}
			}
			else//path->path.size() != yPath->path.size()
			{
				find = false;
			}

			if (find)//path->path[li] 与 yPath->path[li]相对应了
			{
				ePath = path;
				//cout<<"doing ePath = path"<<endl;
				break;
			}

		}
		//
		if (!find)
		{
			nPath++;//?
			TNM_SPATH* addPath = new TNM_SPATH;
			addPath->path = yPath->path;
			addPath->id = nPath;
			dest->pathSet.push_back(addPath);
			ePath = addPath;
			//cout<<"ePath = addPath"<<endl;
		}
	
	}
	//added to celerate
	double maxCost =0.0;
	double minCost = 100000000.0;
	for (int pi=0;pi<dest->pathSet.size();pi++)
	{
		path = dest->pathSet[pi];
		path->cost = 0.0;
		path->fdCost = 0.0;
		for (int li=0;li<path->path.size();li++)
		{
			TNM_SLINK* slink = path->path[li];
			path->cost+=slink->cost;
			path->fdCost+= slink->fdCost;

		}
		if (path->cost>maxCost)
		{
			maxCost = path->cost;//update max path cost
		}
		if (path->cost < minCost)
		{
			minCost = path->cost;// update min path cost
			ePath = path;
			//cout<<"doing ePath = path"<<endl;
		}
	}
	//cout<<"3"<<endl;
	double ss;
	if (curIter == 1)
	{
		ss = 1e-2;
	}
	else
	{
		ss = convIndicator/100.0;
	}

	if (maxCost - minCost > maxPathGap)
	{
		maxPathGap = maxCost - minCost;// update max path gap
	}
	dest->shiftFlow = maxCost - minCost;// 
	///end of adding
	//cout<<"dest->shiftFlow is "<<dest->shiftFlow<<"ss is "<<ss<<endl;
	//cout<<"---------------1--------------"<<endl;

	if (dest->shiftFlow > ss)
	{
		//make the label of the ePath;
		//cout<<"ePath->flow is "<<ePath->flow<<endl;
		ePath->preFlow = ePath->flow;// old flow of the shortest path 
		//cout<<"ePath->preFlow is "<<ePath->preFlow<<endl;
		count += 1 ;
		//cout<<"count is "<<count<<endl;
		for (int li=0;li<ePath->path.size();li++)
		{
			ePath->path[li]->markStatus = ePath->id;

		}

		//update the path flows
		if (dest->pathSet.size()>1)
		{
			//cout<<"The size of path set is "<<dest->pathSet.size()<<endl;
			TNM_SLINK* slink;
			double tFlow =0.0;
			for (vector<TNM_SPATH*>::iterator pv = dest->pathSet.begin(); pv != dest->pathSet.end();pv++)
			{
				path = *pv;
				if (path !=ePath)
				{
					//if (abs(path->cost-ePath->cost) > convIndicator/10.0)
					{
						path->preFlow = path->flow;
						path->cost = 0.0;
						vector<TNM_SLINK*> dLinkSet;
						vector<TNM_SLINK*> iLinkSet;
						iLinkSet.clear();
						dLinkSet.clear();
						//compute the second order derivative
						double scost = 0.0;

						for (int li=0;li<path->path.size();li++)
						{
							//
							slink = path->path[li];
							path->cost += slink->cost;
							//
							if (slink->markStatus != ePath->id)
							{
								dLinkSet.push_back(slink);
								//sdLinkSet.push_back(slink);
								scost+=slink->fdCost;
							}
							else
							{
								slink->markStatus = path->id;
							}
						}
						//update those paths with significant difference

						for (int li=0;li<ePath->path.size();li++)
						{
							slink = ePath->path[li];
							//
							if (slink->markStatus == ePath->id)
							{
								//sdLinkSet.push_back(slink);
								iLinkSet.push_back(slink);
								scost+=slink->fdCost;
							}
							else
							{
								slink->markStatus = ePath->id;
							}
						}
						//now compute the flow shift（避免分母接近 0 导致数值爆炸）
						if (fabs(scost) < 1e-12)
						{
							// 路径二阶代价近零，跳过本路径的流量调整
							continue;
						}
						double dflow = (path->cost - ePath->cost)/scost;//ref TRR greedy algorithm

						if (dflow > 1e-11)
						{
							totalShiftFlow+=abs(dflow);

							innerShiftFlow+=abs(dflow);

							//path's flow become 0
							if (dflow >= path->flow)
							{
								dflow = path->flow;
								path->flow =0.0;
								tFlow+=path->flow;
							}
							else
							{
								path->flow = path->flow - dflow;//update path flow
								tFlow+=path->flow;

							}
							//update the link flow of the path
							for (int lj =0; lj<path->path.size();lj++)
							{
								slink = path->path[lj];
								floatType __newVol = slink->volume - dflow;
								if (__newVol < 0.0) __newVol = 0.0;
								slink->volume = __newVol;
								if (abs(slink->volume) < 1e-8)
								{
									slink->volume = 0.0;
								}
								if (slink->volume < 0)
								{
									cout<<"Wrong in updating the path link flow"<<endl;
									cout<<"The volume is "<<slink->volume<<endl;
									cout<<"dflow is "<<dflow<<endl;
									cout<<"preFlow is "<<path->preFlow<<endl;
									cout<<"path flow is "<<path->flow<<endl;
									//PrintPath(path);
									// // system("PAUSE");

								}
								slink->cost = slink->GetCost();// update link cost
								slink->fdCost = slink->GetDerCost();// update link cost derivatives

							}

						}
						else
						{
							tFlow+=path->flow;
						}

					}

				}

			}
			//end of the path search
			if (tFlow-dest->assDemand>1e-5)
			{
				cout<<"The total OD flow of paths is larger than assDemand"<<endl;
				// // system("PAUSE");
			}
			else
			{
				ePath->flow = dest->assDemand - tFlow;
				//updating the path flows
				double aFlow = ePath->flow - ePath->preFlow;
				for (int lj=0;lj<ePath->path.size();lj++)
				{
					slink = ePath->path[lj];
					slink->volume = (slink->volume + aFlow) < 0.0 ? 0.0 : (slink->volume + aFlow);
					slink->cost = slink->GetCost();
					slink->fdCost = slink->GetDerCost();
				}
			}
		}
		//cout<<"doing untill this "<<endl;
		//
		for (vector<TNM_SPATH*>::iterator pv = dest->pathSet.begin(); pv != dest->pathSet.end();)
		{
			path = *pv;
			if (path->flow == 0.0)
			{
				pv = dest->pathSet.erase(pv);
				dePathSet.push_back(path);// delete unused path 
			}
			else
			{
				pv++;
			}
		}
		//cout<<"end if iter"<<endl;
		//cout<<"count is "<<count<<endl;
	}
	//cout<<"-------------------2---------------"<<endl;
	
}


void TAP_Greedy::UpdatePathFlowGreedy(TNM_SORIGIN* pOrg,TNM_SDEST* dest)
{
	
	//check if the yPath is existed in the path set
	bool find = true;
	TNM_SLINK* slink;
	TNM_SPATH* path;
	dest->shiftFlow = 0.0;
	if (columnG)
	{
		for (vector<TNM_SPATH*>::iterator pv = dest->pathSet.begin(); pv != dest->pathSet.end();pv++)
		{
			find = true;
			path = *pv;
			if (path->path.size() == yPath->path.size())
			{

				for (int li=0; li< yPath->path.size();li++)
				{
					if (path->path[li] != yPath->path[li])
					{
						find = false;
						break;
					}
				}
			}
			else
			{
				find = false;
			}


			if (find)
			{
				break;
			}

		}
		//
		if (!find)
		{
			nPath++;
			TNM_SPATH* addPath = new TNM_SPATH;
			addPath->path = yPath->path;
			addPath->id = nPath;
			dest->pathSet.push_back(addPath);
			//
		}
	}
	
	
	if (dest->pathSet.size()>1)
	{
		int doIter = 0;
		bool repeat = false;
		do 
		{
			doIter++;
			//update the path cost
			double maxCost =0.0;
			double minCost = 100000000.0;
			for (int pi=0;pi<dest->pathSet.size();pi++)
			{
				path = dest->pathSet[pi];
				path->preFlow = path->flow;
				path->preRatio = path->preFlow / dest->assDemand;
				path->cost = 0.0;
				path->fdCost = 0.0;
				path->curRatio =0.0;
				path->markStatus = 0;
				for (int li=0;li<path->path.size();li++)
				{
					slink = path->path[li];
					path->cost += slink->cost;
					path->fdCost += slink->fdCost;

				}
				path->estCost = path->cost - path->fdCost * dest->assDemand * path->preRatio;



				if (path->cost>maxCost)
				{
					maxCost = path->cost;
				}
				if (path->cost < minCost)
				{
					minCost = path->cost;
				}
			}
			double ss;
			if (curIter == 1)
			{
				ss = 1e-3;
			}
			else
			{
				ss = convIndicator/2.0;
			}

			if (maxCost - minCost > maxPathGap)
			{
				maxPathGap = maxCost - minCost;
			}
			dest->shiftFlow = maxCost - minCost;

			if (maxCost - minCost > ss)
			{
				repeat = true;
				numOfD++;
				//sort the pathset according to the path's cost 
				QuickSortPath(dest->pathSet,0,dest->pathSet.size()-1);
				dest->costDif = abs(dest->pathSet[dest->pathSet.size()-1]->estCost - dest->pathSet[0]->estCost);
				//re-assign the path flows by greedy algorithm
				double w = dest->pathSet[0]->estCost + dest->pathSet[0]->fdCost*dest->assDemand;
				double B = 1.0/(dest->pathSet[0]->fdCost*dest->assDemand);
				double C = dest->pathSet[0]->estCost/(dest->pathSet[0]->fdCost*dest->assDemand);
				vector<TNM_SPATH*> tempPathSet;
				tempPathSet.clear();
				
				dest->pathSet[0]->markStatus = 1;
				tempPathSet.push_back(dest->pathSet[0]);
				dest->pathSet[0]->curRatio = 1.0;
				int wi = 1;
				
				while (  wi < dest->pathSet.size() && dest->pathSet[wi]->estCost < w)
				{
					path = dest->pathSet[wi];
					path->markStatus = 1;
					C = C+ path->estCost/(path->fdCost*dest->assDemand);
					B = B + 1.0/(path->fdCost*dest->assDemand);
					tempPathSet.push_back(path);
					w = (1.0+C)/B;
					double tr =0.0;
					for (int pj=0;pj<tempPathSet.size();pj++)
					{
						TNM_SPATH* kpath = tempPathSet[pj];
						double denom = kpath->fdCost * dest->assDemand;
						if (fabs(denom) < 1e-12) kpath->curRatio = 0.0;
						else kpath->curRatio = (w-kpath->estCost)/denom;

					}

					wi++;
				}

				while(wi < dest->pathSet.size())
				{
					dePathSet.push_back(dest->pathSet[wi]);
					wi++;
				}
				
				//update the flow and link cost
				for (vector<TNM_SPATH*>::iterator pv = dest->pathSet.begin();pv != dest->pathSet.end();pv++)
				{
					path = *pv;
					{
						//this path has flow 
						path->flow = path->curRatio*dest->assDemand;
						double dflow = path->flow - path->preFlow;

						if (abs(dflow)> 1e-10)
						{
							dest->shiftFlow = abs(dflow);
							totalShiftFlow+=abs(dflow);
							innerShiftFlow+=abs(dflow);
							for (int li =0; li<path->path.size();li++)
							{
								slink = path->path[li];
								slink->volume = (slink->volume + dflow) < 0.0 ? 0.0 : (slink->volume + dflow);
								if ( abs(slink->volume) < 1e-8)
								{
									slink->volume =0.0;
								}
								if (slink->volume<0)
								{
									cout<<"status "<<1<<endl;
									cout<<"slink's volume is "<<slink->volume<<endl;
									cout<<"dflow is "<<dflow<<endl;
									cout<<"curRatio is "<<path->curRatio<<endl;
									cout<<"path flow "<<path->flow<<endl;
									cout<<"path preflow is "<<path->preFlow<<endl;
									cout<<"demand is "<<dest->assDemand<<endl;
									// // system("PAUSE");
								}
								slink->cost = slink->GetCost();
								slink->fdCost = slink->GetDerCost();
							}
						}

					}

				}
				//update the pathset
				if (tempPathSet.size()<dest->pathSet.size())
				{
					dest->pathSet = tempPathSet;

				}

			}
			else
			{
				repeat = false;
			}

		} while (repeat && doIter < 1);

	}
}

//sort the path set according to the path's cost fro min to max
void TAP_Greedy::QuickSortPath(vector<TNM_SPATH*> & order,int low, int high)
{
	if( low < high)
	{
		TNM_SPATH * spath = order[low];
		int i = low;
		int j = high;
		
		//path->estCost = path->cost - path->fdCost * dest->assDemand * path->preRatio;

		//cout<<spath->estCost<<" "<<spath->cost<<" "<<spath->fdCost<<" "<<spath->preRatio<<endl;
			
		while(i<j)
		{
			while ( (i < j) && (order[j]->estCost >= spath->estCost))
			{
				j=j-1;
			}
			order[i] = order[j];
			while( ( i<j ) && (order[i]->estCost <= spath->estCost))
			{
				i=i+1;
			}
			order[j] = order[i];
		}
		order[i] = spath;

		QuickSortPath(order,low,i-1);
		QuickSortPath(order,i+1,high);
	}
}


OD_ESTIMATION::OD_ESTIMATION()
{
    /*The parameters need to be adjusted according to specific requirements*/
    gamma1 = 0.5;//The parameter of the upper-level objective function
    gamma2 = 0.5;//The parameter of the upper-level objective function
	L = 300;//The maximum main loop iteration
	conv_Criterion = 1e-4;//OD_ESTIMATION mainloop convergence criterion
	P = 15;//The maximum quadratic problem general iteration
	ls_J = 5;//J in Pseudo code, the maximum line search iteration need to adjust 
	epsilon_1 = 0.1;//The projection allowable accuracy
	epsilon_2 = 100;//Armijo-type line search stop criterion
	Theta = 10.0;//Armijo parameter
	oblink_ratio = 0.3;//The proportion of observed link flow to the total link flow.
    alpha_max = 1000.0;//The upper bound of the main loop step size
    reportDemandDetail = false;
    reportlinkinforDetail = false;
    reportUpperObjective = false;
    useExternalObservedFlow = true;
    localEstimateMode = false;
    resultTablePrefix.clear();
    // rmseThreshold = 0.0; // 默认不启用 RMSE 平滑阈值
    useRoadwayObserved = false; // 默认不从网络表读取观测流（需显式开启或由接口覆盖）
    observedFlowScale = 1.0;
    solveMaxIterFast = 20;
    solveConvFast = 1e-5f;
    solveMaxIterStrict = 500;
    solveConvStrict = 1e-6f;
    finalStrictSolve = true;
}

OD_ESTIMATION::~OD_ESTIMATION()
{

}



void OD_ESTIMATION::SetGamma(floatType gamma)
{
	SetGamma(gamma, 1.0 - gamma);
}

void OD_ESTIMATION::SetGamma(floatType gammaValue1, floatType gammaValue2)
{
	if(gammaValue1 > 1.0 || gammaValue1 < 0.0 || gammaValue2 > 1.0 || gammaValue2 < 0.0)
	{
		cout<<"\tThe weights of the objective function should be between 0 and 1!"
			<<"\n\tThe default values "<<gamma1<<" and "<<gamma2<<" are retained."<<endl;
	}
	else
	{
		gamma1 = gammaValue1;
		gamma2 = gammaValue2;
	}
}

void OD_ESTIMATION::SetOblink_ratio(floatType ratio)
{
	if(ratio > 1.0 || ratio < 0.0)
	{
		cout<<"\tThe proportion of observed links should be between 0 and 1!"
			<<"\n\tThe default value "<<oblink_ratio<<" is retained."<<endl;
	}
	else
	{
		oblink_ratio = ratio;
	}
}

void OD_ESTIMATION::SetMainloop_maxiter(int maxiter)
{
	if(maxiter > MAXMAXITER || maxiter < MINMAXITER)
	{
		cout<<"\tMaximum allowed iterations should range between "<<MINMAXITER<<" and "<<MAXMAXITER
			<<"\n\tThe default value "<<L<<" is retained."<<endl;
	}
	else
	{
		L = maxiter;
	}
}

void OD_ESTIMATION::SetMainloop_conv(floatType Conv)
{
	if(Conv > MAXCONV || Conv < MINCONV)
	{
		cout<<"\tAccuracy should range between "<<MINCONV<<" and "<<MAXCONV
			<<"\n\tThe default value "<<conv_Criterion<<" is retained."<<endl;
	}
	else
	{
		conv_Criterion = Conv;
	}
}


void OD_ESTIMATION::SetSolveFastParams(int maxIter, floatType conv)
{
	if (maxIter >= MINMAXITER && maxIter <= MAXMAXITER) solveMaxIterFast = maxIter;
	if (conv >= MINCONV && conv <= MAXCONV) solveConvFast = conv;
}

void OD_ESTIMATION::SetSolveStrictParams(int maxIter, floatType conv)
{
	if (maxIter >= MINMAXITER && maxIter <= MAXMAXITER) solveMaxIterStrict = maxIter;
	if (conv >= MINCONV && conv <= MAXCONV) solveConvStrict = conv;
}

namespace {
bool OdAssignTraceEnabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (std::getenv("TNA_OD_ASSIGN_TRACE") != nullptr) ? 1 : 0;
	return cached != 0;
}

void OdAssignTrace(const std::string& line)
{
	if (!OdAssignTraceEnabled()) return;
	std::cout << "[OD_ASSIGN_TRACE] " << line << std::endl;
}
} // namespace

TERMFLAGS OD_ESTIMATION::SolveOdAssignment(bool strict)
{
	const int mi = strict ? solveMaxIterStrict : solveMaxIterFast;
	const floatType cv = strict ? solveConvStrict : solveConvFast;
	const int savedMax = maxMainIter;
	const floatType savedConv = convCriterion;
	const int savedInner = m_maxInnerIter;
	maxMainIter = mi;
	convCriterion = cv;
	// fast 档限制 Greedy 内层迭代，与机动车分配耗时接近
	SetMaxInnerIter(strict ? 500 : 80);
	clock_t t0 = clock();
	{
		std::ostringstream oss;
		oss << "SolveOdAssignment START strict=" << (strict ? 1 : 0)
		    << " maxMainIter=" << mi << " conv=" << cv
		    << " maxInner=" << m_maxInnerIter
		    << " numOrigin=" << (network ? network->numOfOrigin : -1)
		    << " numOD=" << (network ? network->numOfOD : -1)
		    << " numLink=" << (network ? network->numOfLink : -1);
		OdAssignTrace(oss.str());
	}
	TERMFLAGS r = Solve();
	{
		std::ostringstream oss;
		oss << "SolveOdAssignment END strict=" << (strict ? 1 : 0)
		    << " termFlag=" << static_cast<int>(r)
		    << " outerIters=" << curIter
		    << " convIndicator=" << convIndicator
		    << " elapsed_sec=" << (1.0 * (clock() - t0) / CLOCKS_PER_SEC);
		OdAssignTrace(oss.str());
		if (OdAssignTraceEnabled() && !iterRecord.empty())
		{
			for (size_t k = 0; k < iterRecord.size(); ++k)
			{
				const ITERELEM* e = iterRecord[k];
				std::ostringstream row;
				row << "  iterRecord[" << k << "] iter=" << e->iter
				    << " conv=" << e->conv << " ofv=" << e->ofv
				    << " time_sec=" << e->time;
				OdAssignTrace(row.str());
			}
		}
	}
	maxMainIter = savedMax;
	convCriterion = savedConv;
	SetMaxInnerIter(savedInner);
	return r;
}

void OD_ESTIMATION::finalize_strict_equilibrium()
{
	if (!finalStrictSolve) return;
	for (int oo = 0; oo < network->numOfOrigin; oo++)
	{
		TNM_SORIGIN* org = network->originVector[oo];
		for (int dd = 0; dd < org->numOfDest; dd++)
		{
			TNM_SDEST* dest = org->destVector[dd];
			dest->assDemand = dest->buffer[0];
			dest->ifZeroDemand = (dest->assDemand == 0);
		}
	}
	emit_estimation_progress(l + 1, 19.0);
	if (SolveOdAssignment(true) == ErrorTerm)
	{
		cout << "Strict equilibrium solve failed in finalize." << endl;
	}
	emit_estimation_progress(l + 1, 19.5);
	for (int ll = 0; ll < network->numOfLink; ll++)
	{
		network->linkVector[ll]->buffer[0] = network->linkVector[ll]->volume;
	}
	up_obj = upper_objective();
	if (!up_obj_vec.empty()) up_obj_vec.back() = up_obj;
}

void OD_ESTIMATION::SetArmijo_maxiter(int maxiter)
{
	if(maxiter > MAXMAXITER || maxiter < MINMAXITER)
	{
		cout<<"\tMaximum allowed Armijo_iterations should range between "<<MINMAXITER<<" and "<<MAXMAXITER
			<<"\n\tThe default value "<<ls_J<<" is retained."<<endl;
	}
	else
	{
		ls_J = maxiter;
	}
}

void OD_ESTIMATION::SetArmijo_stopcriterion(floatType stop)
{
	if(stop < 0)
	{
		cout<<"\tThe Armijo criterion for objective improvement (algorithm termination criterion) should not be less than 0!"
		<<"\n\tThe default value "<<epsilon_2<<" is retained."<<endl;
	}
	else
	{
		epsilon_2 = stop;
	}
}

void OD_ESTIMATION::SetArmijo_coefficient(floatType theta)
{
	if(theta < 1.0)
	{
		cout<<"\tThe step size reduction coefficient in the Armijo method should not be less than 1!"
			<<"\n\tThe default value "<<Theta<<" is retained."<<endl;
	}
	else
	{
		Theta = theta;
	}
}

void OD_ESTIMATION::SetArmijo_alphamax(floatType max)
{
	if(max < 0)
	{
		cout<<"\tThe maximum feasible step size in the Armijo method should not be less than 0!"
			<<"\n\tThe default value "<<alpha_max<<" is retained."<<endl;
	}
	else
	{
		alpha_max = max;
	}
}


int OD_ESTIMATION::Random(int start, int end)
{
	return start + 1.0 * (end - start) * rand() / (RAND_MAX + 1.0);
}


floatType OD_ESTIMATION::calc_RMSE()
{
        floatType RMSE = 0.0;
        if (useExternalObservedFlow)
        {
                int cnt = 0;
                for (int ll = 0; ll < network->numOfLink; ll++)
                {
                        TNM_SLINK* link = network->linkVector[ll];
                        if (link->observed_volume > 0)
                        {
                                RMSE += pow(link->observed_volume - link->volume, 2);
                                cnt++;
                        }
                }
                if (cnt > 0) RMSE = sqrt(RMSE / cnt);
        }
        else
        {
                for (int ll = 0; ll < observed_link.size(); ll++)
                {
                        if (observed_link[ll]->buffer[1] > 0)
                        {
                                RMSE += pow(observed_link[ll]->buffer[1] - observed_link[ll]->volume, 2);

                        }
                }
                if (!observed_link.empty()) RMSE = sqrt(RMSE / observed_link.size());
        }

        return RMSE;
}


floatType OD_ESTIMATION::upper_objective()
{
	floatType obj = 0.0;
	for (int ii = 0; ii < network->numOfOrigin; ii++)
	{
		TNM_SORIGIN* org = network->originVector[ii];
		for (int jj = 0; jj < org->numOfDest; jj++)
		{
			TNM_SDEST* dest = org->destVector[jj];

			obj += (gamma1 * pow((dest->assDemand - dest->buffer[1]), 2));//The left-hand side of the objective function.
		}
	}


        if (useExternalObservedFlow)
        {
                for (int ll = 0; ll < network->numOfLink; ll++)
                {
                        TNM_SLINK* link = network->linkVector[ll];
                        if (link->observed_volume > 0)
                        {
                                obj += (gamma2 * pow((link->volume - link->observed_volume), 2));
                        }
                }
        }
        else
        {
                for (int ll = 0; ll < observed_link.size(); ll++)
                {
                        obj += (gamma2 * pow((observed_link[ll]->volume - observed_link[ll]->buffer[1]), 2));//The right-hand side of the objective function.
                }
        }

	return obj;
}


void OD_ESTIMATION::Initialize()
{
	network->UpdateLinkCost();
	network->InitialSubNet3();//Implementing the all-or-nothing algorithm
	network->UpdateLinkCost();


	network->UpdateLinkCostDer();
	ComputeOFV(); //Compute objective function value

	for (int oi = 0; oi < network->numOfOrigin; oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di = 0; di < pOrg->numOfDest; di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];
			if(pOrg->origin == sdest->dest) continue;
			if (sdest->pathSet.empty()) continue;
			TNM_SPATH* spath = sdest->pathSet.front();
			nPath++;
			spath->id = nPath;
		}
	}
}


void OD_ESTIMATION::odes_preprocess()
{
	/*Initialize buffer*/
	network->AllocateDestBuffer(7);//estimated_demand(buffer[0]), initial_demand(buffer[1])，real demand(buffer[6])
	network->AllocateLinkBuffer(9);//estimated_link_flow(buffer[0]),observed_link_flow(buffer[1]),equilibrim link flow with real OD demand(buffer[8])
	network->AllocatePathBuffer(5);

	if (localEstimateMode && useExternalObservedFlow)
	{
		for (int ii = 0; ii < network->numOfOrigin; ii++)
		{
			TNM_SORIGIN* org = network->originVector[ii];
			for (int jj = 0; jj < org->numOfDest; jj++)
			{
				TNM_SDEST* dest = org->destVector[jj];
				dest->buffer[6] = dest->assDemand;
				dest->buffer[1] = dest->assDemand;
			}
		}
		observed_link.clear();
		for (int ii = 0; ii < network->numOfLink; ii++)
		{
			TNM_SLINK* link = network->linkVector[ii];
			if (link->observed_volume > 0 && link->db_ltype != 10)
			{
				link->buffer[1] = link->observed_volume;
				observed_link.push_back(link);
			}
		}
		cout << "\tLocal external observed mode: selected " << (int)observed_link.size()
		     << "/" << network->numOfLink << " links from observation table." << endl;
	}
	else
	{
	/*Generate initial demand and real demand*/
	for (int ii = 0; ii < network->numOfOrigin; ii++)
	{
		TNM_SORIGIN* org = network->originVector[ii];
		for (int jj = 0; jj < org->numOfDest; jj++)
		{
			TNM_SDEST* dest = org->destVector[jj];
			dest->buffer[6] = dest->assDemand;//real demand read from file
			float Random1;
			Random1 = Random(-100,100);
			floatType expdemand = dest->assDemand*(1 + float(0.003 * Random1));
			dest->buffer[1] = ((expdemand>0.0)?expdemand:0.0);//initial demand
			dest->assDemand = dest->buffer[6];
		}
	}

	/*Implement Greedy to equilibrium the real demand*/
	emit_estimation_progress(0, 1.2);
	ConfigureProgressWindow(0, 1.21, 1.79);
	OdAssignTrace("odes_preprocess FIRST_ASSIGN (preprocess equilibrium) BEGIN");
	clock_t t_first = clock();
	if(SolveOdAssignment(false)== ErrorTerm)
	{
		string err = TNM_GetLastError();
		if (err.empty())
			TNM_SetLastError("odes_preprocess 首轮交通分配失败（不可达 OD 或均衡求解错误）");
		termFlag = ErrorTerm;
		cout<<"Something wrong happened in solving the problem."<<endl;
		cout << TNM_GetLastError() << endl;
	}
	{
		std::ostringstream oss;
		oss << "odes_preprocess FIRST_ASSIGN END elapsed_sec="
		    << (1.0 * (clock() - t_first) / CLOCKS_PER_SEC)
		    << " termFlag=" << static_cast<int>(termFlag);
		OdAssignTrace(oss.str());
	}
	ClearProgressWindow();
	emit_estimation_progress(0, 1.8);

       /*Select the observed link*/
       if (useExternalObservedFlow)
       {
               observed_link.clear();
               for (int ii = 0; ii < network->numOfLink; ii++)
               {
                       TNM_SLINK* link = network->linkVector[ii];
                       if (link->observed_volume > 0 && link->db_ltype != 10)
                       {
                               link->buffer[1] = link->observed_volume; //外部观测流量
                               observed_link.push_back(link);
                       }
               }
       }
       else
       {
               if (useRoadwayObserved)
               {
                       // 承载力业务且接口强制 useExternalObservedFlow=false 的情形：
                       // 使用 roadway 写入的 observed_volume 作为“内部观测”，拷贝到 buffer[1]
                       observed_link.clear();
                       for (int ii = 0; ii < network->numOfLink; ii++)
                       {
                               TNM_SLINK* link = network->linkVector[ii];
                               if (link->observed_volume > 0 && link->db_ltype != 10)
                               {
                                       link->buffer[1] = link->observed_volume;
                                       observed_link.push_back(link);
                               }
                       }
                       cout << "\tRoadway observed mode (internal): selected " << (int)observed_link.size() << "/" << network->numOfLink << endl;
               }
               else
               {
                       vector<int> v;
                       v.reserve(network->numOfLink);
                       for (int ii=0; ii<network->numOfLink; ii++)
                       {
                               TNM_SLINK* lk = network->linkVector[ii];
                               if (lk && lk->db_ltype != 10) v.push_back(ii);
                       }
                       if (!v.empty()) random_shuffle(v.begin(), v.end());

                       /*Determine the observed link set*/
                       int want = int(oblink_ratio * (int)v.size());
                       if (want < 0) want = 0; if (want > (int)v.size()) want = (int)v.size();
                       for(int ii = 0; ii < want; ii++)
                       {
                               TNM_SLINK* lk = network->linkVector[v[ii]];
                               observed_link.push_back(lk);
                               lk->buffer[8] = lk->volume;//real equilibrium link flow
                               lk->buffer[1] = lk->buffer[8];//observed link flow
                       }
                       cout << "\tInternal observed mode: selected " << (int)observed_link.size() << "/" << v.size() << ", oblink_ratio=" << oblink_ratio << endl;
               }
       }
	}


	/*Change the assDemand to initial demand*/
	for (int ii = 0; ii < network->numOfOrigin; ii++)
	{
		TNM_SORIGIN* org = network->originVector[ii];
		for (int jj = 0; jj < org->numOfDest; jj++)
		{
			TNM_SDEST* dest = org->destVector[jj];
			dest->buffer[0] = dest->buffer[1];//initial_estimated demand equal to the initial demand
			dest->assDemand = dest->buffer[0];
		}
	}

	/*Solve the equilibrium of initial demand*/
	emit_estimation_progress(0, 2.0);
	if(SolveOdAssignment(false) == ErrorTerm)
	{
		cout<<"Something wrong happened in solving the problem."<<endl;
	}
	emit_estimation_progress(0, 4.0);

	for (int ll = 0; ll < network->numOfLink; ll++)
	{
		network->linkVector[ll]->buffer[0] = network->linkVector[ll]->volume;//initial_estimated link flow
	}

	l = 0;//Set the current iteration to 0.
	up_obj = upper_objective();//Calculate the objective function value
	up_obj_vec.push_back(up_obj);
}


void OD_ESTIMATION::search_direction()
{
	V.clear();//Clear the matrix of quadratic programming calculation results

	/*Solve the quadratic programming problem for each OD pair*/
	for (int oo = 0; oo < network->numOfOrigin; oo++)
	{
		TNM_SORIGIN* org = network->originVector[oo];
		for (int dd = 0; dd < org->numOfDest; dd++)
		{
			TNM_SDEST* dest = org->destVector[dd];
			if (dest->buffer[0] == 0)
			{
				continue;
			}
			for (int kk = 0; kk < dest->pathSet.size(); kk++)
			{
				TNM_SPATH* pa = dest->pathSet[kk];

				for (int ka = 0; ka < pa->path.size(); ka++)
				{
					TNM_SLINK* lk = pa->path[ka];
					lk->buffer[2] = 0; //Initialize the link flow proportion (buffer[2]) to 0.
					lk->volume = lk->buffer[0];
					lk->buffer[3] = lk->GetDerCost();//the first derivative of the link cost function with respect to the link flow (buffer[3])
				}
			}
	
			for (int kk = 0; kk < dest->pathSet.size(); kk++)
			{
				TNM_SPATH* pa = dest->pathSet[kk];
				pa->buffer[0] = pa->flow/dest->assDemand;//Initialize the path flow proportion (buffer0)
				for (int ka = 0; ka < pa->path.size(); ka++)
				{
					pa->path[ka]->buffer[2] += pa->buffer[0];//Update the link flow proportion
				}
			}

			/*Check the flow conservation*/
			double topor = 0;
			for (int pi = 0; pi < dest->pathSet.size(); pi++)
			{

				topor += dest->pathSet[pi]->buffer[0];
			}
			if (abs(topor - 1.0) > 0.0001 && dest->pathSet.size() > 0)
			{
				cout<<"the current iteration is "<<l<<endl;
				cout<<"Before computing the quadratic "<<endl;
				cout<<"OD "<<dest->origin->id_()<<"->"<<dest->id_()<<" path total porportion is "<<topor<<endl;
				cout<<"OD demand is "<<dest->assDemand<<endl;
				for (int pi = 0; pi < dest->pathSet.size(); pi++)
				{
					cout<<"path "<<dest->pathSet[pi]->id<<" 's proportion is "<<dest->pathSet[pi]->buffer[0]<<endl;
				}
				// // system("PAUSE");
			}


			p = 0; //Set the current iteration of the quadratic programming problem to 0
			if (dest->pathSet.size() > 1)
			{
				/*Compute the quadratic programming problem*/
				while (p < P)
				{
					bool stop_quadra = quadratic_prob(dest);
					if (stop_quadra)
					{
						break;
					}
				}
			}
			else if (dest->pathSet.size() == 1)
			{
				for (int pi=0; pi<dest->pathSet[0]->path.size(); pi++) 
				{
					TNM_SLINK* lik = dest->pathSet[0]->path[pi];
					lik->buffer[2] = 1.0;
				}
			}
		
			/*Check the flow conservation*/
			topor = 0;
			for (int pi = 0; pi < dest->pathSet.size(); pi++)
			{
				
				topor += dest->pathSet[pi]->buffer[0];
			}
			if (abs(topor - 1.0) > 0.0001 && dest->pathSet.size() > 0)
			{
				cout<<"The current iter is "<<l<<endl;
				cout<<"after computing the quadratic "<<endl;
				cout<<"OD "<<dest->origin->id_()<<"->"<<dest->id_()<<" path total porportion is "<<topor<<endl;
				cout<<"OD demand is "<<dest->assDemand<<endl;
				for (int pi = 0; pi < dest->pathSet.size(); pi++)
				{

					cout<<"path "<<dest->pathSet[pi]->id<<" 's proportion is "<<dest->pathSet[pi]->buffer[0]<<endl;
				}
				// // system("PAUSE");
			}
			
			/*Store the quadratic programming calculation results in a matrix container*/
			for (int kk = 0; kk < dest->pathSet.size(); kk++)
			{
				TNM_SPATH* pa = dest->pathSet[kk];

				for (int ka = 0; ka < pa->path.size(); ka++)
				{
					TNM_SLINK* lk = pa->path[ka];

					lk->markStatus = 0;
				}
			}

			for (int kk = 0; kk < dest->pathSet.size(); kk++)
			{
				TNM_SPATH* pa = dest->pathSet[kk];

				for (int ka = 0; ka < pa->path.size(); ka++)
				{
					TNM_SLINK* lk = pa->path[ka];
			
					if(lk->markStatus == 0)
					{
						pair<TNM_SDEST*, TNM_SLINK*> od_link;
						od_link = make_pair(dest,lk);
						V.insert(pair<pair<TNM_SDEST*, TNM_SLINK*>, floatType>(od_link, lk->buffer[2]));
						
						lk->markStatus = 1;
					}
				}
			}
		}
	}

	/*Calculate the search direction and the maximum feasible step size to satisfy the non-negativity condition of the OD demands*/
	for (int oo = 0; oo < network->numOfOrigin; oo++)
	{
		TNM_SORIGIN* org = network->originVector[oo];
		for (int dd = 0; dd < org->numOfDest; dd++)
		{
			TNM_SDEST* dest = org->destVector[dd];

			dest->buffer[2] = dest->buffer[0] - dest->buffer[1];//the derivative of the left-hand side of the objective function with respect to the OD demand
			dest->buffer[3] = 0.0;
			for (int ol = 0; ol < observed_link.size(); ol++)
			{
				TNM_SLINK* olink = observed_link[ol];

                floatType obs_flow = useExternalObservedFlow ? olink->observed_volume : olink->buffer[1];
                olink->buffer[6] = olink->buffer[0] - obs_flow;//the derivative of the right-hand side of the objective function with respect to the link flow.

				pair<TNM_SDEST*, TNM_SLINK*> od_link;
				od_link = make_pair(dest,olink);
				floatType v_value;
				if (V.find(od_link) == V.end())
					v_value = 0.0;
				else
					v_value = V[make_pair(dest, olink)];

				dest->buffer[3] += (gamma2 * v_value * olink->buffer[6]);//the derivative of the right-hand side of the objective function with respect to the OD demand
			}

			dest->buffer[4] = (-2.0) * (gamma1 * dest->buffer[2] + dest->buffer[3]);//descent direction
			
			/*Adjust the descent direction with respect to the feasibility (non-negativity) conditions for the demands*/
			//Obtain the search direction
			if ((dest->buffer[0] > epsilon_1)||((dest->buffer[0] <= epsilon_1) && (dest->buffer[4] > 0)))
			{
				dest->buffer[5] = dest->buffer[4];
			} 
			else
			{
				dest->buffer[5] = 0.0;
			}

			/*Adjust the maximum feasible step size based on the non-negativity condition of the OD demands*/
			if (dest->buffer[5] < 0)
			{
				floatType negative_gbarr = (-1)*(dest->buffer[0])/dest->buffer[5];
				if (negative_gbarr < alpha_max)
				{
					alpha_max = negative_gbarr;
				}
			}
		}
	}

    // 调试：观测残差（估计-观测）的平均绝对值
	if (!observed_link.empty())
	{
		double avg_abs = 0.0;
		for (int i = 0; i < (int)observed_link.size(); ++i)
		{
			TNM_SLINK* lk = observed_link[i];
			double obs = useExternalObservedFlow ? (double)lk->observed_volume : (double)lk->buffer[1];
			avg_abs += fabs((double)lk->buffer[0] - obs);
		}
		avg_abs /= (double)observed_link.size();
		static int s_obs_debug_counter = 0;
		++s_obs_debug_counter;
		if (s_obs_debug_counter % 20 == 0)
		{
			cout << "[debug] observed residual avg_abs=" << avg_abs << ", count=" << (int)observed_link.size() << endl;
		}
	}
}

bool OD_ESTIMATION::quadratic_prob(TNM_SDEST* dest)
{
	bool stop = false;
	double maxbeta = 100000000000;

	for (int kk = 0; kk < dest->pathSet.size(); kk++)
	{
		TNM_SPATH* pa = dest->pathSet[kk];
		for (int ka = 0; ka < pa->path.size(); ka++)
		{
			TNM_SLINK* lk = pa->path[ka];
			lk->buffer[4] = lk->buffer[3] * lk->buffer[2];
			lk->buffer[5] = 0;
			lk->markStatus = 0;//a status variable for temporary usage
		}
	}

	floatType sum_sigma = 0.0;
	floatType sum_ww = 0.0;
	for (int kk = 0; kk < dest->pathSet.size(); kk++)
	{
		TNM_SPATH* pa = dest->pathSet[kk];
		pa->buffer[1] = 0.0;
		for (int ka = 0; ka < pa->path.size(); ka++)
		{
			pa->buffer[1] += pa->path[ka]->buffer[4];
		}
		sum_sigma += pa->buffer[1];
	}
	floatType sigma_dest = sum_sigma/dest->pathSet.size();

	/*Solve the descent direction for each path*/
	for (int kk = 0; kk < dest->pathSet.size(); kk++)
	{
		TNM_SPATH* pa = dest->pathSet[kk];
		pa->buffer[2] = pa->buffer[1] - sigma_dest;
		pa->buffer[3] = (-1.0) * pa->buffer[2];

		/*Ensure path flow proportion cannot be negative*/
		if (pa->buffer[3] < 0)
		{
			if (maxbeta > -pa->buffer[0] / pa->buffer[3])
			{
				maxbeta = -pa->buffer[0] / pa->buffer[3];
			}
		}

		sum_ww += abs(pa->buffer[2]);
		for (int ka = 0; ka < pa->path.size(); ka++)
		{
			pa->path[ka]->buffer[5] -= pa->buffer[2];
		}
	}
	
	/*Calculate the optimal step size for the quadratic problem*/
	floatType fenzi = 0.0;
	floatType fenmu = 0.0;

	for (int kk = 0; kk < dest->pathSet.size(); kk++)
	{
		TNM_SPATH* pa = dest->pathSet[kk];

		for (int ka = 0; ka < pa->path.size(); ka++)
		{
			TNM_SLINK* lk = pa->path[ka];
			if (lk->markStatus == 0)
			{
				fenzi -= (lk->buffer[3] * lk->buffer[2] * lk->buffer[5]);
				fenmu += (lk->buffer[3] * pow(lk->buffer[5], 2));
				
				lk->markStatus = 1;
			}
		}
	}

	floatType beta = fenzi/fenmu;
	if (beta > maxbeta)
	{
		beta = maxbeta;	
	}

	/*Check the convergence conditions*/
	if (sum_ww < sigma_dest * 0.01 || maxbeta < 1e-3)
	{
		stop = true;
		return stop;
	}

	//Update the path flow proportion
	for (int kk = 0; kk < dest->pathSet.size(); kk++)
	{
		TNM_SPATH* pa = dest->pathSet[kk];
		pa->buffer[0] += (beta * pa->buffer[3]);

		if (abs(pa->buffer[0]) < 1e-8)
		{
			pa->buffer[0] = 0.0;
		}

		if (pa->buffer[0] < 0)
		{
			cout<<"sum_w is "<<sum_ww<<endl;
			cout<<"p is "<<p<<endl;
			cout<<"sigma k is "<<pa->buffer[1]<<endl;
			cout<<"sigma dest is "<<sigma_dest<<endl;
			cout<<"w_k is "<<pa->buffer[2]<<endl;
			cout<<"beta is "<<beta<<" y is "<<pa->buffer[3]<<endl;
			cout<<"this path has a negative x"<<endl;
			cout<<" x is "<<pa->buffer[0]<<endl;
			cout<<"direction is "<<beta * pa->buffer[3]<<endl;
			cout<<"The original is "<<pa->buffer[0] - beta * pa->buffer[3]<<endl;
			// // system("PAUSE");
		}
	}

	/*Update the link flow proportion*/
	for (int kk = 0; kk < dest->pathSet.size(); kk++)
	{
		TNM_SPATH* pa = dest->pathSet[kk];
	
		for (int ka=0; ka<pa->path.size(); ka++)
		{
			pa->path[ka]->buffer[2] += pa->buffer[0];
		}
	}
	
	p += 1;
	return stop;
}

void OD_ESTIMATION::main_line_search()
{
    ls_j = 0;//Set the current Armijo line search iteration to 0.
    floatType* alpha = new floatType[ls_J + 2];//Initialize the iteration step size container
    alpha[0] = alpha_max;
    alpha[1] = alpha_max;
    floatType* F = new floatType[ls_J + 2];//Initialize the objective function container.
    F[0] = up_obj;

	/*Peform Armijo-type line search*/
	while (ls_j < ls_J)
	{
		ls_j += 1;

		for (int oo = 0; oo < network->numOfOrigin; oo++)
		{
			TNM_SORIGIN* org = network->originVector[oo];
			for (int dd = 0; dd < org->numOfDest; dd++)
			{
				TNM_SDEST* dest = org->destVector[dd];
				dest->assDemand = dest->buffer[0] + alpha[ls_j] * dest->buffer[5];

				/*Mark the OD pairs with zero traffic demand*/
				dest->ifZeroDemand = false;
				if(dest->assDemand == 0)
				{
					dest->ifZeroDemand = true;
				} 
			}
		}
		
		/*Perform traffic assignment*/
		emit_estimation_progress(l + 1, 13.0 + 0.5 * ls_j);
		if(SolveOdAssignment(false)== ErrorTerm)
		{
			cout<<"Something wrong happened in solving the problem."<<endl;
		}
		emit_estimation_progress(l + 1, 13.5 + 0.5 * ls_j);

		F[ls_j] = upper_objective();//Obtain the objective function value

		/*Check the convergence conditions*/
		if (up_obj - F[ls_j] > epsilon_2)
		{
			alpha_l = alpha[ls_j];
			break;
		}
		else
        {
            if (ls_j + 1 < ls_J + 2) alpha[ls_j + 1] = alpha[ls_j] / Theta;//Scale down the current step size by the given ratio
        }
	}

	/*If the maximum number of iterations is reached, find the step size corresponding to its minimum value.*/
	floatType minvalue = 1000000000;//This parameter needs to be adjusted based on the specific numerical values
	if (ls_j == ls_J)
    {
        for (int ii = 1;ii <= ls_J; ii++)
        {
            if (minvalue > F[ii])
            {
                minvalue = F[ii];
                alpha_l = alpha[ii];
            }
        }
    }
    delete[] alpha;
    delete[] F;
}

void OD_ESTIMATION::estimation_update()
{
	for (int oo = 0; oo < network->numOfOrigin; oo++)
	{
		TNM_SORIGIN* org = network->originVector[oo];
		for (int dd = 0; dd < org->numOfDest; dd++)
		{
			TNM_SDEST* dest = org->destVector[dd];
			dest->buffer[0] += (alpha_l * dest->buffer[5]);//Update the OD demand

			dest->assDemand = dest->buffer[0];

			/*Mark the OD pairs with zero traffic demand*/
			dest->ifZeroDemand = false;
			if(dest->assDemand == 0)
			{
				dest->ifZeroDemand = true;
			} 
		}
	}

	/*Perform traffic assignment*/
	emit_estimation_progress(l + 1, 17.0);
	if(SolveOdAssignment(false)== ErrorTerm)
	{
		cout<<"Something wrong happened in solving the problem."<<endl;
	}
	emit_estimation_progress(l + 1, 17.5);

	/*Update the estimated link flow and objective function value*/
	for (int ll =  0; ll < network->numOfLink; ll++)
	{
		network->linkVector[ll]->buffer[0] = network->linkVector[ll]->volume;
	}
	up_obj = upper_objective();
	up_obj_vec.push_back(up_obj);
}


void OD_ESTIMATION::emit_estimation_progress(int iterHint, double progress)
{
	if (m_progressCb)
	{
		m_progressCb(iterHint, progress);
	}
}



void OD_ESTIMATION::overall_process()
{
    clock_t start,end;
    start = clock();
    // 默认：路段流量观测表 other_observation；仅当显式 SetUseRoadwayObserved(true) 时用分配 volume
    if (useRoadwayObserved)
    {
        useExternalObservedFlow = false;
        string roadwayTable = networkTableName.empty() ? TNM_DefaultNetworkTable(roleName) : networkTableName;
        int rc = LoadObservedLinkFlowFromRoadway(network, m_dbConnStr, roadwayTable, observedFlowScale);
        if (rc != 0)
        {
            string err = TNM_GetLastError();
            if (err.empty()) {
                err = string("未找到分配结果或缺少有效 volume 字段: ") + roadwayTable + string("。请先运行交通分配。");
            }
            TNM_SetLastError(err);
            termFlag = ErrorTerm;
            return;
        }
    }
    else
    {
        useExternalObservedFlow = true;
        useRoadwayObserved = false;
        string observedTable = observedTableName.empty()
            ? TNM_DefaultObservedTable(roleName)
            : observedTableName;
        int rc = LoadObservedLinkFlowFromPG(network, m_dbConnStr, observedTable);
        if (rc != 0)
        {
            string err = TNM_GetLastError();
            if (err.empty()) {
                err = string("加载路段流量观测失败: ") + observedTable
                    + string("。请先导入路段流量观测数据（link_id、flow）。");
            }
            TNM_SetLastError(err);
            termFlag = ErrorTerm;
            return;
        }
    }
    	/*Pretreatment and initialize to generate the true demand and initial demand, do the equilibrium procedure*/
	emit_estimation_progress(0, 1.0);
    odes_preprocess();
	if (ReachError())
		return;
	emit_estimation_progress(0, 5.0);

	RMSE = calc_RMSE();
	RMSE_vec.push_back(RMSE);

	end = clock();
	Time_vec.push_back(double(end - start)/CLOCKS_PER_SEC);

	if (up_obj_vec.size() >= 2)
	{
		double prev = up_obj_vec[up_obj_vec.size()-2];
		double curr = up_obj_vec.back();
		double denom = abs(prev);
		if (denom < 1e-16) denom = 1e-16;
		RGP = abs(curr - prev) / denom;
	}
	else
	{
		RGP = 1.0;
	}
	RGP_vec.push_back(RGP);

	/*Main Loop*/
	while (l < L)
	{
		// cout<<"===================================Current_iter is "<<l<<"==================================="<<endl;

		/*Computation of a search direction: find the descent direction and projection*/
		emit_estimation_progress(l + 1, 8.0);
		search_direction();

		/*Armijo-type line search to generate an appropriate step size in main loop*/
		emit_estimation_progress(l + 1, 12.0);
		main_line_search();

		/*Use the search direction and step size to update the OD matrix and do the equilibrium procedure*/
		emit_estimation_progress(l + 1, 16.0);
		estimation_update();
		emit_estimation_progress(l + 1, 18.0);

		RMSE = calc_RMSE();
		RMSE_vec.push_back(RMSE);

		end = clock();
		Time_vec.push_back(double(end - start)/CLOCKS_PER_SEC);

		if (up_obj_vec.size() >= 2)
		{
			double prev = up_obj_vec[up_obj_vec.size()-2];
			double curr = up_obj_vec.back();
			double denom = abs(prev);
			if (denom < 1e-16) denom = 1e-16;
			RGP = abs(curr - prev) / denom;
		}
		else
		{
			RGP = 1.0;
		}
		RGP_vec.push_back(RGP);

		int human_iter = l + 1;
		bool print_this = false;
		if (human_iter <= 10)
		{
			print_this = true;
		}
		else
		{
			print_this = (human_iter % 20 == 0);
		}
		if (print_this)
		{
			cout << "Main loop " << human_iter << "/" << L
				 << ", RGP=" << RGP
				 << ", threshold=" << conv_Criterion << endl;
		}

		// cout<<"Up_Obj is: "<<up_obj<<" RMSE is: "<<RMSE<<" RGP is: "<<RGP<<" Iter_time is "<<double(end - start)/CLOCKS_PER_SEC<<endl<<endl;

		// 平滑进度回调：iter_frac 与基于 RGP 的 gap_frac 取最大值并上限 99
		if (m_progressCb)
		{
			double iter_frac = (L > 0) ? (l * 100.0 / L) : 0.0;
			double gap_frac = 0.0;
			if (conv_Criterion > 0.0)
			{
				double denom = (RGP > 0.0) ? RGP : 1e-16; // 除零保护
				double ratio = sqrt(conv_Criterion / denom);
				if (ratio > 1.0) ratio = 1.0;
				if (ratio < 0.0) ratio = 0.0;
				gap_frac = 100.0 * ratio;
			}
			int progress = (int)((iter_frac > gap_frac) ? iter_frac : gap_frac);
			if (progress > 98) progress = 98;
			if (progress < 0) progress = 0;
			int human_iter_cb = l + 1;
			// 调试输出：打印本次回调的迭代序号与进度（注意：算法内回调最大为99，最终100%在外层收尾处回调）
			{
				bool print_cb = false;
				if (human_iter_cb <= 10)
				{
					print_cb = true;
				}
				else
				{
					print_cb = (human_iter_cb % 20 == 0);
				}
				if (print_cb)
				{
					cout << "[progress_cb] iter=" << human_iter_cb
						 << "/" << L
						 << ", iter_frac=" << iter_frac
						 << ", gap_frac=" << gap_frac
						 << ", progress=" << progress
						 << ", RGP=" << RGP
						 << ", threshold=" << conv_Criterion
						 << endl;
				}
			}
			m_progressCb(human_iter_cb, (double)progress);
		}

		/*Check the convergence conditions*/
		if (RGP < conv_Criterion)
		{
			cout<<"Reach the target accuracy!"<<endl;
			break;
		}

		l += 1;
	}

	if(l == L)
	{
		cout<<"Reach the maximum iteration!"<<endl;
	}

	finalize_strict_equilibrium();
}


void OD_ESTIMATION::ReportDemand(ofstream &out)
{
	int odid = 0;
	out<<setw(intWidth)<<"Number"
		<<setw(intWidth)<<"Origin"
		<<setw(intWidth)<<"Dest"
		<<setw(floatWidth)<<"Estimated_demand"
		<<setw(floatWidth)<<"Initial_demand"
		<<endl;
	for (int oi = 0; oi < network->numOfOrigin; oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di = 0; di < pOrg->numOfDest; di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];
			if (sdest->buffer[0] > 1e-4)
			{
				odid ++;
				out<<TNM_IntFormat(odid)<<TNM_IntFormat(pOrg->origin->id)<<TNM_IntFormat(sdest->dest->id)<<TNM_FloatFormat(sdest->assDemand)<<TNM_FloatFormat(sdest->buffer[1]);
				out<<endl;
			}
		}
	}
}

void OD_ESTIMATION::ReportLinkInfor(ofstream &out)
{
	out<<setw(intWidth)<<"ID"
		<<setw(intWidth)<<"From"
		<<setw(intWidth)<<"To"
		<<setw(floatWidth)<<"Estimated_flow"
		<<setw(floatWidth)<<"Observed_flow"
		<<setw(floatWidth)<<"Cost"
		<<setw(floatWidth)<<"Ratio"<<endl;
	for (int i = 0; i < network->numOfLink; i++)
	{
               TNM_SLINK *link = network->linkVector[i];
                floatType obs = useExternalObservedFlow ? link->observed_volume : link->buffer[1];
                floatType ratio = (link->volume != 0.0) ? (obs / link->volume) : 0.0;

                out<<TNM_IntFormat(link->id)
                        <<TNM_IntFormat(link->tail->id)
                        <<TNM_IntFormat(link->head->id)
                        <<TNM_FloatFormat(link->volume)
                        <<TNM_FloatFormat(obs)
                        <<TNM_FloatFormat(link->cost)
                        <<TNM_FloatFormat(ratio)<<endl;

	}
}

void OD_ESTIMATION::ReportUpperObjective(ofstream &out)
{
	if(!up_obj_vec.empty())
	{
		out<<setw(intWidth)<<"Iter"
			<<setw(floatWidth)<<"Up_obj"
			<<setw(floatWidth)<<"RMSE"
			<<setw(floatWidth)<<"RGP"
			<<setw(floatWidth)<<"Time"
			<<endl;


		int iter_count = up_obj_vec.size();
	 	for (int i = 0; i < iter_count; i++)
		{
			out<<TNM_IntFormat(i)
				<<TNM_FloatFormat(up_obj_vec[i])
				<<TNM_FloatFormat(RMSE_vec[i])
				<<TNM_FloatFormat(RGP_vec[i])
				<<TNM_FloatFormat(Time_vec[i])<<endl;
		}
	}
}

int OD_ESTIMATION::Report()
{
	intWidth           = TNM_IntFormat::GetWidth();
	floatWidth         = TNM_FloatFormat::GetWidth();


	if(reportPathDetail)
	{
		string pthFileName  = outFileName + ".pth";
		if (!TNM_OpenOutFile(pthFile, pthFileName))
		{
			cout<<"\n\tFail to Initialize an algorithm object: Cannot open .pth file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting path details into file "<<(outFileName + ".pth")<<"..."<<endl;
			ReportPath(pthFile); 
			
		}
	}

	if(reportDemandDetail)
	{
		string demandFileName  = outFileName + ".de";
		if (!TNM_OpenOutFile(demandFile, demandFileName))
		{
			cout<<"\n\tFail to Initialize an algorithm object: Cannot open .de file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting demand details into file "<<(outFileName + ".de")<<"..."<<endl;
			ReportDemand(demandFile); 
			
		}
	}

	if(reportlinkinforDetail)
	{
		string lkinfFileName  = outFileName + ".lkinf";
		if (!TNM_OpenOutFile(linkinforFile, lkinfFileName))
		{
			cout<<"\n\tFail to Initialize an algorithm object: Cannot open .lkinf file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting link details into file "<<(outFileName + ".lkinf")<<"..."<<endl;
			ReportLinkInfor(linkinforFile); 
			
		}
	}

	if(reportUpperObjective)
	{
		string upobjFileName  = outFileName + ".upobj";
		if (!TNM_OpenOutFile(upobjFile, upobjFileName))
		{
			cout<<"\n\tFail to Initialize an algorithm object: Cannot open .upobj file to write!"<<endl;
		}
		else
		{
			cout<<"\tWriting upper objective details into file "<<(outFileName + ".upobj")<<"..."<<endl;
			ReportUpperObjective(upobjFile); 
			
		}
	}

	if(pthFile.is_open()) pthFile.close();
	if(demandFile.is_open()) demandFile.close();
	if(linkinforFile.is_open()) linkinforFile.close();
	if(upobjFile.is_open()) upobjFile.close();

	return 0;
}

int OD_ESTIMATION::ReportPG()
{
    bool reportHistOD = true;
    // 建立数据库连接
    PGconn* conn = PQconnectdb(m_dbConnStr.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        cerr << TNM_AcpToUtf8("数据库连接失败: ") << PQerrorMessage(conn);
        if (conn) PQfinish(conn);
        return -1;
    }

    PQsetNoticeProcessor(conn, TNM_PGNoticeProcessor, NULL);

    auto extractRoleFromConn = [&](const string& conn) -> string {
        string v; size_t i = 0, n = conn.size();
        while (i < n) {
            while (i < n && (conn[i] == ' ' || conn[i] == ';' || conn[i] == '\t' || conn[i] == '\r' || conn[i] == '\n')) ++i;
            if (i >= n) break;
            size_t j = i;
            while (j < n && conn[j] != '=' && conn[j] != ' ' && conn[j] != ';' && conn[j] != '\t' && conn[j] != '\r' && conn[j] != '\n') ++j;
            string k = conn.substr(i, j - i);
            if (j < n && conn[j] == '=') {
                ++j;
                if (j < n && (conn[j] == '"' || conn[j] == '\'')) {
                    char q = conn[j++]; size_t s = j; while (j < n && conn[j] != q) ++j; v = conn.substr(s, j - s); if (k == "role") return v; if (j < n) ++j;
                } else {
                    size_t s = j; while (j < n && conn[j] != ' ' && conn[j] != ';' && conn[j] != '\t' && conn[j] != '\r' && conn[j] != '\n') ++j; v = conn.substr(s, j - s); if (k == "role") return v;
                }
                i = j;
            } else {
                i = j + 1;
            }
        }
        return string();
    };

    string schema;
    {
        string r = extractRoleFromConn(m_dbConnStr);
        if (!r.empty()) schema = r;
        else if (!networkTableName.empty()) {
            size_t dot = networkTableName.find('.');
            if (dot != string::npos && dot > 0) schema = networkTableName.substr(0, dot);
        }
        if (schema.empty()) schema = roleName;
        if (schema.size() >= 2 && schema.front() == '"' && schema.back() == '"')
        {
            schema = schema.substr(1, schema.size() - 2);
        }
    }

    string netTable = networkTableName.empty() ? TNM_DefaultNetworkTable(schema) : networkTableName;

    auto buildOutputTable = [&](const string& suffix) -> string {
        string p = resultTablePrefix;
        string s = suffix;
        if (p.empty()) return TNM_ComposeTableName(schema, s);
        while (!p.empty() && p.back() == '_') p.pop_back();
        while (!s.empty() && s.front() == '_') s.erase(0, 1);
        return TNM_ComposeTableName(schema, p + "_" + s);
    };

    auto execComment = [&](const string& sql) {
        PGresult* res = PQexec(conn, sql.c_str());
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
            cerr << "\x1b[31m[ERROR] Comment failed: " << PQerrorMessage(conn);
            cerr << " SQL: " << sql << "\x1b[0m" << endl;
        }
        if (res) PQclear(res);
    };

    auto sqlEscapeUtf8Literal = [&](const char* raw) -> string {
        string utf8 = TNM_AcpToUtf8(raw);
        string escaped;
        escaped.reserve(utf8.size() + 8);
        for (char c : utf8) {
            if (c == '\'') escaped += "''";
            else escaped.push_back(c);
        }
        return escaped;
    };

    auto logTableColumns = [&](const string& tableName, const char* const* cols, const char* const* cmts, int count) {
        cout << TNM_AcpToUtf8("表结构（") << tableName << TNM_AcpToUtf8("）:") << endl;
        for (int i = 0; i < count; ++i) {
            cout << "    " << cols[i] << " - " << TNM_AcpToUtf8(cmts[i]) << endl;
        }
    };

    string odResultTable = buildOutputTable("od_estimation_results");
    string linkResultTable = buildOutputTable("link_flow_results");
    string iterResultTable = buildOutputTable("iteration_record");
    string odSourceTable = odTableName.empty() ? TNM_DefaultODTable(roleName) : odTableName;
    string histResultTable = buildOutputTable("other_trip_distribution_hist");

    // 小区面表 / 质心表（质心一律从 road_community 面心 ST_Centroid 生成）
    string communityTable;
    string centroidTable;
    {
        string prefix = resultTablePrefix;
        while (!prefix.empty() && prefix.back() == '_') prefix.pop_back();
        communityTable = schema + "." + (prefix + "_road_community");
        centroidTable = schema + "." + (prefix + "_road_community_centeroid");
    }

    auto pg_quote_ident = [](const string& name) -> string {
        if (name.find('"') != string::npos) return name;
        size_t dot = name.find('.');
        if (dot != string::npos) return string("\"") + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
        return string("\"") + name + "\"";
    };

    string qNetTable = pg_quote_ident(netTable);
    string qOdResultTable = pg_quote_ident(odResultTable);
    string qLinkResultTable = pg_quote_ident(linkResultTable);
    string qIterResultTable = pg_quote_ident(iterResultTable);
    string qOdSourceTable = pg_quote_ident(odSourceTable);
    string qHistResultTable = pg_quote_ident(histResultTable);
    string qCentroidTable = pg_quote_ident(centroidTable);
    string qCommunityTable = pg_quote_ident(communityTable);

    cout << TNM_AcpToUtf8("数据库 schema: ") << schema << endl;
    cout << TNM_AcpToUtf8("结果表: ") << odResultTable << ", " << linkResultTable << ", " << iterResultTable << endl;
    cout << TNM_AcpToUtf8("观测流来源: ") << (useExternalObservedFlow ? TNM_AcpToUtf8("外部(observed_volume)") : TNM_AcpToUtf8("内部(buffer[1])")) << endl;
    cout << TNM_AcpToUtf8("历史OD备份表: ") << histResultTable << TNM_AcpToUtf8(" (源OD表: ") << odSourceTable << ")" << endl;

    // 输入 OD 表字段名（优先 f_id/t_id，其次 origin_id/dest_id）
    string odFromCol = "f_id";
    string odToCol = "t_id";
    {
        string odSchema = schema;
        string odName;
        size_t dot = odSourceTable.find('.');
        if (dot != string::npos && dot > 0)
        {
            odSchema = odSourceTable.substr(0, dot);
            odName = odSourceTable.substr(dot + 1);
        }
        else
        {
            odName = odSourceTable;
        }
        string chkSql = string("SELECT column_name FROM information_schema.columns WHERE table_schema='") + odSchema + "' AND table_name='" + odName + "' AND column_name IN ('f_id','t_id','origin_id','dest_id')";
        PGresult* rchk = PQexec(conn, chkSql.c_str());
        bool has_f = false, has_t = false, has_o = false, has_d = false;
        if (rchk && PQresultStatus(rchk) == PGRES_TUPLES_OK)
        {
            int n = PQntuples(rchk);
            for (int i = 0; i < n; ++i)
            {
                const char* cn = PQgetvalue(rchk, i, 0);
                if (!cn) continue;
                if (strcmp(cn, "f_id") == 0) has_f = true;
                else if (strcmp(cn, "t_id") == 0) has_t = true;
                else if (strcmp(cn, "origin_id") == 0) has_o = true;
                else if (strcmp(cn, "dest_id") == 0) has_d = true;
            }
        }
        if (rchk) PQclear(rchk);

        if (has_f && has_t)
        {
            odFromCol = "f_id";
            odToCol = "t_id";
        }
        else if (has_o && has_d)
        {
            odFromCol = "origin_id";
            odToCol = "dest_id";
        }
        else
        {
            // 兜底：保持默认 f_id/t_id（若 SQL 执行失败/权限不足也不会中断）
            string warn = string("Warning: 无法识别输入OD表起讫点字段名，默认使用 f_id/t_id: ") + odSourceTable;
            cerr << TNM_AcpToUtf8(warn) << endl;
            TNM_AppendMessageNote(warn);
        }
    }

    // 为原始需求表增加需求差值列（现状需求 - 历史需求），并写入注释与差值
    if (reportHistOD)
    {
        string addColSql = "ALTER TABLE " + qOdSourceTable + " ADD COLUMN IF NOT EXISTS demand_diff DOUBLE PRECISION";
        PGresult* ra = PQexec(conn, addColSql.c_str());
        if (ra && PQresultStatus(ra) != PGRES_COMMAND_OK)
        {
            string warn = string("Warning: 输入OD表增加列 demand_diff 失败: ") + PQerrorMessage(conn);
            cerr << TNM_AcpToUtf8(warn) << endl;
            TNM_AppendMessageNote(warn);
        }
        if (ra) PQclear(ra);

        string csql = string("COMMENT ON COLUMN ") + qOdSourceTable + ".demand_diff IS '" + sqlEscapeUtf8Literal("需求差值（现状-历史）") + "'";
        execComment(csql);

        // 按起讫点匹配历史表，填充需求差值；若 demand 列缺失则告警但不中断
        string updSql =
            "UPDATE " + qOdSourceTable + " o SET demand_diff = o.demand - h.demand"
            " FROM " + qHistResultTable + " h"
            " WHERE o." + odFromCol + " = h." + odFromCol + " AND o." + odToCol + " = h." + odToCol;
        PGresult* ru = PQexec(conn, updSql.c_str());
        if (!ru || PQresultStatus(ru) != PGRES_COMMAND_OK)
        {
            string warn = string("Warning: 需求差值回填失败，检查 demand 列及键名: ") + PQerrorMessage(conn);
            cerr << TNM_AcpToUtf8(warn) << endl;
            TNM_AppendMessageNote(warn);
        }
        if (ru) PQclear(ru);
    }

    struct LinkAnalysisRow
    {
        int linkId;
        int fromNode;
        int toNode;
        double estimatedFlow;
        double observedFlow;
        double cost;
        double capacity;
        double vcRatio;
        double observedEstimatedRatio;
    };

    vector<LinkAnalysisRow> linkAnalysis;
    if (network && network->numOfLink > 0)
    {
        linkAnalysis.reserve(network->numOfLink);
        const double EPS0 = 1e-12;
        const double EPSC = 1e-6;
        for (int i = 0; i < network->numOfLink; ++i)
        {
            TNM_SLINK* link = network->linkVector[i];
            LinkAnalysisRow row;
            row.linkId = link->id;
            row.fromNode = link->tail ? link->tail->id : 0;
            row.toNode = link->head ? link->head->id : 0;
            row.estimatedFlow = link->volume;
            row.observedFlow = useExternalObservedFlow ? (link->observed_volume) : (link->buffer ? link->buffer[1] : 0.0);
            row.cost = link->cost;
            row.capacity = link->capacity;
            row.vcRatio = (link->capacity > EPSC) ? link->volume / link->capacity : 0.0;
            if (fabs(row.estimatedFlow) > EPS0)
            {
                row.observedEstimatedRatio = row.observedFlow / row.estimatedFlow;
            }
            else
            {
                row.observedEstimatedRatio = 0.0;
            }
            linkAnalysis.push_back(row);
        }
    }
    // 创建输出表（若不存在）
    {
        string sql_de =
            "CREATE TABLE IF NOT EXISTS " + qOdResultTable + " ("
            "  id BIGSERIAL PRIMARY KEY,"
            "  origin_id INTEGER NOT NULL,"
            "  dest_id INTEGER NOT NULL,"
            "  estimated_demand DOUBLE PRECISION NOT NULL,"
            "  initial_demand DOUBLE PRECISION NOT NULL,"
            "  created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");";
        PGresult* r1 = PQexec(conn, sql_de.c_str());
        if (PQresultStatus(r1) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("创建表 ") << odResultTable << TNM_AcpToUtf8(" 失败: ") << PQerrorMessage(conn);
            PQclear(r1); PQfinish(conn); return -1;
        }
        PQclear(r1);
        // 列注释（非关键流程，失败不终止）
        {
            const char* cols[] = {"id","origin_id","dest_id","estimated_demand","initial_demand","created_at"};
            const char* cmts[] = {"OD对ID","起点交通小区ID","终点交通小区ID","估计的需求值","初始需求值","创建时间"};
            for (int i = 0; i < 6; ++i) {
                string csql = string("COMMENT ON COLUMN ") + qOdResultTable + "." + cols[i] + " IS '" + sqlEscapeUtf8Literal(cmts[i]) + "';";
                execComment(csql);
            }
            logTableColumns(odResultTable, cols, cmts, 6);
        }

        string sql_lk =
            "CREATE TABLE IF NOT EXISTS " + qLinkResultTable + " ("
            "  id BIGSERIAL PRIMARY KEY,"
            "  link_id INTEGER NOT NULL,"
            "  from_node INTEGER NOT NULL,"
            "  to_node INTEGER NOT NULL,"
            "  estimated_flow DOUBLE PRECISION NOT NULL,"
            "  observed_flow DOUBLE PRECISION NOT NULL,"
            "  cost DOUBLE PRECISION NOT NULL,"
            "  ratio DOUBLE PRECISION,"
            "  created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");";
        PGresult* r2 = PQexec(conn, sql_lk.c_str());
        if (PQresultStatus(r2) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("创建表 ") << linkResultTable << TNM_AcpToUtf8(" 失败: ") << PQerrorMessage(conn);
            PQclear(r2); PQfinish(conn); return -1;
        }
        PQclear(r2);
        // 列注释（非关键流程，失败不终止）
        {
            const char* cols[] = {"id","link_id","from_node","to_node","estimated_flow","observed_flow","cost","ratio","created_at"};
            const char* cmts[] = {"自增主键","路段ID","起始节点","终止节点","估计流量","观测流量","路段成本","观测流量与估计流量的比值","写入时间"};
            for (int i = 0; i < 9; ++i) {
                string csql = string("COMMENT ON COLUMN ") + qLinkResultTable + "." + cols[i] + " IS '" + sqlEscapeUtf8Literal(cmts[i]) + "';";
                execComment(csql);
            }
            logTableColumns(linkResultTable, cols, cmts, 9);
        }

        string sql_it =
            "CREATE TABLE IF NOT EXISTS " + qIterResultTable + " ("
            "  id BIGSERIAL PRIMARY KEY,"
            "  iteration INTEGER NOT NULL,"
            "  upper_objective DOUBLE PRECISION NOT NULL,"
            "  rmse DOUBLE PRECISION NOT NULL,"
            "  rgp DOUBLE PRECISION NOT NULL,"
            "  \"time\" DOUBLE PRECISION NOT NULL,"
            "  created_at TIMESTAMPTZ NOT NULL DEFAULT now()"
            ");";
        PGresult* r3 = PQexec(conn, sql_it.c_str());
        if (PQresultStatus(r3) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("创建表 ") << iterResultTable << TNM_AcpToUtf8(" 失败: ") << PQerrorMessage(conn);
            PQclear(r3); PQfinish(conn); return -1;
        }
        PQclear(r3);
        // 列注释（非关键流程，失败不终止）
        {
            const char* cols[] = {"id","iteration","upper_objective","rmse","rgp","\"time\"","created_at"};
            const char* cmts[] = {"自增主键","迭代次数","上层目标函数值","均方根误差","相对间隙","计算时间(秒)","创建时间"};
            for (int i = 0; i < 7; ++i) {
                string csql = string("COMMENT ON COLUMN ") + qIterResultTable + "." + cols[i] + " IS '" + sqlEscapeUtf8Literal(cmts[i]) + "';";
                execComment(csql);
            }
            logTableColumns(iterResultTable, cols, cmts, 7);
        }
    }

    // 全局规则：OD 对外键统一为交通小区 area_id（与 other_od.f_id/t_id、road_community.area_id 一致）
    map<int, int> centroid2zone;
    bool useCentroidZoneKeys = false;
    {
        string mapSql =
            "SELECT DISTINCT centroid_matched_node::int AS zone_id, "
            "GREATEST(init_node, term_node)::int AS centroid_node_id "
            "FROM " + qNetTable + " "
            "WHERE \"type\" = 10 AND centroid_matched_node IS NOT NULL";
        PGresult* rm = PQexec(conn, mapSql.c_str());
        int pgMapRows = 0;
        if (rm && PQresultStatus(rm) == PGRES_TUPLES_OK)
        {
            pgMapRows = PQntuples(rm);
            for (int mi = 0; mi < pgMapRows; ++mi)
            {
                int zone = atoi(PQgetvalue(rm, mi, 0));
                int cnode = atoi(PQgetvalue(rm, mi, 1));
                if (cnode > 0 && zone > 0)
                {
                    centroid2zone[cnode] = zone;
                }
            }
        }
        else
        {
            string warn = string("Warning: 无法从路网表构建质心→小区映射: ") + PQerrorMessage(conn);
            cerr << TNM_AcpToUtf8(warn) << endl;
            TNM_AppendMessageNote(warn);
        }
        if (rm) PQclear(rm);

        if (network != NULL && !network->centroidNodeToZone.empty())
        {
            for (map<int, int>::const_iterator it = network->centroidNodeToZone.begin();
                 it != network->centroidNodeToZone.end(); ++it)
            {
                if (it->first > 0 && it->second > 0)
                {
                    centroid2zone[it->first] = it->second;
                }
            }
        }

        if (!centroid2zone.empty())
        {
            useCentroidZoneKeys = true;
        }
        cout << TNM_AcpToUtf8("[OD键] 质心节点→交通小区映射 PG=") << pgMapRows
             << TNM_AcpToUtf8(" 内存=") << (network ? (int)network->centroidNodeToZone.size() : 0)
             << TNM_AcpToUtf8(" 合并=") << (int)centroid2zone.size()
             << TNM_AcpToUtf8("；统一 area_id 写库=") << (useCentroidZoneKeys ? "是" : "否") << endl;
    }

    auto resolveOdZoneKey = [&](int nodeId, bool& ok) -> int
    {
        if (!useCentroidZoneKeys)
        {
            ok = true;
            return nodeId;
        }
        map<int, int>::iterator it = centroid2zone.find(nodeId);
        if (it != centroid2zone.end())
        {
            ok = true;
            return it->second;
        }
        ok = false;
        return nodeId;
    };

    if (reportDemandDetail) {
        string truncateSql = "TRUNCATE " + qOdResultTable + " RESTART IDENTITY";
        PGresult* rd = PQexec(conn, truncateSql.c_str());
        if (PQresultStatus(rd) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("清空 ") << odResultTable << TNM_AcpToUtf8(" 失败: ") << PQerrorMessage(conn);
            PQclear(rd); PQfinish(conn); return -1;
        }
        PQclear(rd);

        PGresult* rb = PQexec(conn, "BEGIN");
        if (PQresultStatus(rb) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("BEGIN(") << odResultTable << TNM_AcpToUtf8(") 失败: ") << PQerrorMessage(conn);
            PQclear(rb); PQfinish(conn); return -1;
        }
        PQclear(rb);

        string copySql = "COPY " + qOdResultTable + " (origin_id, dest_id, estimated_demand, initial_demand) FROM STDIN WITH (FORMAT csv)";
        PGresult* cp = PQexec(conn, copySql.c_str());
        if (PQresultStatus(cp) != PGRES_COPY_IN) {
            cerr << TNM_AcpToUtf8("COPY 启动失败(") << odResultTable << TNM_AcpToUtf8("): ") << PQerrorMessage(conn);
            PQclear(cp); PQfinish(conn); return -1;
        }
        PQclear(cp);

        auto d2s = [](double v)->string { ostringstream oss; oss.setf(ios::fmtflags(0), ios::floatfield); oss << setprecision(numeric_limits<double>::max_digits10) << v; return oss.str(); };
        const double EPS = 1e-4;
        int inserted = 0;
        int skippedUnmappedResults = 0;
        for (int oi = 0; oi < network->numOfOrigin; ++oi) {
            TNM_SORIGIN* pOrg = network->originVector[oi];
            for (int di = 0; di < pOrg->numOfDest; ++di) {
                TNM_SDEST* sdest = pOrg->destVector[di];
                double est = (double)sdest->assDemand;
                if (est > EPS) {
                    bool okO = false, okD = false;
                    int origKey = resolveOdZoneKey(pOrg->origin->id, okO);
                    int destKey = resolveOdZoneKey(sdest->dest->id, okD);
                    if (!okO || !okD)
                    {
                        ++skippedUnmappedResults;
                        continue;
                    }
                    string line = to_string(origKey) + "," + to_string(destKey) + "," + d2s(est) + "," + d2s(sdest->buffer[1]) + "\n";
                    if (PQputCopyData(conn, line.c_str(), (int)line.size()) != 1) {
                        cerr << TNM_AcpToUtf8("COPY 传输数据失败(") << odResultTable << TNM_AcpToUtf8(")") << endl;
                        PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
                    }
                    ++inserted;
                }
            }
        }
        if (PQputCopyEnd(conn, NULL) != 1) {
            cerr << TNM_AcpToUtf8("COPY 结束失败(") << odResultTable << TNM_AcpToUtf8(")") << endl;
            PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
        }
        PGresult* cres = PQgetResult(conn);
        if (!cres || PQresultStatus(cres) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("COPY 写入失败(") << odResultTable << TNM_AcpToUtf8("): ") << PQerrorMessage(conn);
            if (cres) PQclear(cres); PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
        }
        if (cres) PQclear(cres);

        PGresult* rc = PQexec(conn, "COMMIT");
        if (PQresultStatus(rc) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("COMMIT(") << odResultTable << TNM_AcpToUtf8(") 失败: ") << PQerrorMessage(conn);
            PQclear(rc); PQfinish(conn); return -1;
        }
        PQclear(rc);
        cout << TNM_AcpToUtf8("已写入 ") << odResultTable << TNM_AcpToUtf8(" 共 ") << inserted << TNM_AcpToUtf8(" 行");
        if (skippedUnmappedResults > 0)
        {
            cout << TNM_AcpToUtf8("，跳过未映射质心节点对=") << skippedUnmappedResults;
        }
        cout << endl;

        // 展示层：从 road_community 面心重建质心点表，并回填 OD 期望线（失败仅 warning，不中断）
        {
            string communityName;
            {
                string prefix = resultTablePrefix;
                while (!prefix.empty() && prefix.back() == '_') prefix.pop_back();
                communityName = prefix + string("_road_community");
            }
            bool communityExists = false;
            {
                string chkSql = string("SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='") + schema + "' AND c.relname='" + communityName + "' LIMIT 1";
                PGresult* rchk = PQexec(conn, chkSql.c_str());
                if (rchk && PQresultStatus(rchk) == PGRES_TUPLES_OK && PQntuples(rchk) > 0)
                    communityExists = true;
                if (rchk) PQclear(rchk);
            }

            if (!communityExists)
            {
                string warn = string("Warning: ") + communityTable + " 表不存在，跳过质心点/OD期望线回填";
                cerr << TNM_AcpToUtf8(warn) << endl;
                TNM_AppendMessageNote(warn);
            }
            else
            {
                {
                    string createSql =
                        "CREATE TABLE IF NOT EXISTS " + qCentroidTable + " ("
                        "  area_id bigint,"
                        "  geometry geometry(Point, 4326)"
                        ");";
                    PGresult* rc = PQexec(conn, createSql.c_str());
                    if (rc && PQresultStatus(rc) != PGRES_COMMAND_OK)
                    {
                        string warn = string("Warning: 创建质心表失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (rc) PQclear(rc);
                }
                {
                    string delSql = "DELETE FROM " + qCentroidTable;
                    PGresult* rd = PQexec(conn, delSql.c_str());
                    if (rd) PQclear(rd);
                }
                {
                    string insSql =
                        "INSERT INTO " + qCentroidTable + " (area_id, geometry) "
                        "SELECT area_id::bigint, ST_Centroid(geometry) "
                        "FROM " + qCommunityTable + " "
                        "WHERE geometry IS NOT NULL";
                    PGresult* ri = PQexec(conn, insSql.c_str());
                    if (!ri || PQresultStatus(ri) != PGRES_COMMAND_OK)
                    {
                        string warn = string("Warning: 从 road_community 灌入质心点失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (ri) PQclear(ri);
                }

                {
                    string a1 = "ALTER TABLE " + qOdSourceTable + " ADD COLUMN IF NOT EXISTS od_centroid_line_geom geometry(LineString, 4326)";
                    PGresult* ra1 = PQexec(conn, a1.c_str());
                    if (ra1 && PQresultStatus(ra1) != PGRES_COMMAND_OK)
                    {
                        string warn = string("Warning: 输入OD表增加列 od_centroid_line_geom 失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (ra1) PQclear(ra1);

                    string c1 = string("COMMENT ON COLUMN ") + qOdSourceTable + ".od_centroid_line_geom IS '" + sqlEscapeUtf8Literal("起讫点连接线") + "'";
                    execComment(c1);
                }

                {
                    string updSql =
                        "UPDATE " + qOdSourceTable + " o SET "
                        "  od_centroid_line_geom = ST_MakeLine(ST_Centroid(c1.geometry), ST_Centroid(c2.geometry)) "
                        " FROM " + qCommunityTable + " c1, " + qCommunityTable + " c2"
                        " WHERE o." + odFromCol + " = c1.area_id AND o." + odToCol + " = c2.area_id"
                        "   AND c1.geometry IS NOT NULL AND c2.geometry IS NOT NULL";
                    PGresult* ru = PQexec(conn, updSql.c_str());
                    if (!ru || PQresultStatus(ru) != PGRES_COMMAND_OK)
                    {
                        string warn = string("Warning: 从 road_community 回填OD期望线失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (ru) PQclear(ru);
                }
            }
        }

        // 读取小区质心表并回填输入 OD 表(odSourceTable)的起点/终点质心 geometry（失败仅 warning，不中断）
#if 0
        {
            // 1) 检查质心表是否存在
            bool centroidExists = true;
            // 注意：此处表名检查必须与 centroidTable 的拼接规则一致（scenario_prefix 与 road_community_centeroid 之间有且只有一个下划线）
            string centroidSchema = schema;
            string centroidName;
            {
                string prefix = resultTablePrefix;
                while (!prefix.empty() && prefix.back() == '_') prefix.pop_back();
                centroidName = prefix + string("_road_community_centeroid");
            }
            {
                // 不使用 information_schema.tables：当目标为 foreign table / 分区表 / 物化视图等对象时可能查不到
                // 改用 pg_catalog，可覆盖更多 relkind
                string chkSql = string("SELECT 1 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='") + centroidSchema + "' AND c.relname='" + centroidName + "' LIMIT 1";
                cout << TNM_AcpToUtf8("[CentroidCheck] centroid_table=") << centroidTable << endl;
                cout << TNM_AcpToUtf8("[CentroidCheck] sql=") << chkSql << endl;
                PGresult* rchk = PQexec(conn, chkSql.c_str());
                if (rchk && PQresultStatus(rchk) == PGRES_TUPLES_OK)
                {
                    if (PQntuples(rchk) == 0)
                    {
                        centroidExists = false;
                    }
                }
                else
                {
                    centroidExists = false;
                }
                if (rchk) PQclear(rchk);
            }

            if (!centroidExists)
            {
                string warn = string("Warning: ") + centroidTable + " 表不存在，无法回填OD起讫点小区质心坐标";
                cerr << TNM_AcpToUtf8(warn) << endl;
                TNM_AppendMessageNote(warn);
            }
            else
            {
                // 1.5) 为输入 OD 表增加起点/终点小区质心 geometry 列（失败不终止）
                {
                    string a1 = "ALTER TABLE " + odSourceTable + " ADD COLUMN IF NOT EXISTS origin_centroid_geom geometry";
                    PGresult* ra1 = PQexec(conn, a1.c_str());
                    if (ra1 && PQresultStatus(ra1) != PGRES_COMMAND_OK)
                    {
                        string warn = string("Warning: 输入OD表增加列 origin_centroid_geom(geometry) 失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (ra1) PQclear(ra1);

                    string a2 = "ALTER TABLE " + odSourceTable + " ADD COLUMN IF NOT EXISTS dest_centroid_geom geometry";
                    PGresult* ra2 = PQexec(conn, a2.c_str());
                    if (ra2 && PQresultStatus(ra2) != PGRES_COMMAND_OK)
                    {
                        string warn = string("Warning: 输入OD表增加列 dest_centroid_geom(geometry) 失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (ra2) PQclear(ra2);

                    string c1 = string("COMMENT ON COLUMN ") + odSourceTable + ".origin_centroid_geom IS '" + sqlEscapeUtf8Literal("起点小区质心坐标") + "'";
                    execComment(c1);
                    string c2 = string("COMMENT ON COLUMN ") + odSourceTable + ".dest_centroid_geom IS '" + sqlEscapeUtf8Literal("终点小区质心坐标") + "'";
                    execComment(c2);
                }

                // 2) 统计 OD 起讫点在质心表中缺失的数量
                {
                    string statSql =
                        "SELECT "
                        "  SUM(CASE WHEN c1.area_id IS NULL THEN 1 ELSE 0 END) AS missing_origin_cnt,"
                        "  SUM(CASE WHEN c2.area_id IS NULL THEN 1 ELSE 0 END) AS missing_dest_cnt"
                        " FROM " + odSourceTable + " o"
                        " LEFT JOIN " + centroidTable + " c1 ON o." + odFromCol + " = c1.area_id"
                        " LEFT JOIN " + centroidTable + " c2 ON o." + odToCol + " = c2.area_id";
                    cout << TNM_AcpToUtf8("[CentroidCheck] stat_sql=") << statSql << endl;
                    PGresult* rs = PQexec(conn, statSql.c_str());
                    if (rs && PQresultStatus(rs) == PGRES_TUPLES_OK && PQntuples(rs) >= 1)
                    {
                        long long m1 = atoll(PQgetvalue(rs, 0, 0));
                        long long m2 = atoll(PQgetvalue(rs, 0, 1));
                        if (m1 > 0 || m2 > 0)
                        {
                            ostringstream w;
                            w << "Warning: 质心id和OD需求的起讫点id不匹配 (missing_origin_cnt=" << m1
                              << ", missing_dest_cnt=" << m2
                              << ", centroid_table=" << centroidTable
                              << ", od_table=" << odSourceTable << ")";
                            string warn = w.str();
                            cerr << TNM_AcpToUtf8(warn) << endl;
                            TNM_AppendMessageNote(warn);
                        }
                    }
                    else
                    {
                        string warn = string("Warning: 质心匹配性统计失败，跳过统计: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    if (rs) PQclear(rs);
                }

                // 3) 执行回填（起点/终点分别匹配，缺失的保持为 NULL）
                {
                    // 起点
                    {
                        cout << TNM_AcpToUtf8("[CentroidFill] origin_update_sql=") << updSql1 << endl;
                        PGresult* ru1 = PQexec(conn, updSql1.c_str());
                        if (!ru1 || PQresultStatus(ru1) != PGRES_COMMAND_OK)
                        {
                            string warn = string("Warning: 回填OD起点小区质心坐标失败: ") + PQerrorMessage(conn);
                            cerr << TNM_AcpToUtf8(warn) << endl;
                            TNM_AppendMessageNote(warn);
                        }
                        if (ru1) PQclear(ru1);
                    }

                    // 终点
                    {
                        string updSql2 =
                            "UPDATE " + odSourceTable + " o SET "
                            "  dest_centroid_geom = c.geometry"
                            " FROM " + centroidTable + " c"
                            " WHERE o." + odToCol + " = c.area_id";
                        cout << TNM_AcpToUtf8("[CentroidFill] dest_update_sql=") << updSql2 << endl;
                        PGresult* ru2 = PQexec(conn, updSql2.c_str());
                        if (!ru2 || PQresultStatus(ru2) != PGRES_COMMAND_OK)
                        {
                            string warn = string("Warning: 回填OD终点小区质心坐标失败: ") + PQerrorMessage(conn);
                            cerr << TNM_AcpToUtf8(warn) << endl;
                            TNM_AppendMessageNote(warn);
                        }
                        if (ru2) PQclear(ru2);
                    }
                }
            }
        }
#endif
    }

#if 1
    // 写入链路结果表 link_flow_results（COPY 批量模式）
    {
        string truncateSql = "TRUNCATE " + qLinkResultTable + " RESTART IDENTITY";
        PGresult* rd = PQexec(conn, truncateSql.c_str());
        if (PQresultStatus(rd) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("清空 ") << linkResultTable << TNM_AcpToUtf8(" 失败: ") << PQerrorMessage(conn);
            PQclear(rd); PQfinish(conn); return -1;
        }
        PQclear(rd);

        PGresult* rb = PQexec(conn, "BEGIN");
        if (PQresultStatus(rb) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("BEGIN(") << linkResultTable << TNM_AcpToUtf8(") 失败: ") << PQerrorMessage(conn);
            PQclear(rb); PQfinish(conn); return -1;
        }
        PQclear(rb);

        string copySql = "COPY " + qLinkResultTable + " (link_id, from_node, to_node, estimated_flow, observed_flow, cost, ratio) FROM STDIN WITH (FORMAT csv)";
        PGresult* cp = PQexec(conn, copySql.c_str());
        if (PQresultStatus(cp) != PGRES_COPY_IN) {
            cerr << TNM_AcpToUtf8("COPY 启动失败(") << linkResultTable << TNM_AcpToUtf8("): ") << PQerrorMessage(conn);
            PQclear(cp); PQfinish(conn); return -1;
        }
        PQclear(cp);

        auto d2s_csv = [](double v)->string { ostringstream oss; oss.setf(ios::fmtflags(0), ios::floatfield); oss << setprecision(numeric_limits<double>::max_digits10) << v; return oss.str(); };

        size_t inserted2 = 0;
        for (size_t i = 0; i < linkAnalysis.size(); ++i)
        {
            const LinkAnalysisRow& row = linkAnalysis[i];
            string line = to_string(row.linkId) + "," + to_string(row.fromNode) + "," + to_string(row.toNode) + "," +
                          d2s_csv(row.estimatedFlow) + "," + d2s_csv(row.observedFlow) + "," + d2s_csv(row.cost) + "," + d2s_csv(row.observedEstimatedRatio) + "\n";
            if (PQputCopyData(conn, line.c_str(), (int)line.size()) != 1) {
                cerr << TNM_AcpToUtf8("COPY 传输数据失败(") << linkResultTable << TNM_AcpToUtf8(")") << endl;
                PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
            }
            ++inserted2;
        }
        if (PQputCopyEnd(conn, NULL) != 1) {
            cerr << TNM_AcpToUtf8("COPY 结束失败(") << linkResultTable << TNM_AcpToUtf8(")") << endl;
            PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
        }
        PGresult* cres = PQgetResult(conn);
        if (!cres || PQresultStatus(cres) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("COPY 写入失败(") << linkResultTable << TNM_AcpToUtf8("): ") << PQerrorMessage(conn);
            if (cres) PQclear(cres); PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
        }
        if (cres) PQclear(cres);

        PGresult* rc2 = PQexec(conn, "COMMIT");
        if (PQresultStatus(rc2) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("COMMIT(") << linkResultTable << TNM_AcpToUtf8(") 失败: ") << PQerrorMessage(conn);
            PQclear(rc2); PQfinish(conn); return -1;
        }
        PQclear(rc2);
        cout << TNM_AcpToUtf8("已写入 ") << linkResultTable << TNM_AcpToUtf8(" 共 ") << (unsigned long long)inserted2 << TNM_AcpToUtf8(" 行") << endl;
    }

    // 写入迭代记录表 iteration_record（COPY 批量模式）
    {
        string truncateSql = "TRUNCATE " + qIterResultTable + " RESTART IDENTITY";
        PGresult* rd = PQexec(conn, truncateSql.c_str());
        if (PQresultStatus(rd) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("清空 ") << iterResultTable << TNM_AcpToUtf8(" 失败: ") << PQerrorMessage(conn);
            PQclear(rd); PQfinish(conn); return -1;
        }
        PQclear(rd);

        PGresult* rb = PQexec(conn, "BEGIN");
        if (PQresultStatus(rb) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("BEGIN(") << iterResultTable << TNM_AcpToUtf8(") 失败: ") << PQerrorMessage(conn);
            PQclear(rb); PQfinish(conn); return -1;
        }
        PQclear(rb);

        string copySql = "COPY " + qIterResultTable + " (iteration, upper_objective, rmse, rgp, \"time\") FROM STDIN WITH (FORMAT csv)";
        PGresult* cp = PQexec(conn, copySql.c_str());
        if (PQresultStatus(cp) != PGRES_COPY_IN) {
            cerr << TNM_AcpToUtf8("COPY 启动失败(") << iterResultTable << TNM_AcpToUtf8("): ") << PQerrorMessage(conn);
            PQclear(cp); PQfinish(conn); return -1;
        }
        PQclear(cp);

        auto d2s_csv = [](double v)->string { ostringstream oss; oss.setf(ios::fmtflags(0), ios::floatfield); oss << setprecision(numeric_limits<double>::max_digits10) << v; return oss.str(); };
        auto is_finite_num = [](double v)->bool { return (v == v) && (v != numeric_limits<double>::infinity()) && (v != -numeric_limits<double>::infinity()); };

        int n = (int)up_obj_vec.size();
        int inserted3 = 0;
        for (int i = 0; i < n; ++i)
        {
            double dv_up   = up_obj_vec[i];
            double dv_rmse = (i < (int)RMSE_vec.size()) ? RMSE_vec[i] : 0.0;
            double dv_rgp  = (i < (int)RGP_vec.size()) ? RGP_vec[i] : 0.0;
            double dv_tm   = (i < (int)Time_vec.size()) ? Time_vec[i] : 0.0;

            string s_up   = is_finite_num(dv_up)   ? d2s_csv(dv_up)   : string("0");
            string s_rmse = is_finite_num(dv_rmse) ? d2s_csv(dv_rmse) : string("0");
            string s_rgp  = is_finite_num(dv_rgp)  ? d2s_csv(dv_rgp)  : string("0");
            string s_tm   = is_finite_num(dv_tm)   ? d2s_csv(dv_tm)   : string("0");

            string line = to_string(i) + "," + s_up + "," + s_rmse + "," + s_rgp + "," + s_tm + "\n";
            if (PQputCopyData(conn, line.c_str(), (int)line.size()) != 1) {
                cerr << TNM_AcpToUtf8("COPY 传输数据失败(") << iterResultTable << TNM_AcpToUtf8(")") << endl;
                PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
            }
            ++inserted3;
        }
        if (PQputCopyEnd(conn, NULL) != 1) {
            cerr << TNM_AcpToUtf8("COPY 结束失败(") << iterResultTable << TNM_AcpToUtf8(")") << endl;
            PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
        }
        PGresult* cres = PQgetResult(conn);
        if (!cres || PQresultStatus(cres) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("COPY 写入失败(") << iterResultTable << TNM_AcpToUtf8("): ") << PQerrorMessage(conn);
            if (cres) PQclear(cres); PGresult* rr = PQexec(conn, "ROLLBACK"); if (rr) PQclear(rr); PQfinish(conn); return -1;
        }
        if (cres) PQclear(cres);

        PGresult* rc3 = PQexec(conn, "COMMIT");
        if (PQresultStatus(rc3) != PGRES_COMMAND_OK) {
            cerr << TNM_AcpToUtf8("COMMIT(") << iterResultTable << TNM_AcpToUtf8(") 失败: ") << PQerrorMessage(conn);
            PQclear(rc3); PQfinish(conn); return -1;
        }
        PQclear(rc3);
        cout << TNM_AcpToUtf8("已写入 ") << iterResultTable << TNM_AcpToUtf8(" 共 ") << inserted3 << TNM_AcpToUtf8(" 行") << endl;
    }
#endif
#ifdef _WIN32
        string sep = "\\";
#else
        string sep = "/";
#endif
        string analysisDirPath = "report_local_analysis";

        auto ensureDirectory = [](const string& path) -> bool
        {
#ifdef _WIN32
            if (_access(path.c_str(), 0) == 0) return true;
            if (_mkdir(path.c_str()) == 0) return true;
            if (errno == EEXIST) return true;
            return false;
#else
            struct stat st;
            if (stat(path.c_str(), &st) == 0)
            {
                if (S_ISDIR(st.st_mode)) return true;
                return false;
            }
            if (mkdir(path.c_str(), 0755) == 0) return true;
            if (errno == EEXIST) return true;
            return false;
#endif
        };

        if (!ensureDirectory(analysisDirPath))
        {
            cerr << TNM_AcpToUtf8("创建本地分析目录失败: ") << analysisDirPath << endl;
        }
        else
        {
            string prefix = resultTablePrefix.empty() ? "scenario" : resultTablePrefix;
            auto d2sCsv = [](double v) -> string
            {
                ostringstream oss;
                oss.setf(ios::fmtflags(0), ios::floatfield);
                oss << setprecision(10) << v;
                return oss.str();
            };

            vector<LinkAnalysisRow> flowTop = linkAnalysis;
            sort(flowTop.begin(), flowTop.end(), [](const LinkAnalysisRow& a, const LinkAnalysisRow& b)
            {
                if (a.estimatedFlow == b.estimatedFlow)
                {
                    return a.linkId < b.linkId;
                }
                return a.estimatedFlow > b.estimatedFlow;
            });
            if ((int)flowTop.size() > 20) flowTop.resize(20);

            vector<LinkAnalysisRow> vcTop = linkAnalysis;
            sort(vcTop.begin(), vcTop.end(), [](const LinkAnalysisRow& a, const LinkAnalysisRow& b)
            {
                if (a.vcRatio == b.vcRatio)
                {
                    return a.linkId < b.linkId;
                }
                return a.vcRatio > b.vcRatio;
            });
            if ((int)vcTop.size() > 20) vcTop.resize(20);

            auto joinPath = [&](const string& dir, const string& name) -> string
            {
                if (dir.empty()) return name;
                char last = dir[dir.size()-1];
                if (last == '\\' || last == '/') return dir + name;
                return dir + sep + name;
            };
            string flowFilePath = joinPath(analysisDirPath, prefix + "top20_link_flow.csv");
            string vcFilePath   = joinPath(analysisDirPath, prefix + "top_vc_ratio_links.csv");

            ofstream flowFile(flowFilePath.c_str(), ios::out | ios::trunc);
            if (!flowFile)
            {
                cerr << TNM_AcpToUtf8("写入本地分析表失败: ") << flowFilePath << endl;
            }
            else
            {
                flowFile << "link_id,from_node,to_node,estimated_flow,observed_flow,capacity,vc_ratio,observed_over_estimated,cost" << endl;
                for (int i = 0; i < (int)flowTop.size(); ++i)
                {
                    const LinkAnalysisRow& row = flowTop[i];
                    flowFile << row.linkId << ',' << row.fromNode << ',' << row.toNode << ','
                             << d2sCsv(row.estimatedFlow) << ',' << d2sCsv(row.observedFlow) << ','
                             << d2sCsv(row.capacity) << ',' << d2sCsv(row.vcRatio) << ','
                             << d2sCsv(row.observedEstimatedRatio) << ',' << d2sCsv(row.cost) << endl;
                }
                flowFile.close();
                cout << TNM_AcpToUtf8("       已生成本地流量前20链路表: ") << flowFilePath << endl;
            }

            ofstream vcFile(vcFilePath.c_str(), ios::out | ios::trunc);
            if (!vcFile)
            {
                cerr << TNM_AcpToUtf8("写入本地分析表失败: ") << vcFilePath << endl;
            }
            else
            {
                vcFile << "link_id,from_node,to_node,estimated_flow,observed_flow,capacity,vc_ratio,observed_over_estimated,cost" << endl;
                for (int i = 0; i < (int)vcTop.size(); ++i)
                {
                    const LinkAnalysisRow& row = vcTop[i];
                    vcFile << row.linkId << ',' << row.fromNode << ',' << row.toNode << ','
                           << d2sCsv(row.estimatedFlow) << ',' << d2sCsv(row.observedFlow) << ','
                           << d2sCsv(row.capacity) << ',' << d2sCsv(row.vcRatio) << ','
                           << d2sCsv(row.observedEstimatedRatio) << ',' << d2sCsv(row.cost) << endl;
                }
                vcFile.close();
                cout << TNM_AcpToUtf8("       已生成本地最高拥挤度链路表: ") << vcFilePath << endl;
            }
        }

        // 批量写回网络拓扑表的 volume/v_c 及预设列（可开关，便于随时取消）
        const bool enableNetWriteback = true;
        auto writeBackNetworkTable = [&]() {
            // 确保列存在（失败不终止）
            {
                string a1 = "ALTER TABLE " + qNetTable + " ADD COLUMN IF NOT EXISTS volume DOUBLE PRECISION";
                PGresult* ra1 = PQexec(conn, a1.c_str()); if (ra1) PQclear(ra1);
                string a2 = "ALTER TABLE " + qNetTable + " ADD COLUMN IF NOT EXISTS v_c DOUBLE PRECISION";
                PGresult* ra2 = PQexec(conn, a2.c_str()); if (ra2) PQclear(ra2);
                string a3 = "ALTER TABLE " + qNetTable + " ADD COLUMN IF NOT EXISTS volume_edit DOUBLE PRECISION";
                PGresult* ra3 = PQexec(conn, a3.c_str()); if (ra3) PQclear(ra3);
                string a4 = "ALTER TABLE " + qNetTable + " ADD COLUMN IF NOT EXISTS saturation_edit DOUBLE PRECISION";
                PGresult* ra4 = PQexec(conn, a4.c_str()); if (ra4) PQclear(ra4);
                string a5 = "ALTER TABLE " + qNetTable + " ADD COLUMN IF NOT EXISTS saturation_diff DOUBLE PRECISION";
                PGresult* ra5 = PQexec(conn, a5.c_str()); if (ra5) PQclear(ra5);
                string a6 = "ALTER TABLE " + qNetTable + " ADD COLUMN IF NOT EXISTS link_flow_diff DOUBLE PRECISION";
                PGresult* ra6 = PQexec(conn, a6.c_str()); if (ra6) PQclear(ra6);
                string c1 = string("COMMENT ON COLUMN ") + qNetTable + ".volume IS '" + sqlEscapeUtf8Literal("分配所得路段流量") + "'";
                execComment(c1);
                string c2 = string("COMMENT ON COLUMN ") + qNetTable + ".v_c IS '" + sqlEscapeUtf8Literal("饱和度") + "'";
                execComment(c2);
                string c3 = string("COMMENT ON COLUMN ") + qNetTable + ".volume_edit IS '" + sqlEscapeUtf8Literal("求解流量（同volume）") + "'";
                execComment(c3);
                string c4 = string("COMMENT ON COLUMN ") + qNetTable + ".saturation_edit IS '" + sqlEscapeUtf8Literal("求解饱和度（同v_c）") + "'";
                execComment(c4);
                string c5 = string("COMMENT ON COLUMN ") + qNetTable + ".saturation_diff IS '" + sqlEscapeUtf8Literal("饱和度变化量（本次-运行前）") + "'";
                execComment(c5);
                string c6 = string("COMMENT ON COLUMN ") + qNetTable + ".link_flow_diff IS '" + sqlEscapeUtf8Literal("流量变化量（本次-运行前）") + "'";
                execComment(c6);
            }

            PGresult* rb = PQexec(conn, "BEGIN");
            if (PQresultStatus(rb) != PGRES_COMMAND_OK) { if (rb) PQclear(rb); }
            else { PQclear(rb); }

            string ctt = "CREATE TEMP TABLE tna_tmp_linkflow(link_id INTEGER, volume DOUBLE PRECISION) ON COMMIT DROP";
            PGresult* rt = PQexec(conn, ctt.c_str());
            if (PQresultStatus(rt) != PGRES_COMMAND_OK) { if (rt) PQclear(rt); }
            else { PQclear(rt); }

            string cpcmd = "COPY tna_tmp_linkflow (link_id, volume) FROM STDIN WITH (FORMAT csv)";
            PGresult* cp = PQexec(conn, cpcmd.c_str());
            if (PQresultStatus(cp) == PGRES_COPY_IN) {
                PQclear(cp);
                auto d2s = [](double v)->string { ostringstream oss; oss.setf(ios::fmtflags(0), ios::floatfield); oss << setprecision(numeric_limits<double>::max_digits10) << v; return oss.str(); };
                for (size_t i = 0; i < linkAnalysis.size(); ++i) {
                    const LinkAnalysisRow& row = linkAnalysis[i];
                    string line = to_string(row.linkId) + "," + d2s(row.estimatedFlow) + "\n";
                    PQputCopyData(conn, line.c_str(), (int)line.size());
                }
                PQputCopyEnd(conn, NULL);
                PGresult* cr = PQgetResult(conn); if (cr) PQclear(cr);

                string upsql =
                    string("UPDATE ") + qNetTable + " AS nt "
                    "SET "
                    "  volume = t.volume, "
                    "  v_c = CASE WHEN nt.capacity > 0 THEN t.volume/nt.capacity ELSE NULL END, "
                    // 真实值写回：不做分段随机调整，全部使用本次求解的流量/饱和度
                    "  volume_edit = t.volume, "
                    "  saturation_edit = CASE WHEN nt.capacity > 0 THEN t.volume/nt.capacity ELSE NULL END, "
                    "  saturation_diff = CASE WHEN nt.capacity > 0 THEN (t.volume/nt.capacity) - COALESCE(nt.v_c, 0) ELSE NULL END, "
                    "  link_flow_diff = t.volume - COALESCE(nt.volume, 0) "
                    "FROM (SELECT link_id, volume FROM tna_tmp_linkflow) t "
                    "WHERE nt.link_id = t.link_id";
                PGresult* ru = PQexec(conn, upsql.c_str()); if (ru) PQclear(ru);

                const char* netCols[] = {"volume", "v_c", "volume_edit", "saturation_edit", "saturation_diff", "link_flow_diff"};
                const char* netCmts[] = {
                    "分配所得流量",
                    "饱和度",
                    "求解流量（同volume）",
                    "求解饱和度（同v_c）",
                    "饱和度变化量（本次-运行前）",
                    "流量变化量（本次-运行前）"
                };
                logTableColumns(netTable, netCols, netCmts, 6);
            } else { if (cp) PQclear(cp); }

            PGresult* rc = PQexec(conn, "COMMIT"); if (rc) PQclear(rc);
        };
        if (enableNetWriteback) {
            writeBackNetworkTable();
        }

        // 将反推后的 assDemand 回写输入 OD 表的 demand 列（不影响历史表，保留 demand_diff）
        {
            PGresult* rb = PQexec(conn, "BEGIN");
            if (PQresultStatus(rb) != PGRES_COMMAND_OK) { if (rb) PQclear(rb); }
            else { PQclear(rb); }

            string ctt = "CREATE TEMP TABLE tna_tmp_od_demand(orig INTEGER, dest INTEGER, demand DOUBLE PRECISION) ON COMMIT DROP";
            PGresult* rt = PQexec(conn, ctt.c_str());
            if (PQresultStatus(rt) != PGRES_COMMAND_OK) { if (rt) PQclear(rt); }
            else { PQclear(rt); }

            string cpcmd = "COPY tna_tmp_od_demand (orig, dest, demand) FROM STDIN WITH (FORMAT csv)";
            PGresult* cpod = PQexec(conn, cpcmd.c_str());
            if (PQresultStatus(cpod) == PGRES_COPY_IN) {
                PQclear(cpod);
                auto d2s = [](double v)->string { ostringstream oss; oss.setf(ios::fmtflags(0), ios::floatfield); oss << setprecision(numeric_limits<double>::max_digits10) << v; return oss.str(); };
                const double EPS = 1e-4;
                int copiedRows = 0;
                int skippedUnmapped = 0;
                for (int oi = 0; oi < network->numOfOrigin; ++oi) {
                    TNM_SORIGIN* pOrg = network->originVector[oi];
                    if (!pOrg) continue;
                    for (int di = 0; di < pOrg->numOfDest; ++di) {
                        TNM_SDEST* sdest = pOrg->destVector[di];
                        if (!sdest) continue;
                        double ass = (double)sdest->assDemand;
                        if (ass > EPS) {
                            bool okO = false, okD = false;
                            int origKey = resolveOdZoneKey(pOrg->origin->id, okO);
                            int destKey = resolveOdZoneKey(sdest->dest->id, okD);
                            if (!okO || !okD)
                            {
                                ++skippedUnmapped;
                                continue;
                            }
                            string line = to_string(origKey) + "," + to_string(destKey) + "," + d2s(ass) + "\n";
                            PQputCopyData(conn, line.c_str(), (int)line.size());
                            ++copiedRows;
                        }
                    }
                }
                PQputCopyEnd(conn, NULL);
                PGresult* cr = PQgetResult(conn); if (cr) PQclear(cr);
                cout << TNM_AcpToUtf8("[OD写回] COPY 行数=") << copiedRows;
                if (skippedUnmapped > 0)
                {
                    cout << TNM_AcpToUtf8("，跳过未映射质心节点对=") << skippedUnmapped;
                }
                cout << endl;

                string upsql = string("UPDATE ") + qOdSourceTable + " AS o "
                              + "SET demand = t.demand "
                              + "FROM tna_tmp_od_demand t "
                              + "WHERE o." + odFromCol + " = t.orig AND o." + odToCol + " = t.dest";
                PGresult* ru = PQexec(conn, upsql.c_str());
                if (ru)
                {
                    if (PQresultStatus(ru) == PGRES_COMMAND_OK)
                    {
                        cout << TNM_AcpToUtf8("[OD写回] UPDATE 命中行数=") << PQcmdTuples(ru) << endl;
                    }
                    else
                    {
                        string warn = string("Warning: OD demand 写回失败: ") + PQerrorMessage(conn);
                        cerr << TNM_AcpToUtf8(warn) << endl;
                        TNM_AppendMessageNote(warn);
                    }
                    PQclear(ru);
                }
            } else { if (cpod) PQclear(cpod); }

            PGresult* rc = PQexec(conn, "COMMIT"); if (rc) PQclear(rc);
        }

    PQfinish(conn);
    return 0;
}

