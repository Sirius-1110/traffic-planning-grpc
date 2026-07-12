#include "header/stdafx.h"
#include <math.h>
#include <stack>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>
#include <iomanip>
#ifdef _WIN32
#include "../../../Include/postgresql/libpq-fe.h"
#else
#include <libpq-fe.h>
#endif
#include <fstream>
#include <stdexcept>
#include <sstream>

namespace greedy_od_trace {
static bool enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (std::getenv("TNA_GREEDY_OD_TRACE") != nullptr) ? 1 : 0;
	return cached != 0;
}
static bool pair(int o, int d)
{
	if (!enabled()) return false;
	return (o == 20384 && d == 20393) || (o == 20393 && d == 20384);
}
static void log_pathset(const char* phase, int o, int d, double demand, const std::vector<TNM_SPATH*>& ps)
{
	if (!pair(o, d)) return;
	std::cout << "[OD_TRACE] " << phase << " OD(" << o << "->" << d << ") demand=" << demand
	          << " pathSet.size=" << ps.size() << std::endl;
	for (size_t i = 0; i < ps.size(); ++i)
	{
		TNM_SPATH* p = ps[i];
		std::cout << "[OD_TRACE]   [" << i << "] path_id=" << p->id << " nlinks=" << p->path.size()
		          << " flow=" << p->flow << " cost=" << p->cost << std::endl;
	}
}
} // namespace

static inline void trim_inplace(std::string& s) {
	if (s.empty()) return;
	size_t i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
	size_t j = s.size();
	while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\r' || s[j - 1] == '\n')) j--;
	if (i == 0 && j == s.size()) return;
	s = s.substr(i, j - i);
}

static std::string build_path_result_sql_value(int origin_id, int dest_id, int path_id_out,
                                               double path_flow, double path_cost,
                                               const std::string& path_str)
{
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(6);
	oss << '(' << origin_id << ',' << dest_id << ',' << path_id_out << ','
	    << path_flow << ',' << path_cost << ",'" << path_str << "')";
	return oss.str();
}

#ifdef _WIN32
extern "C" IMAGE_DOS_HEADER __ImageBase;
#endif
int  TNM_TAP::intWidth       = 0;
int  TNM_TAP::floatWidth     = 0;

static std::string __GetDllDirA()
{
#ifdef _WIN32
	HMODULE hMod = reinterpret_cast<HMODULE>(&__ImageBase);
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(hMod, buf, MAX_PATH);
	if (n == 0) return std::string(".");
	std::string full(buf, buf + n);
	size_t p = full.find_last_of("\\/");
	if (p == std::string::npos) return std::string(".");
	return full.substr(0, p);
#else
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) return std::string(".");
	buf[n] = '\0';
	std::string full(buf);
	size_t p = full.find_last_of('/');
	if (p == std::string::npos) return std::string(".");
	return full.substr(0, p);
#endif
}

static void __WriteProgressCacheJSON(int iter, double gap)
{
	std::string dir = __GetDllDirA();
#ifdef _WIN32
	std::string file = dir + "\\greedy_progress.json";
#else
	std::string file = dir + "/greedy_progress.json";
#endif
	std::ofstream out(file.c_str(), std::ios::out | std::ios::trunc);
	if (!out.is_open()) return;
	out << "{";
	out << "\"iter\":" << iter << ",";
	out << "\"gap\":" << gap;
	out << "}";
	out.close();
}

static void __AppendProgressHistoryJSONL(int iter, double gap, double ofv, double t)
{
	std::string dir = __GetDllDirA();
#ifdef _WIN32
	std::string file = dir + "\\greedy_progress_history.jsonl";
#else
	std::string file = dir + "/greedy_progress_history.jsonl";
#endif
	std::ofstream out(file.c_str(), std::ios::out | std::ios::app);
	if (!out.is_open()) return;
	out << "{";
	out << "\"iter\":" << iter << ",";
	out << "\"gap\":" << gap << ",";
	out << "\"ofv\":" << ofv << ",";
	out << "\"time\":" << t;
	out << "}" << "\n";
	out.close();
}

// -------------------------------
// Message notes accumulator (run-scoped warnings/info)
// -------------------------------
static std::string& TNM_MessageNotesStorage(){ static std::string notes; return notes; }
void TNM_ResetMessageNotes(){ TNM_MessageNotesStorage().clear(); }
void TNM_AppendMessageNote(const std::string& note){ if(note.empty()) return; std::string& s = TNM_MessageNotesStorage(); if(!s.empty()) s += "\n"; s += note; }
const std::string& TNM_GetMessageNotes(){ return TNM_MessageNotesStorage(); }

// -------------------------------
// Touched tables accumulator (run-scoped)
// Format: a JSON array string, e.g. ["schema.table1","schema.table2"]
// -------------------------------
static std::string& TNM_TouchedTablesStorage(){ static std::string s = "[]"; return s; }
TNM_EXT_CLASS void TNM_ResetTouchedTables(){ TNM_TouchedTablesStorage() = "[]"; }
TNM_EXT_CLASS void TNM_AppendTouchedTable(const std::string& qname){
	if (qname.empty()) return;
	std::string& arr = TNM_TouchedTablesStorage();
	if (arr.size() < 2) arr = "[]";
	std::string q = qname;
	trim_inplace(q);
	if (q.empty()) return;
	std::string needle = std::string("\"") + q + std::string("\"");
	if (arr.find(needle) != std::string::npos) return;
	if (arr == "[]") {
		arr = std::string("[") + needle + std::string("]");
		return;
	}
	if (!arr.empty() && arr.back() == ']') {
		arr.pop_back();
		arr += ",";
		arr += needle;
		arr += "]";
		return;
	}
	arr = std::string("[") + needle + std::string("]");
}
TNM_EXT_CLASS const std::string& TNM_GetTouchedTables(){ return TNM_TouchedTablesStorage(); }

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
	progressCallback   = NULL;
}

TNM_TAP::~TNM_TAP()
{
	ClearIterRecord();
}

void TNM_TAP::SetConv(floatType tf)
{
	if(tf>MAXCONV || tf <MINCONV)
	{
		std::cout<<"\tAccuracy should range between "<<MINCONV<<" and "<<MAXCONV
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
		std::cout<<"\tMaximum allowed line search iterations should range between "<<MINLSITER<<" and "<<MAXLSITER
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
		std::cout<<"\tMaximum allowed iterations should range between "<<MINMAXITER<<" and "<<MAXMAXITER
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
		std::cout<<"\t cannot set toll type, please build network first!"<<endl;
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
	inFileName = inFile;
	outFileName = outFile;
	if(network!=NULL)
	{
		delete network;
		network = NULL;
	}
	else        
		network = new TNM_SNET(inFile);
	network->SetLinkCostScalar(costScalar);
	network->InitializeCostCoef(timeCostCoefficient,distCostCoefficient);
	switch (in)
	{
	case NETTAPAS:
		/*Build the network using Hillel Bar-Gera's file format*/
		if(network->BuildTAPAS(true, lpf)!=0) 
		{
			std::cout<<"\tEncounter problems when building a network object!"<<endl;
			return 4;
		}
		break;
	default:
		std::cout<<"\tUnrecognized network format. "<<endl;
		return 5;
	}
	network->ClearZeroDemandOD();
	return 0;
};

int TNM_TAP::Build(const string& inFile, const string& outFile, MATINFORMAT in, const string& dbConnStr)
{
	inFileName = inFile;
	outFileName = outFile;
	if(network!=NULL)
	{
		delete network;
		network = NULL;
	}
	else        
		network = new TNM_SNET(inFile);
	network->SetLinkCostScalar(costScalar);
	network->InitializeCostCoef(timeCostCoefficient,distCostCoefficient);

	/*Build the network using PostgreSQL database*/
	if(network->BuildPostgreSQL(true, lpf, dbConnStr, "road_way", "other_od")!=0) 
	{
		std::cout<<"\tEncounter problems when building a network object from PostgreSQL!"<<endl;
		// system("PAUSE");
		return 4;
	}

	network->ClearZeroDemandOD();
	return 0;
};

int TNM_TAP::Build(const string& inFile, const string& outFile, MATINFORMAT in, const string& dbConnStr, const string& networkTableName, const string& odTableName)
{
	inFileName = inFile;
	outFileName = outFile;
	if(network!=NULL)
	{
		delete network;
		network = NULL;
	}
	else        
		network = new TNM_SNET(inFile);
	network->SetLinkCostScalar(costScalar);
	network->InitializeCostCoef(timeCostCoefficient,distCostCoefficient);
    
    try {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
#endif
        auto w2u8 = [](const wchar_t* ws)->std::string{
            if (!ws) return std::string();
#ifdef _WIN32
            int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
            std::string s; if (len > 0) { s.resize((size_t)len - 1); WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, NULL, NULL); }
            return s;
#else
            std::string out;
            for (const wchar_t* p = ws; *p; ++p) {
                wchar_t c = *p;
                if (c < 0x80) { out += (char)c; }
                else if (c < 0x800) { out += (char)(0xC0|(c>>6)); out += (char)(0x80|(c&0x3F)); }
                else { out += (char)(0xE0|(c>>12)); out += (char)(0x80|((c>>6)&0x3F)); out += (char)(0x80|(c&0x3F)); }
            }
            return out;
#endif
        };
        const char* env_net = getenv("PG_TABLE_NET");
        const char* env_od  = getenv("PG_TABLE_OD");
        std::string chosen_net = (env_net && *env_net) ? std::string(env_net) : networkTableName;
        std::string chosen_od  = (env_od  && *env_od)  ? std::string(env_od)  : odTableName;

        auto splitSchemaTable = [&](const std::string& qname)->std::pair<std::string,std::string>{
            size_t dot = qname.find('.');
            if (dot == std::string::npos) return std::make_pair(std::string(), qname);
            return std::make_pair(qname.substr(0, dot), qname.substr(dot+1));
        };
        auto ends_with = [&](const std::string& s, const std::string& suf)->bool{
            return s.size()>=suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf)==0;
        };

        std::pair<std::string,std::string> st = splitSchemaTable(chosen_net);
        std::string area_schema = st.first;
        std::string net_name = st.second;
        std::string prefix = net_name;
        if (ends_with(prefix, "road_way")) { prefix = prefix.substr(0, prefix.size()-8); }
        std::string area_name = prefix + "road_community";
        std::string area_full = area_schema.empty()? area_name : (area_schema + "." + area_name);

        PGconn* pconn = PQconnectdb(dbConnStr.c_str());
        if (pconn && PQstatus(pconn) == CONNECTION_OK) {
            
            std::string q_exist;
            if (!area_schema.empty()) {
                q_exist = "SELECT 1 FROM information_schema.tables WHERE table_schema='" + area_schema + "' AND table_name='" + area_name + "' LIMIT 1";
            } else {
                q_exist = "SELECT 1 FROM information_schema.tables WHERE table_name='" + area_name + "' LIMIT 1";
            }
            PGresult* rex = PQexec(pconn, q_exist.c_str());
            bool area_exists = (rex && PQresultStatus(rex) == PGRES_TUPLES_OK && PQntuples(rex) > 0);
            if (rex) PQclear(rex);

            if (!area_exists) {
                std::string warn = w2u8(L"Warning: 未找到小区面表: ") + area_full + w2u8(L"，请校验数据");
                std::cerr << warn << std::endl;
                TNM_AppendMessageNote(warn);
            } else {
                
                std::string q =
                    std::string("WITH cz AS (\n  SELECT DISTINCT \"centroid_matched_node\"::int AS zone\n  FROM ") + chosen_net + "\n  WHERE \"type\" = 10 AND \"centroid_matched_node\" IS NOT NULL\n), od AS (\n  SELECT DISTINCT \"f_id\"::int AS zone FROM " + chosen_od + " WHERE \"demand\" > 0\n  UNION SELECT DISTINCT \"t_id\"::int AS zone FROM " + chosen_od + " WHERE \"demand\" > 0\n), ar AS (\n  SELECT DISTINCT \"area_id\"::int AS zone FROM " + area_full + "\n)\nSELECT\n  (SELECT COUNT(*) FROM cz) AS centroid_zone_cnt,\n  (SELECT COUNT(*) FROM od) AS od_zone_cnt,\n  (SELECT COUNT(*) FROM ar) AS area_zone_cnt,\n  (SELECT COUNT(*) FROM (SELECT zone FROM od EXCEPT SELECT zone FROM cz) s)  AS od_not_in_centroid_cnt,\n  (SELECT COUNT(*) FROM (SELECT zone FROM cz EXCEPT SELECT zone FROM od) s)  AS centroid_not_in_od_cnt,\n  (SELECT COUNT(*) FROM (SELECT zone FROM od EXCEPT SELECT zone FROM ar) s)  AS od_not_in_area_cnt,\n  (SELECT COUNT(*) FROM (SELECT zone FROM ar EXCEPT SELECT zone FROM cz) s)  AS area_not_in_centroid_cnt;";
                std::cout << "[PreCheck] net=" << chosen_net << " od=" << chosen_od << " area=" << area_full << std::endl;
                PGresult* r = PQexec(pconn, q.c_str());
                if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) >= 1) {
                    auto to_i = [&](int c)->long long{ char* v = PQgetvalue(r, 0, c); return v? atoll(v) : 0; };
                    long long centroid_zone_cnt = to_i(0);
                    long long od_zone_cnt = to_i(1);
                    long long area_zone_cnt = to_i(2);
                    long long od_not_in_centroid_cnt = to_i(3);
                    long long centroid_not_in_od_cnt = to_i(4);
                    long long od_not_in_area_cnt = to_i(5);
                    long long area_not_in_centroid_cnt = to_i(6);
                    std::cout << "[PreCheck] centroid_zone_cnt=" << centroid_zone_cnt
                              << " od_zone_cnt=" << od_zone_cnt
                              << " area_zone_cnt=" << area_zone_cnt
                              << " od_not_in_centroid_cnt=" << od_not_in_centroid_cnt
                              << " centroid_not_in_od_cnt=" << centroid_not_in_od_cnt
                              << " od_not_in_area_cnt=" << od_not_in_area_cnt
                              << " area_not_in_centroid_cnt=" << area_not_in_centroid_cnt
                              << std::endl;
                    if (od_not_in_area_cnt > 0) {
                        std::string warn = w2u8(L"Warning: 小区id和OD起讫点id不匹配，请校核数据 (od_not_in_area_cnt=")
                                            + std::to_string(od_not_in_area_cnt)
                                            + ", od_table=" + chosen_od + ", area_table=" + area_full + ")";
                        std::cerr << warn << std::endl;
                        TNM_AppendMessageNote(warn);
                    }
                }
                if (r) PQclear(r);
            }
            PQfinish(pconn);
        } else {
            if (pconn) { PQfinish(pconn); }
        }
    } catch (...) {
        
    }

	/*Build the network using PostgreSQL database with custom table names*/
	if(network->BuildPostgreSQL(true, lpf, dbConnStr, networkTableName, odTableName)!=0) 
	{
		std::cout<<"\tEncounter problems when building a network object from PostgreSQL!"<<endl;
		// system("PAUSE");
		return 4;
	}

	network->ClearZeroDemandOD();
	return 0;
};

TERMFLAGS TNM_TAP::Solve()
{
	m_startRunTime = clock();//CPU time
	PreProcess();
	numLineSearch = 0;
	if(termFlag!=ErrorTerm)
	{
		curIter = 0;//Current iterations
		/*Get an initial solution*/
		Initialize();
		/*Solve the problem iteratively*/
		if(!Terminate())
		{
			RecordCurrentIter();
			do 
			{
				curIter ++;
				MainLoop();
				RecordCurrentIter();
			}while (!Terminate());
		}
		PostProcess();
		termFlag = TerminationType();
	}
	std::cout<<"\n[CRITIC]Solution process terminated"<<endl;
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
	std::cout<<*iterElem;
#endif
	iterRecord.push_back(iterElem); 
	__WriteProgressCacheJSON(curIter, convIndicator);
	__AppendProgressHistoryJSONL(iterElem->iter, iterElem->conv, iterElem->ofv, iterElem->time);
    // Invoke user progress callback if provided
    if (progressCallback) {
        // iteration-based percent
        int percent_iter = 0;
        if (maxMainIter > 0) {
            int p = (int)((curIter * 100.0) / (double)maxMainIter);
            if (p < 0) p = 0; if (p > 100) p = 100;
            percent_iter = p;
        }
        // convergence-based percent: convCriterion -> 100%, start (large gap) -> small
        int percent_conv = 0;
        if (convCriterion > 0) {
            double ratio = convIndicator <= 0 ? 1.0 : (convCriterion / convIndicator);
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
            percent_conv = (int)(ratio * 100.0);
            if (percent_conv < 0) percent_conv = 0; if (percent_conv > 100) percent_conv = 100;
        }
        int percent = (percent_iter > percent_conv ? percent_iter : percent_conv);
        if (percent > 98) percent = 98;
        std::cout << "[PROGRESS] iter=" << curIter << " precision=" << (double)convIndicator << " percent=" << percent << std::endl;
        progressCallback(curIter, (double)convIndicator, percent);
    }
    return iterElem;
}

bool TNM_TAP::Terminate()
{
    bool terminate = ReachAccuracy() || ReachMaxIter() || ReachError() || ReachUser();
    if (terminate && progressCallback) {
        int percent = 98;
        std::cout << "[PROGRESS] iter=" << curIter << " precision=" << (double)convIndicator << " percent=" << percent << std::endl;
        progressCallback(curIter, (double)convIndicator, percent);
    }
    return terminate;
}

TERMFLAGS TNM_TAP::TerminationType()
{
	if(ReachError())    return ErrorTerm;
	if(ReachAccuracy()) return ConvergeTerm;
	if(ReachMaxIter())  return MaxIterTerm;
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
			std::cout<<"The link is null in calculating the direction for OFW algorithm"<<endl;
			break;
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


floatType TNM_TAP::RelativeGap2(bool scale)
{
	TNM_SLINK* link;
	TNM_SORIGIN* org;
	TNM_SDEST* dest;
	floatType gap, tt = 0.0, tmd = 0.0;

	for (int i = 0; i < network->numOfLink; i++)
	{
		link = network->linkVector[i];
		tt += link->volume * link->cost;
		//gap += link->volume * link->cost;
	}
	gap = tt;
	for (int i = 0; i < network->numOfOrigin; i++)
	{
		org = network->originVector[i];
		tmd += org->m_tdmd;
		for (int j = 0; j < org->numOfDest; j++)
		{
			dest = org->destVector[j];
			if (dest->assDemand == 0.0) continue;
			if (org->origin == dest->dest) continue;
			if (!network->SPath(org->origin, dest->dest))
			{
				termFlag = ErrorTerm;
				return fabs(gap);
			}
			gap -= (org->origin->pathElem->cost * dest->assDemand);
		}
	}

	if (scale && tt > 0.0) gap /= tt;

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
			std::cout<<"\n\tFail to build an algorithm object: cannot open .ite file to write!"<<endl;
		}
		else
		{
			std::cout<<"\tWriting the iteration history into file "<<(outFileName + ".ite")<<"..."<<endl;
			ReportIter(iteFile);
		}
	}
	if(reportLinkDetail)
	{
		string lfpFileName  = outFileName + ".lfp";
		if (!TNM_OpenOutFile(lfpFile, lfpFileName))
		{
			std::cout<<"\n\tFail to Initialize an algorithm object: Cannot open .lfp file to write!"<<endl;
		}
		else
		{
			std::cout<<"\tWriting link details into file "<<(outFileName + ".lfp")<<"..."<<endl;
			ReportLink(lfpFile); 
			
		}

	}
	if(reportPathDetail)
	{
		string pthFileName  = outFileName + ".pth";
		if (!TNM_OpenOutFile(pthFile, pthFileName))
		{
			std::cout<<"\n\tFail to Initialize an algorithm object: Cannot open .pth file to write!"<<endl;
		}
		else
		{
			std::cout<<"\tWriting path details into file "<<(outFileName + ".pth")<<"..."<<endl;
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
	out<<"path id      "<<"origin       "<<" dest      "<<"     path flow    "<<"   num of link    "<<"   links  "<<endl;
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
					out<<TNM_IntFormat(pthid,4)<<TNM_IntFormat(pOrg->origin->id,4)<<TNM_IntFormat(sdest->dest->id,4)<<TNM_FloatFormat(path->flow,12,6)<< " "
						<<TNM_IntFormat(path->path.size(),4);
					for (int li=0; li<path->path.size();li++)
					{
						TNM_SLINK* link = path->path[li];
						out<<TNM_IntFormat(link->id,4);
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
				<<TNM_FloatFormat(link->capacity)<<" "
				<<TNM_FloatFormat(link->volume,18,11)<< " "
				<<TNM_FloatFormat(link->cost)<< " "
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
				<<TNM_FloatFormat((*pv)->ofv)<< " "
				<<TNM_FloatFormat((*pv)->conv)<< " "
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

	std::cout << "[DEBUG] num of origin is " << network->numOfOrigin << endl;
	std::cout << "[DEBUG] num of nodes is " << network->numOfNode << endl;

	network->UpdateLinkCost();
	network->InitialSubNet3();//Implementing the all-or-nothing algorithm
	network->UpdateLinkCost();


	//network->UpdateLinkCostDer();
	ComputeOFV(); //compute objective function value

	std::cout<<"The initial objective is : "<<OFV<<endl;
	
	for (int oi=0;oi<network->numOfOrigin;oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di=0;di<pOrg->numOfDest;di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];
			if(pOrg->origin == sdest->dest) continue;
			TNM_SPATH* spath = sdest->pathSet.front();
			nPath++;
			spath->id = nPath;
				
		}
	}

	std::cout<<"[DEBUG] The num of links  is "<<network->numOfLink<<endl;
	std::cout<<"[DEBUG] end of the ini "<<endl;

}

void TAP_Greedy::PostProcess()
{
	network->UpdateLinkCost();
}

TAP_Greedy::TAP_Greedy()
{
	aveFlowChange = 1.0;
	convIndicator = 1.0;
	yPath = new TNM_SPATH;
}

TAP_Greedy::~TAP_Greedy()
{
	delete yPath;
	yPath = NULL;
}

void TAP_Greedy::MainLoop()
{
	TotalFlowChange = 0.0;
	numOfPathChange =0;
	floatType oldOFV = OFV;
	totalShiftFlow = 0.0;
	maxPathGap = 0;

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
			if(pOrg->origin == dest->dest) continue;
			dest->shiftFlow = 1.0;
			/*Updating the path set*/
			ColumnGeneration(pOrg,dest);
		    
			columnG = true;
			/*Perform a flow shift*/
			UpdatePathFlowGreedy(pOrg,dest);
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
				if(pOrg->origin == dest->dest) continue;
				if (il%(maxInIter/100) == 0)
				{
					dest->shiftFlow =1.0;
				}
				if (dest->shiftFlow> convIndicator/2.0)
				{
					columnG = false;
					/*Perform a flow shift*/
					UpdatePathFlowGreedy(pOrg,dest);
					numofD2++;
					
				}
				numofPath+=dest->pathSet.size();
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
	for (int pi=0; pi<dePathSet.size(); pi++)
	{
		path = dePathSet[pi];
		delete path;
	}
	dePathSet.clear();
	//
	convIndicator = RelativeGap();//compute Relative Gap
	aveFlowChange = TotalFlowChange/numOfPathChange;

	ComputeOFV(); //compute objective function value;


	std::cout<<"[DEBUG] iter="<<curIter<<endl;
	std::cout<<"[DEBUG] OFV="<<OFV<<endl;
	std::cout<<"[DEBUG] gap="<<convIndicator<<endl;
	std::cout<<"[DEBUG] shifted_flow="<<totalShiftFlow<<endl;
	std::cout<<"[DEBUG] il="<<il<<endl;
	std::cout<<"[DEBUG] inner_flow_shift="<<innerShiftFlow<<endl;
	std::cout<<"[DEBUG] dest_shifted="<<numOfD<<endl;
	std::cout<<"[DEBUG] searched_od_pairs="<<numofD2<<endl;
	std::cout<<"[DEBUG] max_path_cost_gap="<<maxPathGap<<endl;
	std::cout<<"[DEBUG] path_count="<<numofPath<<endl;
	std::cout<<"[DEBUG] dePathSet_size="<<depathsetsize<<endl;
	std::cout<<"[DEBUG] elapsed="<<1.0*(clock() - m_startRunTime)/CLOCKS_PER_SEC<<endl;
	
}

void TAP_Greedy::UpdatePathFlowGreedy(TNM_SORIGIN* pOrg,TNM_SDEST* dest)
{
	//check if the yPath is existed in the path set
	bool find = true;
	TNM_SLINK* slink;
	TNM_SPATH* path;
	// Initialize shiftFlow - will be updated based on actual flow changes
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
			if (greedy_od_trace::pair(pOrg->origin->id, dest->dest->id))
			{
				std::cout << "[OD_TRACE] UpdatePathFlowGreedy ADD path OD(" << pOrg->origin->id << "->"
				          << dest->dest->id << ") new_id=" << nPath << " nlinks=" << addPath->path.size()
				          << " pathSet.size=" << dest->pathSet.size() << std::endl;
			}
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
				path->preRatio = path->preFlow/dest->assDemand;
				path->cost = 0.0;
				path->fdCost = 0.0;
				path->curRatio =0.0;
				path->markStatus = 0;
				for (int li=0;li<path->path.size();li++)
				{
					slink = path->path[li];
					path->cost+=slink->cost;
					path->fdCost+= slink->fdCost;

				}
				path->estCost=path->cost - path->fdCost*dest->assDemand*path->preRatio;
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
			if (dest->pathSet.size() > 1 && numOfD < 3) { // Debug first few multi-path ODs
				std::cout << "[DEBUG] MultiPath OD(" << pOrg->origin->id << "->" << dest->dest->id 
						  << ") paths=" << dest->pathSet.size() << " costDiff=" << (maxCost-minCost) 
						  << " ss=" << ss << " willReassign=" << (maxCost-minCost > ss ? "YES" : "NO") << std::endl;
			}
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
						kpath->curRatio = (w-kpath->estCost)/(kpath->fdCost*dest->assDemand);

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
								slink->volume = slink->volume + dflow;
								if ( abs(slink->volume) < 1e-8)
								{
									slink->volume =0.0;
								}
								if (slink->volume<0)
								{
									std::cout<<"status "<<1<<endl;
									std::cout<<"slink's volume is "<<slink->volume<<endl;
									std::cout<<"dflow is "<<dflow<<endl;
									std::cout<<"curRatio is "<<path->curRatio<<endl;
									std::cout<<"path flow "<<path->flow<<endl;
									std::cout<<"path preflow is "<<path->preFlow<<endl;
									std::cout<<"demand is "<<dest->assDemand<<endl;
#ifdef _WIN32
									system("PAUSE");
#endif
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



//////////////////////////////methods for Greedy_dijk
TAP_Greedy_dijk::TAP_Greedy_dijk( )
{

}

TAP_Greedy_dijk::~TAP_Greedy_dijk()
{

}

void TAP_Greedy_dijk::Initialize()
{

	network->AllocateNodeBuffer(2);
	network->AllocateLinkBuffer(3);

	std::cout << "[DEBUG] num of origin is " << network->numOfOrigin << endl;
	std::cout << "[DEBUG] num of nodes is " << network->numOfNode << endl;
	std::cout << "[DEBUG] num of OD is " << network->numOfOD << endl;

	network->UpdateLinkCost();
	if (!network->InitialSubNet4())
	{
		termFlag = ErrorTerm;
		return;
	}
	network->UpdateLinkCost();

	//network->UpdateLinkCostDer();
	ComputeOFV(); //compute objective function value

	std::cout << "[DEBUG] The initial objective is : " << OFV << endl;

	for (int oi = 0; oi < network->numOfOrigin; oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di = 0; di < pOrg->numOfDest; di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];
			if (pOrg->origin == sdest->dest) continue;
			if (sdest->assDemand == 0.0 || sdest->pathSet.empty()) continue;
			TNM_SPATH* spath = sdest->pathSet.front();
			nPath++;
			spath->id = nPath;
			greedy_od_trace::log_pathset("Initialize", pOrg->origin->id, sdest->dest->id,
			                             sdest->assDemand, sdest->pathSet);
		}
	}

	std::cout << "[DEBUG] The num of links is " << network->numOfLink << endl;

	std::cout << "[DEBUG] end of the ini " << endl;

}


void TAP_Greedy_dijk::MainLoop()
{
	TotalFlowChange = 0.0;
	numOfPathChange = 0;
	floatType oldOFV = OFV;
	totalShiftFlow = 0.0;
	maxPathGap = 0;

	/*Loop over all OD pairs, update the path set and perform a flow shift*/
	TNM_SORIGIN* pOrg;
	for (int i = 0; i < network->numOfOrigin; i++)
	{
		pOrg = network->originVector[i];
		/*Updating the shortest path tree*/
		//network->UpdateSP(pOrg->origin);
		for (int j = 0; j < pOrg->numOfDest; j++)
		{
			TNM_SDEST* dest = pOrg->destVector[j];
			if (dest->assDemand == 0.0) continue;
			if (pOrg->origin == dest->dest) continue;
			dest->shiftFlow = 1.0;
			/*Updating the path set*/
			ColumnGeneration(pOrg, dest);
			if (ReachError())
				return;

			columnG = true;
			/*Perform a flow shift*/
			UpdatePathFlowGreedy(pOrg, dest);
			greedy_od_trace::log_pathset(
			    (std::string("MainLoop_colGen iter=") + std::to_string(curIter)).c_str(),
			    pOrg->origin->id, dest->dest->id, dest->assDemand, dest->pathSet);
		}

	}

	/*Inner loop*/
	innerShiftFlow = 1.0;
	int il;
	int numofPath;
	double preFlowPre = 1e-10;
	maxPathGap = 0.0;
	numOfD = 0;
	int numofD2 = 0;
	int maxInIter = 500;
	
	// Calculate current relative gap for inner loop threshold
	double currentGap = RelativeGap2();
	if (ReachError())
		return;
	double innerThreshold = currentGap / 2.0;
	
	std::cout << "[DEBUG] Before inner loop: old_convIndicator=" << convIndicator 
			  << " currentGap=" << currentGap << " innerThreshold=" << innerThreshold << std::endl;
	
	for (il = 0; il < maxInIter; il++)
	{
		innerShiftFlow = 0.0;

		numofPath = 0;
		numofD2 = 0;
		int multiPathCount = 0;
		for (int i = 0; i < network->numOfOrigin; i++)
		{
			pOrg = network->originVector[i];

			for (int j = 0; j < pOrg->numOfDest; j++)
			{
				TNM_SDEST* dest = pOrg->destVector[j];
				if (dest->assDemand == 0.0) continue;
				if (pOrg->origin == dest->dest) continue;
				if (il % (maxInIter /100) == 0)
				{
					dest->shiftFlow = 1.0;
				}
				// Also search if this OD has multiple paths or significant flow
				if (dest->pathSet.size() > 1 && il == 0) {
					dest->shiftFlow = 1.0;
				}
				if (dest->shiftFlow > innerThreshold)
				{
					columnG = false;
					/*Perform a flow shift*/
					UpdatePathFlowGreedy(pOrg, dest);
					numofD2++;
				}
				numofPath += dest->pathSet.size();
				if (dest->pathSet.size() > 1) multiPathCount++;
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
	for (int pi = 0; pi < dePathSet.size(); pi++)
	{
		path = dePathSet[pi];
		delete path;
	}
	dePathSet.clear();
	//
	convIndicator = RelativeGap2();//compute Relative Gap
	if (ReachError())
		return;
	aveFlowChange = TotalFlowChange/numOfPathChange;

	ComputeOFV(); //compute objective function value;

	static bool s_greedy_debug = (std::getenv("TNA_GREEDY_DEBUG") != nullptr);
	if (s_greedy_debug)
	{
		std::cout << "[DEBUG] iter=" << curIter << " gap=" << convIndicator
		          << " elapsed=" << 1.0 * (clock() - m_startRunTime) / CLOCKS_PER_SEC << endl;
	}

}

void TAP_Greedy_dijk::ColumnGeneration(TNM_SORIGIN* pOrg, TNM_SDEST* dest)
{
    if (!yPath)
        yPath = new TNM_SPATH;
    yPath->path.clear();
    yPath->flow = 0;
    yPath->cost = 0;
    TNM_SPATH* sp = network->SPath(pOrg->origin, dest->dest);
    if (!sp)
    {
        termFlag = ErrorTerm;
        return;
    }
    yPath->path = sp->path;
    yPath->cost = sp->cost;
    yPath->flow = sp->flow;
    if (greedy_od_trace::pair(pOrg->origin->id, dest->dest->id))
    {
        std::cout << "[OD_TRACE] ColumnGeneration OD(" << pOrg->origin->id << "->" << dest->dest->id
                  << ") yPath nlinks=" << yPath->path.size() << " cost=" << yPath->cost << std::endl;
    }
    delete sp;
}

void TNM_TAP::WritePG(const string& dbConnStr, const string& networkTableName, const string& scenarioPrefix)
{
    // 设置控制台输出编码为 UTF-8，解决中文乱码问题
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::cout << "[INFO] Begin to write Greedy results into PostgreSQL..." << endl;
	TNM_ResetTouchedTables();

    // 连接数据库
    PGconn* conn = PQconnectdb(dbConnStr.c_str());
    if (PQstatus(conn) == CONNECTION_BAD) {
        std::cout << "[ERROR] Connection to database failed: " << PQerrorMessage(conn) << endl;
        PQfinish(conn);
        return;
    }
    PQsetClientEncoding(conn, "UTF-8");

    // 解析 schema 与网络表名
    auto trim_copy = [](string s){ while(!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin()); while(!s.empty() && (s.back()==' '||s.back()=='\t')) s.pop_back(); return s; };
    auto extractSchemaFromConn = [&](const string& connStr)->string{
        size_t p = connStr.find("role=");
        if (p != string::npos) { p += 5; size_t q = p; while (q < connStr.size() && connStr[q] != ' ' && connStr[q] != '\'' && connStr[q] != '"') q++; string role = connStr.substr(p, q - p); while(!role.empty() && (role.back()=='\''||role.back()=='"')) role.pop_back(); return trim_copy(role);} return string(); };
    // auto splitSchemaTable = [&](const string& qname){ struct { string schema; string table; } r; size_t dot = qname.find('.'); if (dot == string::npos) { r.schema = string(); r.table = qname; } else { r.schema = qname.substr(0, dot); r.table = qname.substr(dot+1); } return r; };
	struct SchemaTable { string schema; string table; };
	auto splitSchemaTable = [&](const string& qname) -> SchemaTable {
		SchemaTable r;
		size_t dot = qname.find('.');
		if (dot == string::npos) { r.schema = string(); r.table = qname; }
		else { r.schema = qname.substr(0, dot); r.table = qname.substr(dot + 1); }
		return r;
	};
    string schemaSource; // "role" | "network_table_name" | ""
    string schema = extractSchemaFromConn(dbConnStr);
    if (!schema.empty()) { schemaSource = "role"; std::cout << "[DEBUG]  role schema " << schema << endl; }


    string qualifiedNetworkTable = networkTableName; string netBaseName = networkTableName; string netSchema;
    
	cout << "[DEBUG]  qualifiedNetworkTable " << qualifiedNetworkTable << endl;
	if (!networkTableName.empty()) {
        auto st = splitSchemaTable(networkTableName);
        if (schema.empty() && !st.schema.empty()) { schema = st.schema; if (schemaSource.empty()) schemaSource = "network_table_name"; }
        if (st.schema.empty()) { if (!schema.empty()) { qualifiedNetworkTable = schema + "." + st.table; netSchema = schema; } else { qualifiedNetworkTable = st.table; netSchema.clear(); } netBaseName = st.table; }
        else { qualifiedNetworkTable = st.schema + "." + st.table; netBaseName = st.table; netSchema = st.schema; }
    }
	std::cout << "[DEBUG]  qualified schema " << schema << endl;
    // 输出表（基础名 + schema 限定）
    string rawScenario = trim_copy(scenarioPrefix);
    while (!rawScenario.empty() && rawScenario.back() == '_') rawScenario.pop_back();
    string prefix = rawScenario.empty() ? "" : rawScenario + "_";
    string baseLinkFlowTable = prefix + "greedy_link_flow_results";
    string basePathResultsTable = prefix + "greedy_path_results";
    string baseIterationHistoryTable = prefix + "greedy_iteration_history";
    string baseSummaryStatsTable = prefix + "greedy_summary_statistics";


	
    auto qualifyOut = [&](const string& base)->string{ return schema.empty()? base : (schema + "." + base); };
    // 为 SQL 中的表名加双引号，处理以数字开头或含特殊字符的情况
    auto pg_quote_ident = [](const std::string& name) -> std::string {
        size_t dot = name.find('.');
        if (dot != std::string::npos)
            return "\"" + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
        return "\"" + name + "\"";
    };
    auto w2u8 = [](const wchar_t* ws)->std::string {
        if (!ws) return std::string();
#ifdef _WIN32
        int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
        std::string out;
        if (len > 0) {
            out.resize(static_cast<size_t>(len) - 1);
            WideCharToMultiByte(CP_UTF8, 0, ws, -1, &out[0], len, NULL, NULL);
        }
        return out;
#else
        std::string out;
        for (const wchar_t* p = ws; *p; ++p) {
            wchar_t c = *p;
            if (c < 0x80) { out += (char)c; }
            else if (c < 0x800) { out += (char)(0xC0|(c>>6)); out += (char)(0x80|(c&0x3F)); }
            else { out += (char)(0xE0|(c>>12)); out += (char)(0x80|((c>>6)&0x3F)); out += (char)(0x80|(c&0x3F)); }
        }
        return out;
#endif
    };
    string linkFlowTable = qualifyOut(baseLinkFlowTable);
    string pathResultsTable = qualifyOut(basePathResultsTable);
    string iterationHistoryTable = qualifyOut(baseIterationHistoryTable);
    string summaryStatsTable = qualifyOut(baseSummaryStatsTable);
    // SQL 中使用的已引号化版本（防止表名以数字开头导致语法错误）
    string qLinkFlowTable             = pg_quote_ident(linkFlowTable);
    string qPathResultsTable          = pg_quote_ident(pathResultsTable);
    string qIterationHistoryTable     = pg_quote_ident(iterationHistoryTable);
    string qSummaryStatsTable         = pg_quote_ident(summaryStatsTable);
    string qQualifiedNetworkTable     = pg_quote_ident(qualifiedNetworkTable);

    auto execComment = [&](const string& sql) {
        PGresult* r = PQexec(conn, sql.c_str());
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            std::cout << "[ERROR] Comment failed: " << PQerrorMessage(conn) << endl;
            std::cout << "[DEBUG] SQL: " << sql << endl;
        }
        if (r) PQclear(r);
    };

    try
    {
        const char* env_mode = getenv("TNA_OD_MODE");
        std::string od_mode = (env_mode && *env_mode) ? std::string(env_mode) : std::string();
        if (od_mode.empty()) od_mode = "unknown";
        		bool do_od_backfill = !(od_mode == "road_node_fallback"
                    || od_mode == "centroid_connector_prebuilt"
                    || od_mode == "centroid_connector_multi_osm_prebuilt"
                    || od_mode == "centroid_connector_existing");
		if (!do_od_backfill) {
			std::string note;
			if (od_mode == "road_node_fallback") {
				note = w2u8(L"[TNA_DLL][OD_MODE] 本次运行使用道路节点模式：OD的f_id/t_id直接作为道路节点ID（init_node/term_node体系）。跳过WritePG的ODBackfill（不重建质心连杆/不回填od_centroid_line_geom）。");
			} else {
				note = w2u8(L"[TNA_DLL][OD_MODE] 本次运行已在构网阶段预生成质心连杆(type=10)。跳过WritePG的ODBackfill（不重建质心连杆/不回填od_centroid_line_geom）。");
			}
			std::cout << note << std::endl;
			TNM_AppendMessageNote(note);
		}

		if (do_od_backfill) {
			const char* env_od = getenv("PG_TABLE_OD");
					std::string odSourceTable = (env_od && *env_od) ? std::string(env_od) : std::string();
			std::cout << "[ODBackfill] PG_TABLE_OD=" << (env_od ? env_od : "<NULL>") << std::endl;
			std::cout << "[ODBackfill] schema='" << schema << "'" << std::endl;
			std::cout << "[ODBackfill] scenario_prefix(raw)='" << scenarioPrefix << "'" << std::endl;

		auto table_exists2 = [&](const std::string& qname)->bool{
			SchemaTable st = splitSchemaTable(qname);
			std::string q;
			if (!st.schema.empty()) q = "SELECT 1 FROM information_schema.tables WHERE table_schema='" + st.schema + "' AND table_name='" + st.table + "' LIMIT 1";
			else q = "SELECT 1 FROM information_schema.tables WHERE table_schema=current_schema() AND table_name='" + st.table + "' LIMIT 1";
			PGresult* r = PQexec(conn, q.c_str());
			bool ok = (r && PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)>0);
			if (r) PQclear(r);
			return ok;
		};
		auto column_exists2 = [&](const std::string& qname, const std::string& col)->bool{
			SchemaTable st = splitSchemaTable(qname);
			std::string q;
			if (!st.schema.empty()) q = "SELECT 1 FROM information_schema.columns WHERE table_schema='" + st.schema + "' AND table_name='" + st.table + "' AND column_name='" + col + "' LIMIT 1";
			else q = "SELECT 1 FROM information_schema.columns WHERE table_schema=current_schema() AND table_name='" + st.table + "' AND column_name='" + col + "' LIMIT 1";
			PGresult* r = PQexec(conn, q.c_str());
			bool ok = (r && PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)>0);
			if (r) PQclear(r);
			return ok;
		};

		auto normalize_prefix = [&](std::string p)->std::string{
			trim_inplace(p);
			while(!p.empty() && p.back()=='_') p.pop_back();
			return p;
		};

		std::string normalized_prefix = normalize_prefix(scenarioPrefix);
		std::cout << "[ODBackfill] scenario_prefix(normalized)='" << normalized_prefix << "'" << std::endl;
		std::cout << "[ODBackfill] od_table='" << odSourceTable << "'" << std::endl;
		if (!normalized_prefix.empty() && !odSourceTable.empty()) {
			TNM_AppendTouchedTable(odSourceTable);

			std::string communityBase = normalized_prefix + "_road_community";
			std::string communityTable = schema.empty() ? communityBase : (schema + "." + communityBase);
			std::cout << "[ODBackfill] community_table='" << communityTable << "'" << std::endl;
			TNM_AppendTouchedTable(communityTable);
			if (!table_exists2(communityTable)) {
				throw std::runtime_error(w2u8(L"输入小区图层不存在，请检查数据。community_table=") + communityTable);
			}

			std::string pointBase = normalized_prefix + "_road_point";
			std::string pointTable = schema.empty() ? pointBase : (schema + "." + pointBase);
			std::cout << "[ODBackfill] point_table='" << pointTable << "'" << std::endl;
			TNM_AppendTouchedTable(pointTable);
			if (!table_exists2(pointTable)) {
				throw std::runtime_error(w2u8(L"对应的点图层不存在") + std::string(" (point_table=") + pointTable + ")");
			}

			long long srid = 4326;
			std::cout << "[ODBackfill] srid(fixed)=" << srid << std::endl;

			std::string centroidBase = communityBase + "_centeroid";
			std::string centroidTable = schema.empty() ? centroidBase : (schema + "." + centroidBase);
			std::string centroidToLineBase = communityBase + "_centeroid_to_line";
			std::string centroidToLineTable = schema.empty() ? centroidToLineBase : (schema + "." + centroidToLineBase);
			std::cout << "[ODBackfill] centroid_table='" << centroidTable << "'" << std::endl;
			std::cout << "[ODBackfill] centroid_to_line_table='" << centroidToLineTable << "'" << std::endl;
			TNM_AppendTouchedTable(centroidTable);
			TNM_AppendTouchedTable(centroidToLineTable);

			std::string wayTable = qualifiedNetworkTable.empty() ? (schema.empty() ? (normalized_prefix + "_road_way") : (schema + "." + normalized_prefix + "_road_way")) : qualifiedNetworkTable;
			std::cout << "[ODBackfill] way_table='" << wayTable << "'" << std::endl;
			{
				bool centroid_exists = table_exists2(centroidTable);
				bool c2l_exists = table_exists2(centroidToLineTable);
				std::string d1 = "DELETE FROM " + centroidTable + ";";
				std::string d2 = "DELETE FROM " + centroidToLineTable + ";";
				std::string delWay = "DELETE FROM " + wayTable + " WHERE \"type\" = 10;";
				if (centroid_exists) {
					std::cout << "[ODBackfill] " << d1 << std::endl;
					PGresult* r1 = PQexec(conn, d1.c_str());
					if (!r1 || PQresultStatus(r1) != PGRES_COMMAND_OK) {
						std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
						if (r1) PQclear(r1);
						throw std::runtime_error(w2u8(L"清空质心表失败（需要对该表有DELETE权限；若无权限请让owner执行GRANT DELETE）: ") + em);
					}
					PQclear(r1);
				}
				else {
					std::cout << "[ODBackfill] centroid_table not exists, skip delete." << std::endl;
				}
				if (c2l_exists) {
					std::cout << "[ODBackfill] " << d2 << std::endl;
					PGresult* r2 = PQexec(conn, d2.c_str());
					if (!r2 || PQresultStatus(r2) != PGRES_COMMAND_OK) {
						std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
						if (r2) PQclear(r2);
						throw std::runtime_error(w2u8(L"清空质心连杆表失败（需要对该表有DELETE权限；若无权限请让owner执行GRANT DELETE）: ") + em);
					}
					PQclear(r2);
				}
				else {
					std::cout << "[ODBackfill] centroid_to_line_table not exists, skip delete." << std::endl;
				}
				std::cout << "[ODBackfill] " << delWay << std::endl;
				PGresult* r3 = PQexec(conn, delWay.c_str());
				if (r3 && PQresultStatus(r3) != PGRES_COMMAND_OK) {
					std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
					PQclear(r3);
					throw std::runtime_error(w2u8(L"清理路网表旧质心连杆失败（需要DELETE权限）: ") + em);
				}
				if (r3) PQclear(r3);
			}

			{
				if (!table_exists2(centroidTable)) {
					std::string sql = "CREATE TABLE " + centroidTable + " (area_id BIGINT GENERATED BY DEFAULT AS IDENTITY, geometry GEOMETRY(Point, " + std::to_string(srid) + "));";
					std::cout << "[ODBackfill] " << sql << std::endl;
					PGresult* r = PQexec(conn, sql.c_str());
					if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
						std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
						if (r) PQclear(r);
						throw std::runtime_error(w2u8(L"创建质心表失败: ") + em);
					}
					PQclear(r);
				}
			}

			{
				if (!table_exists2(centroidToLineTable)) {
					std::string sql = "CREATE TABLE " + centroidToLineTable + " (link_id BIGINT GENERATED BY DEFAULT AS IDENTITY, init_node BIGINT, term_node BIGINT, capacity FLOAT8 DEFAULT 9999999, b FLOAT8 DEFAULT 0.15, power FLOAT8 DEFAULT 4, toll FLOAT8 DEFAULT 0, type BIGINT DEFAULT 10, fft FLOAT8 DEFAULT 0.000001, speedlimit FLOAT8 DEFAULT 30, length numeric(10,4) DEFAULT 0, geometry GEOMETRY(LineString, " + std::to_string(srid) + "));";
					std::cout << "[ODBackfill] " << sql << std::endl;
					PGresult* r = PQexec(conn, sql.c_str());
					if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
						std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
						if (r) PQclear(r);
						throw std::runtime_error(w2u8(L"创建质心连杆表失败: ") + em);
					}
					PQclear(r);
				}
			}

			{
				std::string sql = "ALTER TABLE " + communityTable + " ADD COLUMN IF NOT EXISTS zx GEOMETRY(Point, " + std::to_string(srid) + ");";
				std::cout << "[ODBackfill] " << sql << std::endl;
				PGresult* r = PQexec(conn, sql.c_str()); if (r) PQclear(r);
			}
			{
				std::string upd = "UPDATE " + communityTable + " SET zx = ST_Centroid(\"geometry\") WHERE \"geometry\" IS NOT NULL;";
				std::cout << "[ODBackfill] " << upd << std::endl;
				PGresult* r = PQexec(conn, upd.c_str()); if (r) PQclear(r);
			}
			{
				std::string ins = "INSERT INTO " + centroidTable + " (area_id, geometry) SELECT area_id::bigint, ST_Centroid(\"geometry\") FROM " + communityTable + " WHERE \"geometry\" IS NOT NULL;";
				std::cout << "[ODBackfill] " << ins << std::endl;
				PGresult* r = PQexec(conn, ins.c_str());
				if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
					std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
					if (r) PQclear(r);
					throw std::runtime_error(w2u8(L"灌入质心表失败: ") + em);
				}
				PQclear(r);
			}

			{
				std::string ins =
					"INSERT INTO " + centroidToLineTable + " (init_node, term_node, geometry, length)\n"
					"SELECT ra.area_id AS init_node, n.node_id AS term_node, ST_MakeLine(ra.zx, n.geometry) AS geometry,\n"
					"  ST_Distance(ST_Transform(ra.zx, 3857), ST_Transform(n.geometry, 3857)) / 1000 AS length\n"
					"FROM " + communityTable + " ra\n"
					"CROSS JOIN LATERAL (\n"
					"  SELECT node_id, geometry FROM " + pointTable + " ORDER BY ra.zx <-> geometry LIMIT 1\n"
					") AS n\n"
					"WHERE ra.zx IS NOT NULL;";
				std::cout << "[ODBackfill] " << ins << std::endl;
				PGresult* r = PQexec(conn, ins.c_str());
				if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
					std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
					if (r) PQclear(r);
					throw std::runtime_error(w2u8(L"生成质心连杆数据失败: ") + em);
				}
				PQclear(r);
			}

			{
				std::string ins =
					"WITH bm AS (SELECT GREATEST("
					"  COALESCE(MAX(init_node),0),"
					"  COALESCE(MAX(term_node),0),"
					"  COALESCE((SELECT MAX(node_id) FROM " + pg_quote_ident(pointTable) + "),0)"
					") AS base_max FROM " + wayTable + " WHERE \"type\" <> 10),\n"
					"lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM " + wayTable + "),\n"
					"base AS (\n"
					"  SELECT (t.init_node + bm.base_max + 1)::bigint AS init_node, t.term_node::bigint AS term_node,\n"
					"         t.capacity, t.b, t.power, t.toll, t.type, t.fft, t.speedlimit, t.length, t.geometry,\n"
					"         t.init_node::bigint AS centroid_matched_node\n"
					"  FROM " + centroidToLineTable + " t, bm\n"
					"),\n"
					"both_dirs AS (\n"
					"  SELECT * FROM base\n"
					"  UNION ALL\n"
					"  SELECT term_node AS init_node, init_node AS term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node FROM base\n"
					")\n"
					"INSERT INTO " + wayTable + " (link_id, init_node, term_node, capacity, b, power, toll, \"type\", fft, speedlimit, length, geometry, centroid_matched_node)\n"
					"SELECT (lm.link_max + row_number() OVER ())::bigint AS link_id, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node\n"
					"FROM both_dirs, lm;";
				std::cout << "[ODBackfill] " << ins << std::endl;
				PGresult* r = PQexec(conn, ins.c_str());
				if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
					std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
					if (r) PQclear(r);
					throw std::runtime_error(w2u8(L"插入路网质心连杆失败: ") + em);
				}
				PQclear(r);
			}

			{
				std::string addCols = "ALTER TABLE " + odSourceTable +
					" ADD COLUMN IF NOT EXISTS od_centroid_line_geom GEOMETRY(LineString, " + std::to_string(srid) + ");";
				std::cout << "[ODBackfill] " << addCols << std::endl;
				PGresult* r = PQexec(conn, addCols.c_str());
				if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
					std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
					std::cerr << "[ODBackfill][ERROR] Failed to add geometry columns to OD table: " << em << std::endl;
				}
				if (r) PQclear(r);

				std::string odColOrg;
				std::string odColDst;
				if (column_exists2(odSourceTable, "f_id") && column_exists2(odSourceTable, "t_id")) {
					odColOrg = "f_id";
					odColDst = "t_id";
				}
				else {
					std::string em = std::string("[ODBackfill] OD表缺少必要列 f_id/t_id，无法回填 od_centroid_line_geom。od_table=") + odSourceTable;
					std::cerr << "[ODBackfill][ERROR] " << em << std::endl;
					TNM_AppendMessageNote(em);
					throw std::runtime_error(em);
				}

				{
					std::string updLine = "UPDATE " + odSourceTable + " od SET od_centroid_line_geom = ST_MakeLine(co.geometry, cd.geometry) "
						"FROM " + centroidTable + " co, " + centroidTable + " cd "
						"WHERE od.\"" + odColOrg + "\" = co.area_id AND od.\"" + odColDst + "\" = cd.area_id AND co.geometry IS NOT NULL AND cd.geometry IS NOT NULL;";
					std::cout << "[ODBackfill] " << updLine << std::endl;
					r = PQexec(conn, updLine.c_str());
					if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
						std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
						std::cerr << "[ODBackfill][ERROR] Failed to update od_centroid_line_geom: " << em << std::endl;
					}
					else {
						char* tu = PQcmdTuples(r);
						std::cout << "[ODBackfill] Updated od_centroid_line_geom rows=" << (tu ? tu : "") << std::endl;
					}
					if (r) PQclear(r);
				}

				std::string cmt3 = "COMMENT ON COLUMN " + odSourceTable + ".od_centroid_line_geom IS '" + w2u8(L"质心连接线坐标") + "';";
				std::cout << "[ODBackfill] " << cmt3 << std::endl;
				r = PQexec(conn, cmt3.c_str()); if (r) PQclear(r);
			}
		}
		}
	}
	catch (const std::exception& e)
    	{
			std::string em = e.what();
			std::cerr << "[ODBackfill][ERROR] " << em << std::endl;
			TNM_AppendMessageNote(em);
			throw;
    	}
    	catch (...)
    	{
			std::string em = w2u8(L"小区数据不存在，或者小区ID和OD需求数据起讫点id不匹配，请检查数据");
			std::cerr << "[ODBackfill][ERROR] " << em << std::endl;
			TNM_AppendMessageNote(em);
			throw;
    	}



    std::cout << "[INFO] Output table names (schema-qualified):" << endl;
    std::cout << "[INFO] Schema: " << schema << endl;
    std::cout << "[INFO] Link flow results: " << linkFlowTable << endl;
    std::cout << "[INFO] Path results: " << pathResultsTable << endl;
    std::cout << "[INFO] Iteration history: " << iterationHistoryTable << endl;
    std::cout << "[INFO] Summary statistics: " << summaryStatsTable << endl;
	TNM_AppendTouchedTable(linkFlowTable);
	TNM_AppendTouchedTable(pathResultsTable);
	TNM_AppendTouchedTable(iterationHistoryTable);
	TNM_AppendTouchedTable(summaryStatsTable);

    // 1) 链路流量结果表
    {
        string checkSQL = schema.empty()? ("SELECT to_regclass('" + baseLinkFlowTable + "') IS NOT NULL") : ("SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_schema='" + schema + "' AND table_name='" + baseLinkFlowTable + "');");
        PGresult* checkRes = PQexec(conn, checkSQL.c_str());
        bool exists = false; if (PQresultStatus(checkRes) == PGRES_TUPLES_OK && PQntuples(checkRes) > 0) { string v = PQgetvalue(checkRes, 0, 0); exists = (v == "t"); } PQclear(checkRes);
        if (exists) {
            std::cout << "[INFO] Table " << linkFlowTable << " exists, clearing (DELETE)..." << endl;
            PGresult* r = PQexec(conn, ("DELETE FROM " + qLinkFlowTable + ";").c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to clear with DELETE: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        } else {
            std::cout << "[INFO] Table " << linkFlowTable << " does not exist, creating..." << endl;
            string createSQL = "CREATE TABLE " + qLinkFlowTable + " ("
                               "link_id integer NOT NULL, "
                               "from_node integer NOT NULL, "
                               "to_node integer NOT NULL, "
                               "flow numeric NOT NULL, "
                               "cost numeric NOT NULL, "
                               "travel_time numeric NOT NULL, "
                               "capacity numeric NOT NULL, "
                               "length numeric NOT NULL, "
                               "CONSTRAINT \"" + baseLinkFlowTable + "_pkey\" PRIMARY KEY (link_id)" ")";
            PGresult* r = PQexec(conn, createSQL.c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to create link flow results table: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        }
        {
            string s;
            s = "COMMENT ON TABLE " + qLinkFlowTable + " IS '" + w2u8(L"承载力算法路段分配结果表，包含路段分配流量、阻抗、行驶时间、容量、长度等。") + "'"; execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".link_id IS '" + w2u8(L"路段ID（对应输入网络拓扑表 link_id）") + "'";               execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".from_node IS '" + w2u8(L"起始节点ID") + "'";                                       execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".to_node IS '" + w2u8(L"终止节点ID") + "'";                                         execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".flow IS '" + w2u8(L"路段流量（veh/h）") + "'";                                      execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".cost IS '" + w2u8(L"路段阻抗") + "'";                                              execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".travel_time IS '" + w2u8(L"行驶时间") + "'";                                      execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".capacity IS '" + w2u8(L"路段容量") + "'";                                           execComment(s);
            s = "COMMENT ON COLUMN " + qLinkFlowTable + ".length IS '" + w2u8(L"路段长度") + "'";                                             execComment(s);
        }
    }

    // 2) 路径结果表
    {
        string checkSQL = schema.empty()? ("SELECT to_regclass('" + basePathResultsTable + "') IS NOT NULL") : ("SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_schema='" + schema + "' AND table_name='" + basePathResultsTable + "');");
        PGresult* checkRes = PQexec(conn, checkSQL.c_str()); bool exists = false; if (PQresultStatus(checkRes) == PGRES_TUPLES_OK && PQntuples(checkRes) > 0) { string v = PQgetvalue(checkRes, 0, 0); exists = (v == "t"); } PQclear(checkRes);
        if (exists) {
            std::cout << "[INFO] Table " << pathResultsTable << " exists, clearing (DELETE)..." << endl; PGresult* r = PQexec(conn, ("DELETE FROM " + qPathResultsTable + ";").c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to clear with DELETE: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        } else {
            std::cout << "[INFO] Table " << pathResultsTable << " does not exist, creating..." << endl; string createSQL = "CREATE TABLE " + qPathResultsTable + " ("
                               "origin_id integer NOT NULL, "
                               "dest_id integer NOT NULL, "
                               "path_id integer NOT NULL, "
                               "path_flow numeric NOT NULL, "
                               "path_cost numeric NOT NULL, "
                               "path_links text NOT NULL, "
                               "CONSTRAINT \"" + basePathResultsTable + "_pkey\" PRIMARY KEY (origin_id, dest_id, path_id)" ")";
            PGresult* r = PQexec(conn, createSQL.c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to create path results table: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        }
        {
            string s;
            s = "COMMENT ON TABLE " + qPathResultsTable + " IS '" + w2u8(L"承载力算法路径分配结果表，包含路径流量、路径成本与路径路段序列等。") + "'"; execComment(s);
            s = "COMMENT ON COLUMN " + qPathResultsTable + ".origin_id IS '" + w2u8(L"起始点ID") + "'";                                    execComment(s);
            s = "COMMENT ON COLUMN " + qPathResultsTable + ".dest_id IS '" + w2u8(L"目的地ID") + "'";                                       execComment(s);
            s = "COMMENT ON COLUMN " + qPathResultsTable + ".path_id IS '" + w2u8(L"路径ID") + "'";                                         execComment(s);
            s = "COMMENT ON COLUMN " + qPathResultsTable + ".path_flow IS '" + w2u8(L"路径流量（veh/h）") + "'";                             execComment(s);
            s = "COMMENT ON COLUMN " + qPathResultsTable + ".path_cost IS '" + w2u8(L"路径成本") + "'";                                     execComment(s);
            s = "COMMENT ON COLUMN " + qPathResultsTable + ".path_links IS '" + w2u8(L"路径包含的路段ID序列（逗号分隔）") + "'";                 execComment(s);
        }
    }

    // 3) 迭代历史表
    {
        string checkSQL = schema.empty()? ("SELECT to_regclass('" + baseIterationHistoryTable + "') IS NOT NULL") : ("SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_schema='" + schema + "' AND table_name='" + baseIterationHistoryTable + "');");
        PGresult* checkRes = PQexec(conn, checkSQL.c_str()); bool exists = false; if (PQresultStatus(checkRes) == PGRES_TUPLES_OK && PQntuples(checkRes) > 0) { string v = PQgetvalue(checkRes, 0, 0); exists = (v == "t"); } PQclear(checkRes);
        if (exists) {
            std::cout << "[INFO] Table " << iterationHistoryTable << " exists, clearing (DELETE)..." << endl; PGresult* r = PQexec(conn, ("DELETE FROM " + qIterationHistoryTable + ";").c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to clear with DELETE: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        } else {
            std::cout << "[INFO] Table " << iterationHistoryTable << " does not exist, creating..." << endl; string createSQL = "CREATE TABLE " + qIterationHistoryTable + " ("
                               "iteration integer NOT NULL, "
                               "objective_value numeric NOT NULL, "
                               "relative_gap numeric NOT NULL, "
                               "step_size numeric NOT NULL, "
                               "convergence_measure numeric NOT NULL, "
                               "execution_time numeric NOT NULL, "
                               "CONSTRAINT \"" + baseIterationHistoryTable + "_pkey\" PRIMARY KEY (iteration)" ")";
            PGresult* r = PQexec(conn, createSQL.c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to create iteration history table: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        }
        {
            string s;
            s = "COMMENT ON TABLE " + qIterationHistoryTable + " IS '" + w2u8(L"算法迭代过程记录表") + "'";                                execComment(s);
            s = "COMMENT ON COLUMN " + qIterationHistoryTable + ".iteration IS '" + w2u8(L"迭代次数") + "'";                                execComment(s);
            s = "COMMENT ON COLUMN " + qIterationHistoryTable + ".objective_value IS '" + w2u8(L"目标函数值") + "'";                         execComment(s);
            s = "COMMENT ON COLUMN " + qIterationHistoryTable + ".relative_gap IS '" + w2u8(L"相对间隙") + "'";                              execComment(s);
            s = "COMMENT ON COLUMN " + qIterationHistoryTable + ".step_size IS '" + w2u8(L"步长") + "'";                                    execComment(s);
            s = "COMMENT ON COLUMN " + qIterationHistoryTable + ".convergence_measure IS '" + w2u8(L"收敛度量") + "'";                      execComment(s);
            s = "COMMENT ON COLUMN " + qIterationHistoryTable + ".execution_time IS '" + w2u8(L"执行时间（秒）") + "'";                       execComment(s);
        }
    }

    // 4) 汇总统计表
    {
        string checkSQL = schema.empty()? ("SELECT to_regclass('" + baseSummaryStatsTable + "') IS NOT NULL") : ("SELECT EXISTS (SELECT FROM information_schema.tables WHERE table_schema='" + schema + "' AND table_name='" + baseSummaryStatsTable + "');");
        PGresult* checkRes = PQexec(conn, checkSQL.c_str()); bool exists = false; if (PQresultStatus(checkRes) == PGRES_TUPLES_OK && PQntuples(checkRes) > 0) { string v = PQgetvalue(checkRes, 0, 0); exists = (v == "t"); } PQclear(checkRes);
        if (exists) {
            std::cout << "[INFO] Table " << summaryStatsTable << " exists, clearing (DELETE)..." << endl; PGresult* r = PQexec(conn, ("DELETE FROM " + qSummaryStatsTable + ";").c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to clear with DELETE: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        } else {
            std::cout << "[INFO] Table " << summaryStatsTable << " does not exist, creating..." << endl; string createSQL = "CREATE TABLE " + qSummaryStatsTable + " ("
                               "total_iterations integer NOT NULL, "
                               "final_objective_value numeric NOT NULL, "
                               "final_relative_gap numeric NOT NULL, "
                               "total_execution_time numeric NOT NULL, "
                               "convergence_status text NOT NULL, "
                               "algorithm_type text NOT NULL, "
                               "CONSTRAINT \"" + baseSummaryStatsTable + "_pkey\" PRIMARY KEY (total_iterations)" ")";
            PGresult* r = PQexec(conn, createSQL.c_str()); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "[ERROR] Failed to create summary statistics table: " << PQerrorMessage(conn) << endl; PQclear(r); PQfinish(conn); return; } PQclear(r);
        }
        {
            string s;
            s = "COMMENT ON TABLE " + qSummaryStatsTable + " IS '" + w2u8(L"结果汇总统计表") + "'";                                         execComment(s);
            s = "COMMENT ON COLUMN " + qSummaryStatsTable + ".total_iterations IS '" + w2u8(L"总迭代次数") + "'";                             execComment(s);
            s = "COMMENT ON COLUMN " + qSummaryStatsTable + ".final_objective_value IS '" + w2u8(L"最终目标函数值") + "'";                    execComment(s);
            s = "COMMENT ON COLUMN " + qSummaryStatsTable + ".final_relative_gap IS '" + w2u8(L"最终相对间隙") + "'";                         execComment(s);
            s = "COMMENT ON COLUMN " + qSummaryStatsTable + ".total_execution_time IS '" + w2u8(L"总执行时间（秒）") + "'";                    execComment(s);
            s = "COMMENT ON COLUMN " + qSummaryStatsTable + ".convergence_status IS '" + w2u8(L"收敛状态（Converged/Max_Iterations）") + "'";  execComment(s);
            s = "COMMENT ON COLUMN " + qSummaryStatsTable + ".algorithm_type IS '" + w2u8(L"算法类型（TAP_Greedy 或 TAP_Greedy_dijk）") + "'"; execComment(s);
        }
    }
	
    std::cout << "[INFO] All output tables checked and prepared successfully!" << endl;

    // 写入链路流量结果（批量插入）
    std::cout << "[INFO] Writing link flow results to database..." << endl;
    {
        const int kLinkBatchSize = 1000;
        std::string insertPrefix = "INSERT INTO " + qLinkFlowTable +
            " (link_id, from_node, to_node, flow, cost, travel_time, capacity, length) VALUES ";
        std::string insertSql;
        int batchCount = 0;
        for (int i = 0; i < network->numOfLink; i++) {
            TNM_SLINK* link = network->linkVector[i];
            char value[256];
            sprintf(value, "(%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f)",
                    link->id, link->tail->id, link->head->id,
                    (double)link->volume, (double)link->cost, (double)link->cost,
                    (double)link->capacity, (double)link->length);
            if (batchCount == 0) {
                insertSql = insertPrefix;
                insertSql += value;
            } else {
                insertSql += ",";
                insertSql += value;
            }
            batchCount++;
            if (batchCount >= kLinkBatchSize) {
                insertSql += ";";
                PGresult* r = PQexec(conn, insertSql.c_str());
                if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                    std::cout << "[ERROR] Failed to insert batched link flow results: " << PQerrorMessage(conn) << endl;
                }
                if (r) PQclear(r);
                batchCount = 0;
                insertSql.clear();
            }
        }
        if (batchCount > 0) {
            insertSql += ";";
            PGresult* r = PQexec(conn, insertSql.c_str());
            if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                std::cout << "[ERROR] Failed to insert batched link flow results: " << PQerrorMessage(conn) << endl;
            }
            if (r) PQclear(r);
        }
    }

    // 写入路径结果（批量插入）
    std::cout << "[INFO] Writing path results to database..." << endl;
    {
        const int kPathBatchSize = 500;
        std::string insertPrefix = "INSERT INTO " + qPathResultsTable +
            " (origin_id, dest_id, path_id, path_flow, path_cost, path_links) VALUES ";
        std::string insertSql;
        int batchCount = 0;
        for (int oi = 0; oi < network->numOfOrigin; oi++) {
            TNM_SORIGIN* pOrg = network->originVector[oi];
            for (int di = 0; di < pOrg->numOfDest; di++) {
                TNM_SDEST* sdest = pOrg->destVector[di];
                if (sdest->pathSet.size() > 0) {
                    for (int pi = 0; pi < (int)sdest->pathSet.size(); pi++) {
                        TNM_SPATH* path = sdest->pathSet[pi];
                        if (path->flow > 1e-6) {
                            std::string pathStr;
                            for (int li = 0; li < (int)path->path.size(); li++) {
                                if (li > 0) pathStr += ",";
                                pathStr += to_string(path->path[li]->id);
                            }
                            int path_id_out = (path->id > 0) ? path->id : (pi + 1);
                            const std::string value = build_path_result_sql_value(
                                pOrg->origin->id,
                                sdest->dest->id,
                                path_id_out,
                                (double)path->flow,
                                (double)path->cost,
                                pathStr);
                            if (batchCount == 0) {
                                insertSql = insertPrefix;
                                insertSql += value;
                            } else {
                                insertSql += ",";
                                insertSql += value;
                            }
                            batchCount++;
                            if (batchCount >= kPathBatchSize) {
                                insertSql += ";";
                                PGresult* r = PQexec(conn, insertSql.c_str());
                                if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                                    std::cout << "[ERROR] Failed to insert batched path results: " << PQerrorMessage(conn) << endl;
                                }
                                if (r) PQclear(r);
                                batchCount = 0;
                                insertSql.clear();
                            }
                        }
                    }
                }
            }
        }
        if (batchCount > 0) {
            insertSql += ";";
            PGresult* r = PQexec(conn, insertSql.c_str());
            if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                std::cout << "[ERROR] Failed to insert batched path results: " << PQerrorMessage(conn) << endl;
            }
            if (r) PQclear(r);
        }
    }

    // 写入迭代历史（批量插入）
    std::cout << "[INFO] Writing iteration history to database..." << endl;
    if (iterRecord.size() > 0) {
        const int kIterBatchSize = 1000;
        std::string insertPrefix = "INSERT INTO " + qIterationHistoryTable +
            " (iteration, objective_value, relative_gap, step_size, convergence_measure, execution_time) VALUES ";
        std::string insertSql;
        int batchCount = 0;
        for (int i = 0; i < (int)iterRecord.size(); i++) {
            ITERELEM* iter = iterRecord[i];
            char value[256];
            sprintf(value,
                    "(%d,%.6f,%.6f,%.6f,%.6f,%.6f)",
                    iter->iter,
                    (double)iter->ofv,
                    (double)iter->conv,
                    (double)iter->step,
                    (double)iter->conv,
                    (double)iter->time);
            if (batchCount == 0) {
                insertSql = insertPrefix;
                insertSql += value;
            } else {
                insertSql += ",";
                insertSql += value;
            }
            batchCount++;
            if (batchCount >= kIterBatchSize) {
                insertSql += ";";
                PGresult* r = PQexec(conn, insertSql.c_str());
                if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                    std::cout << "Failed to insert batched iteration history: " << PQerrorMessage(conn) << endl;
                }
                if (r) PQclear(r);
                batchCount = 0;
                insertSql.clear();
            }
        }
        if (batchCount > 0) {
            insertSql += ";";
            PGresult* r = PQexec(conn, insertSql.c_str());
            if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                std::cout << "Failed to insert batched iteration history: " << PQerrorMessage(conn) << endl;
            }
            if (r) PQclear(r);
        }
    }

    // 写入汇总统计
    std::cout << "[INFO] Writing summary statistics to database..." << endl;
    {
        char sqlSummary[1024]; const char* convergenceStatus = (termFlag == ConvergeTerm) ? "Converged" : "Max_Iterations";
        sprintf(sqlSummary, "INSERT INTO %s (total_iterations, final_objective_value, final_relative_gap, total_execution_time, convergence_status, algorithm_type) VALUES (%d, %.6f, %.6f, %.6f, '%s', '%s');",
                qSummaryStatsTable.c_str(), curIter, (double)OFV, (double)convIndicator, (double)cpuTime, convergenceStatus, "TAP_Greedy");
        PGresult* r = PQexec(conn, sqlSummary); if (PQresultStatus(r) != PGRES_COMMAND_OK) { std::cout << "Failed to insert summary statistics: " << PQerrorMessage(conn) << endl; } PQclear(r);
    }

    // 更新网络拓扑表
    if (!networkTableName.empty()) {
        std::cout << "[INFO] Updating network topology table with traffic assignment results..." << endl;
        string checkColumnsQuery = netSchema.empty()? ("SELECT column_name, data_type FROM information_schema.columns WHERE table_schema = current_schema() AND table_name='" + netBaseName + "' AND column_name IN ('volume','v_c','bottleneck','volume_edit','saturation_edit','saturation_diff','link_flow_diff')") : ("SELECT column_name, data_type FROM information_schema.columns WHERE table_schema='" + netSchema + "' AND table_name='" + netBaseName + "' AND column_name IN ('volume','v_c','bottleneck','volume_edit','saturation_edit','saturation_diff','link_flow_diff')");
        PGresult* checkRes = PQexec(conn, checkColumnsQuery.c_str()); bool bottleneckIsBoolean = false;
        if (PQresultStatus(checkRes) == PGRES_TUPLES_OK) {
            int n = PQntuples(checkRes); 
            bool hasVolume=false, hasVC=false, hasBottleneck=false, hasVolumeEdit=false;
            bool hasSaturationEdit=false, hasSaturationDiff=false, hasLinkFlowDiff=false;
            for (int i=0;i<n;i++){ 
                string col=PQgetvalue(checkRes,i,0); 
                string typ=PQgetvalue(checkRes,i,1); 
                if(col=="volume") hasVolume=true; 
                else if(col=="v_c") hasVC=true; 
                else if(col=="bottleneck"){ hasBottleneck=true; bottleneckIsBoolean=(typ=="boolean"); } 
                else if(col=="volume_edit") { hasVolumeEdit=true; }
                else if(col=="saturation_edit") { hasSaturationEdit=true; }
                else if(col=="saturation_diff") { hasSaturationDiff=true; }
                else if(col=="link_flow_diff") { hasLinkFlowDiff=true; }
            }
            if (!hasVolume) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN volume DOUBLE PRECISION DEFAULT 0.0").c_str()); PQclear(r);} 
            if (!hasVC) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN v_c DOUBLE PRECISION DEFAULT 0.0").c_str()); PQclear(r);} 
            if (!hasBottleneck) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN bottleneck BOOLEAN DEFAULT FALSE").c_str()); PQclear(r); bottleneckIsBoolean=true; }
            else if (!bottleneckIsBoolean) { std::cout << "[INFO] Detected non-boolean bottleneck column (e.g., integer). Will keep original type and write 0/1 accordingly." << endl; }
            if (!hasVolumeEdit) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN volume_edit DOUBLE PRECISION").c_str()); PQclear(r);} 
            if (!hasSaturationEdit) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN saturation_edit DOUBLE PRECISION DEFAULT 0.0").c_str()); PQclear(r);} 
            if (!hasSaturationDiff) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN saturation_diff DOUBLE PRECISION DEFAULT 0.0").c_str()); PQclear(r);} 
            if (!hasLinkFlowDiff) { PGresult* r=PQexec(conn,("ALTER TABLE "+qQualifiedNetworkTable+" ADD COLUMN link_flow_diff DOUBLE PRECISION DEFAULT 0.0").c_str()); PQclear(r);} 
        }
        PQclear(checkRes);
        {
            string s;
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".volume IS '" + w2u8(L"路段流量") + "'";                                     execComment(s);
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".v_c IS '" + w2u8(L"饱和度(V/C)") + "'";                                         execComment(s);
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".bottleneck IS '" + w2u8(L"瓶颈标识") + "'";         execComment(s);
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".volume_edit IS '" + w2u8(L"预设流量") + "'";               execComment(s);
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".saturation_edit IS '" + w2u8(L"预设饱和度") + "'";            execComment(s);
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".saturation_diff IS '" + w2u8(L"饱和度差值") + "'";            execComment(s);
            s = "COMMENT ON COLUMN " + qQualifiedNetworkTable + ".link_flow_diff IS '" + w2u8(L"流量差值") + "'";                execComment(s);
        }

        // 基于 link 流量结果表的批量更新：将分配结果同步回网络拓扑表
        {
            std::string updateSql;
            if (bottleneckIsBoolean) {
                updateSql =
                    "UPDATE " + qQualifiedNetworkTable + " AS nt SET "
                    "volume = lf.flow, "
                    "volume_edit = lf.flow, "
                    "v_c = CASE WHEN lf.capacity > 0 THEN lf.flow / lf.capacity ELSE 0.0 END, "
                    "bottleneck = CASE WHEN lf.capacity > 0 AND lf.flow / lf.capacity > 0.9 THEN true ELSE false END, "
                    "saturation_edit = CASE WHEN lf.capacity > 0 THEN lf.flow / lf.capacity ELSE 0.0 END, "
                    "saturation_diff = 0.0, "
                    "link_flow_diff = 0.0 "
                    "FROM " + qLinkFlowTable + " AS lf "
                    "WHERE nt.link_id = lf.link_id;";
            } else {
                updateSql =
                    "UPDATE " + qQualifiedNetworkTable + " AS nt SET "
                    "volume = lf.flow, "
                    "volume_edit = lf.flow, "
                    "v_c = CASE WHEN lf.capacity > 0 THEN lf.flow / lf.capacity ELSE 0.0 END, "
                    "bottleneck = CASE WHEN lf.capacity > 0 AND lf.flow / lf.capacity > 0.9 THEN 1 ELSE 0 END, "
                    "saturation_edit = CASE WHEN lf.capacity > 0 THEN lf.flow / lf.capacity ELSE 0.0 END, "
                    "saturation_diff = 0.0, "
                    "link_flow_diff = 0.0 "
                    "FROM " + qLinkFlowTable + " AS lf "
                    "WHERE nt.link_id = lf.link_id;";
            }
            PGresult* r = PQexec(conn, updateSql.c_str());
            if (PQresultStatus(r) != PGRES_COMMAND_OK) {
                std::cout << "Failed to bulk update network topology table: " << PQerrorMessage(conn) << endl;
            }
            if (r) PQclear(r);
        }
        {
            std::string initEditSql = std::string("UPDATE ") + qQualifiedNetworkTable + " SET volume_edit = volume WHERE volume_edit IS NULL";
            PGresult* r=PQexec(conn, initEditSql.c_str()); if (r) PQclear(r);
        }
        std::cout << "[INFO] Successfully updated network topology table with traffic assignment results!" << endl;
    }

    // 导出报告（略）（保留现有实现）
    {
        std::cout << "[INFO] Exporting algorithm report files (CSV/JSON) to current directory..." << endl;
        string scenarioId = rawScenario.empty()? string("scenario"): rawScenario; string scenarioIdEscaped; scenarioIdEscaped.reserve(scenarioId.size()*2); for(size_t i=0;i<scenarioId.size();++i){ char c=scenarioId[i]; if(c=='\'') scenarioIdEscaped+="''"; else scenarioIdEscaped.push_back(c);} 
		// Save files to current working directory
		string histogramCsv = scenarioId + "_saturation_histogram.csv";
		string top20Csv = scenarioId + "_top20_congested_links.csv";
		string summaryJson = scenarioId + "_saturation_summary.json";

		std::string __dllDir = __GetDllDirA();
		if (!__dllDir.empty()) {
#ifdef _WIN32
			histogramCsv = __dllDir + "\\" + histogramCsv;
			top20Csv    = __dllDir + "\\" + top20Csv;
			summaryJson = __dllDir + "\\" + summaryJson;
#else
			histogramCsv = __dllDir + "/" + histogramCsv;
			top20Csv    = __dllDir + "/" + top20Csv;
			summaryJson = __dllDir + "/" + summaryJson;
#endif
		}

        // 直方图 CSV
        {
            string sql = string("COPY (\n") +
                "WITH base AS (\n"
                "  SELECT (CASE WHEN capacity > 0 THEN (flow / capacity)::double precision ELSE NULL END) AS vc\n"
                "  FROM " + qLinkFlowTable + "\n"
                "  WHERE capacity > 0\n"
                "),\n"
                "binned AS (\n"
                "  SELECT\n"
                "    CASE\n"
                "      WHEN vc < 0.6 THEN '[0,0.6)'\n"
                "      WHEN vc < 0.8 THEN '[0.6,0.8)'\n"
                "      WHEN vc < 0.9 THEN '[0.8,0.9)'\n"
                "      WHEN vc < 1.0 THEN '[0.9,1.0)'\n"
                "      WHEN vc < 1.1 THEN '[1.0,1.1)'\n"
                "      ELSE '[1.1,+)'\n"
                "    END AS bin_label,\n"
                "    CASE\n"
                "      WHEN vc < 0.6 THEN 0.0\n"
                "      WHEN vc < 0.8 THEN 0.6\n"
                "      WHEN vc < 0.9 THEN 0.8\n"
                "      WHEN vc < 1.0 THEN 0.9\n"
                "      WHEN vc < 1.1 THEN 1.0\n"
                "      ELSE 1.1\n"
                "    END AS bin_left,\n"
                "    CASE\n"
                "      WHEN vc < 0.6 THEN 0.6\n"
                "      WHEN vc < 0.8 THEN 0.8\n"
                "      WHEN vc < 0.9 THEN 0.9\n"
                "      WHEN vc < 1.0 THEN 1.0\n"
                "      WHEN vc < 1.1 THEN 1.1\n"
                "      ELSE 999.0\n"
                "    END AS bin_right\n"
                "  FROM base\n"
                ")\n"
                "SELECT\n"
                "  '" + scenarioIdEscaped + "'::text AS scenario_id,\n"
                "  bin_label, bin_left, bin_right,\n"
                "  count(*) AS count,\n"
                "  count(*)::float / sum(count(*)) OVER () AS ratio\n"
                "FROM binned\n"
                "GROUP BY bin_label, bin_left, bin_right\n"
                "ORDER BY bin_left\n"
                ") TO STDOUT WITH (FORMAT csv, HEADER true)";
            PGresult* res = PQexec(conn, sql.c_str()); if (PQresultStatus(res)!=PGRES_COPY_OUT){ std::cout << "Failed to start COPY OUT for histogram CSV: " << PQerrorMessage(conn) << endl; PQclear(res);} else { PQclear(res); ofstream out(histogramCsv.c_str(), ios::binary); if(!out.is_open()){ std::cout << "Failed to open file for histogram CSV: " << histogramCsv << endl; char* buf=NULL; int bytes=0; while((bytes=PQgetCopyData(conn,&buf,0))>0){ PQfreemem(buf);} PGresult* endRes=NULL; while((endRes=PQgetResult(conn))!=NULL){ PQclear(endRes);} } else { char* buf=NULL; int bytes=0; while((bytes=PQgetCopyData(conn,&buf,0))>0){ out.write(buf,bytes); PQfreemem(buf);} out.close(); if(bytes==-2){ std::cout << "COPY OUT error for histogram CSV: " << PQerrorMessage(conn) << endl;} PGresult* endRes=NULL; while((endRes=PQgetResult(conn))!=NULL){ PQclear(endRes);} std::cout << "Histogram CSV exported to: " << histogramCsv << endl; } }
        }

        // Top-20 CSV
        {
            string sql;
            if (!networkTableName.empty()) {
                sql = string("COPY (\n") +
                      "WITH t AS (\n"
                      "  SELECT link_id, from_node, to_node, capacity, flow AS volume,\n"
                      "         CASE WHEN capacity > 0 THEN flow/capacity ELSE NULL END AS v_c\n"
                      "  FROM " + qLinkFlowTable + "\n"
                      "  WHERE capacity > 0\n"
                      ")\n"
                      "SELECT\n"
                      "  '" + scenarioIdEscaped + "'::text AS scenario_id,\n"
                      "  row_number() OVER (ORDER BY t.v_c DESC, t.volume DESC) AS rank,\n"
                      "  t.link_id, t.from_node AS tail, t.to_node AS head, t.capacity, t.volume, t.v_c,\n"
                      "  ((t.v_c >= 1.0) OR (nt.bottleneck::text IN ('t','true','1'))) AS is_bottleneck,\n"
                      "  CASE WHEN nt.bottleneck IS NULL THEN (CASE WHEN t.v_c >= 1.0 THEN 1 ELSE 0 END)\n"
                      "       WHEN nt.bottleneck::text IN ('t','true','1') THEN 1 ELSE 0 END AS bottleneck\n"
                      "FROM t LEFT JOIN " + qQualifiedNetworkTable + " nt ON nt.link_id = t.link_id\n"
                      "ORDER BY t.v_c DESC, t.volume DESC\n"
                      "LIMIT 20\n"
                      ") TO STDOUT WITH (FORMAT csv, HEADER true)";
            } else {
                sql = string("COPY (\n") +
                      "SELECT\n"
                      "  '" + scenarioIdEscaped + "'::text AS scenario_id,\n"
                      "  row_number() OVER (ORDER BY v_c DESC, volume DESC) AS rank,\n"
                      "  link_id, from_node AS tail, to_node AS head, capacity, volume, v_c,\n"
                      "  (CASE WHEN v_c >= 1.0 THEN true ELSE false END) AS is_bottleneck,\n"
                      "  (CASE WHEN v_c >= 1.0 THEN 1 ELSE 0 END) AS bottleneck\n"
                      "FROM (\n"
                      "  SELECT link_id, from_node, to_node, capacity, flow AS volume,\n"
                      "         CASE WHEN capacity > 0 THEN flow / capacity ELSE NULL END AS v_c\n"
                      "  FROM " + qLinkFlowTable + "\n"
                      "  WHERE capacity > 0\n"
                      ") t\n"
                      "ORDER BY v_c DESC, volume DESC\n"
                      "LIMIT 20\n"
                      ") TO STDOUT WITH (FORMAT csv, HEADER true)";
            }
            PGresult* res = PQexec(conn, sql.c_str()); if(PQresultStatus(res)!=PGRES_COPY_OUT){ std::cout << "[ERROR] Failed to start COPY OUT for top-20 congested CSV: " << PQerrorMessage(conn) << endl; PQclear(res);} else { PQclear(res); ofstream out(top20Csv.c_str(), ios::binary); if(!out.is_open()){ std::cout << "Failed to open file for top-20 CSV: " << top20Csv << endl; char* buf=NULL; int bytes=0; while((bytes=PQgetCopyData(conn,&buf,0))>0){ PQfreemem(buf);} PGresult* endRes=NULL; while((endRes=PQgetResult(conn))!=NULL){ PQclear(endRes);} } else { char* buf=NULL; int bytes=0; while((bytes=PQgetCopyData(conn,&buf,0))>0){ out.write(buf,bytes); PQfreemem(buf);} out.close(); if(bytes==-2){ std::cout << "COPY OUT error for top-20 CSV: " << PQerrorMessage(conn) << endl;} PGresult* endRes=NULL; while((endRes=PQgetResult(conn))!=NULL){ PQclear(endRes);} std::cout << "[INFO] Top-20 congested links CSV exported to: " << top20Csv << endl; } }
        }

        // 汇总 JSON（单行）
        {
            string sql = string("COPY (\n") +
                "WITH base AS (\n"
                "  SELECT (CASE WHEN capacity > 0 THEN (flow / capacity)::double precision ELSE NULL END) AS vc\n"
                "  FROM " + qLinkFlowTable + "\n"
                "  WHERE capacity > 0\n"
                "),\n"
                "tot AS (SELECT count(*) AS cnt FROM base),\n"
                "stats AS (\n"
                "  SELECT avg(vc) AS mean,\n"
                "         percentile_cont(0.5) WITHIN GROUP (ORDER BY vc) AS median,\n"
                "         percentile_cont(0.9) WITHIN GROUP (ORDER BY vc) AS p90,\n"
                "         percentile_cont(0.95) WITHIN GROUP (ORDER BY vc) AS p95\n"
                "  FROM base\n"
                "),\n"
                "thr AS (\n"
                "  SELECT sum(CASE WHEN vc > 0.8 THEN 1 ELSE 0 END) AS gt_0_8_cnt,\n"
                "         sum(CASE WHEN vc > 0.9 THEN 1 ELSE 0 END) AS gt_0_9_cnt\n"
                "  FROM base\n"
                ")\n"
                "SELECT json_build_object(\n"
                "  'scenario_id', '" + scenarioIdEscaped + "',\n"
                "  'spec_version', '1.0.0',\n"
                "  'aggregation', json_build_object('weighting','unweighted','included_links', tot.cnt),\n"
                "  'vc_stats', json_build_object('mean', stats.mean, 'median', stats.median, 'p90', stats.p90, 'p95', stats.p95),\n"
                "  'thresholds', json_build_object(\n"
                "       'gt_0_8', json_build_object('count', thr.gt_0_8_cnt, 'ratio', CASE WHEN tot.cnt > 0 THEN thr.gt_0_8_cnt::float/tot.cnt ELSE 0 END),\n"
                "       'gt_0_9', json_build_object('count', thr.gt_0_9_cnt, 'ratio', CASE WHEN tot.cnt > 0 THEN thr.gt_0_9_cnt::float/tot.cnt ELSE 0 END)\n"
                "  ),\n"
                "  'bins', json_build_object('edges', array[0.0,0.6,0.8,0.9,1.0,1.1,999.0], 'labels', array['[0,0.6)','[0.6,0.8)','[0.8,0.9)','[0.9,1.0)','[1.0,1.1)','[1.1,+)']),\n"
                "  'generated_at', now()\n"
                ")::text\n"
                "FROM tot, stats, thr\n"
                ") TO STDOUT WITH (FORMAT text)";
            PGresult* res = PQexec(conn, sql.c_str()); if(PQresultStatus(res)!=PGRES_COPY_OUT){ std::cout << "Failed to start COPY OUT for summary JSON: " << PQerrorMessage(conn) << endl; PQclear(res);} else { PQclear(res); ofstream out(summaryJson.c_str(), ios::binary); if(!out.is_open()){ std::cout << "Failed to open file for summary JSON: " << summaryJson << endl; char* buf=NULL; int bytes=0; while((bytes=PQgetCopyData(conn,&buf,0))>0){ PQfreemem(buf);} PGresult* endRes=NULL; while((endRes=PQgetResult(conn))!=NULL){ PQclear(endRes);} } else { char* buf=NULL; int bytes=0; while((bytes=PQgetCopyData(conn,&buf,0))>0){ out.write(buf,bytes); PQfreemem(buf);} out.close(); if(bytes==-2){ std::cout << "COPY OUT error for summary JSON: " << PQerrorMessage(conn) << endl;} PGresult* endRes=NULL; while((endRes=PQgetResult(conn))!=NULL){ PQclear(endRes);} std::cout << "[INFO] Summary JSON exported to: " << summaryJson << endl; } }
        }
    }

    std::cout << "[INFO] Successfully wrote all Greedy results to PostgreSQL database!" << endl;
    PQfinish(conn);
}