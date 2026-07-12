//#pragma once
#include "head.h"
//#include "TNM_utility.h"
#include "My_Predicate.h"
//#include "nlopt.h"
//#include "lp_lib.h"

//#include "..\trafficNet\stdafx.h"
#include "TNM_utility.h"
//#include "..\..\..\include\tnm\TNM_utility.cpp"

//
//#pragma comment(lib, "liblpsolve55d")
//#pragma comment(lib, "liblpsolve55")
//#pragma comment(lib, "lpsolve55")


class PTNet_API PTRoute
{
public:
	PTRoute()
    {
		m_patternid = "unknown";
        m_id = "unknown";
        m_sname = "0";
        m_lname = "unknown";
        m_type  = -1;
    };
	string m_id;
	string m_sname;
	string m_lname;
	string m_patternid;
	int m_type;
};

class PTNode;
class PTLink;
class PTNET;
class GNODE;
class GLINK;
class PTShape;
class PTOrg;
class PTDestination;
typedef map<string, PTNode*, less<string> > PTNodeMap;//key is the ID of the route that node is associated with. 
typedef map<string, PTNode*, less<string> >::iterator PTNodeMapIter;
typedef multimap<long, int, std::less<long> > COORDMAP;
typedef COORDMAP::iterator COORDMAP_I; //iterator
struct  TPHYPERPATHELEM;
typedef map<string,TPHYPERPATHELEM*> LabelsMap;
typedef map<string,TPHYPERPATHELEM*>::iterator LabelsMapIter;
struct Treenode
{
	int val;//is leaf
	floatType prob;
	GNODE* gnode;
    Treenode* parent;
    vector<Treenode*> children;
	Treenode(int v,GNODE* gn): val(v),gnode(gn),parent(nullptr),prob(0.0) {}
};




class PTNet_API PTStop
{
public:
	PTStop(){;};

	bool		TAPInitialize(const string &inf,bool isPos = false);
	PTNode*		GetTransferNode() {return m_transferNode;};
	double		GetLat(){return m_pos.GetLatitude();};
    double		GetLon(){return m_pos.GetLongitude();};
	void        SetTransferNode(PTNode* nd) {m_transferNode = nd;}
	void		SetTransferNodes(PTNode* nd) {m_transferNodes.push_back(nd);}
	PTNode*     GetTransferNodeTP(int index);
	bool		AddNode(const string &shapeid, PTNode* node)
	{
		if(!node) return false;
		else
		{
			pair<PTNodeMapIter, bool> ret = m_nodes.insert(PTNodeMap::value_type(shapeid, node));
			return ret.second;
		}
	
	};
	PTNode*     GetNode(const string &shapeid)
	{
		PTNodeMapIter pi = m_nodes.find(shapeid);
		if(pi == m_nodes.end()) return NULL;
		else return pi->second;	
	};
	
	

	PTNode*		m_transferNode;//this node is initialized after CreateTransferNode. 
	vector<PTNode*> m_transferNodes;
	PTNodeMap	m_nodes; //all nodes belong to this stop 
	string		m_id;
	string		m_name;
	TNM_Position m_pos;
	vector<PTShape*> mshapes;
};


class PTNet_API PTShape
{
public: 
	PTShape()
	{
		m_id = "-1";
		m_routePtr = NULL;
	};

	void   SetRoute(PTRoute* rt) {m_routePtr = rt;}
	bool   AddStop(PTStop* stop)
	{
		if(!stop) return false;
		else
		{
			m_stops.push_back(stop);
			return true;
		}
	}
	PTRoute*        m_routePtr;
	vector<PTStop*> m_stops;//线路中的站点序列
	vector<PTLink*> m_boardlinks;//creater for capacitated assignment
	vector<PTLink*> m_alightlinks;
	string			m_id; 
	long double			m_cap;//capacity of this line
	double			m_freq;//frequency of this line

};




struct PTNet_API StgLinks // strategic links, not create new glink on this strategy, save creating time for glink,
{
	StgLinks(string name, floatType w, vector<int> ls,vector<floatType> lp, floatType c)
	{
		sname = name;
		waitT = w;
		StgLinkPosVec = ls;
		StgLinkProbVec = lp;
		cost = c;
		buffer = NULL;
		isboarding = false;
	}

	StgLinks()
	{
		approach=	0.0;
		sflow	=	0.0;
		waitT	=	0.0;
		cost	=	0.0;
		sname	=	"";
		buffer = NULL;
		isboarding = false;
	}

	StgLinks(int buffersize)
	{
		approach=	0.0;
		sflow	=	0.0;
		waitT	=	0.0;
		cost	=	0.0;
		sname	=	"";
		isboarding = false;
		buffer = new floatType[buffersize];
		for (int j = 0;j<buffersize;j++)	buffer[j] = 0.0;
	}

	void SetLinkProbVec(vector<floatType> v)
	{
		StgLinkProbVec = v;
		numofstglinks = v.size();
	}

	floatType				*buffer;    //this data is added as a working space for outside using.
	floatType				cost;
	string					sname;
	int						numofstglinks;
	bool					isboarding;
	floatType				waitT;//waiting time of strategy
	floatType				sflow;//strategy flow
	floatType				approach;//the ratio of the flow
	vector<int>				StgLinkPosVec;// just save the link position in linkvector
	vector<floatType>		StgLinkProbVec;// approach prob
	void  print()
	{
		cout<<"stg:"<<sname<<", flow:"<<sflow<<", wait delay="<<waitT<<endl;
	}
};

/*
struct PTNet_API StgLinks // strategic links
{
	StgLinks()
	{	
		approach=	0.0;
		sflow	=	0.0;
		waitT	=	0.0;
		cost	=	0.0;
		sname	=	"";
		StgLinkVia = NULL;
	};
	
	floatType				approach;//the ratio of the flow
	floatType				sflow;//strategy flow
	floatType				waitT;//waiting time of strategy
	string					sname;
	floatType				cost;
	GLINK*					StgLinkVia;

	void  print()
	{
		cout<<"stg:"<<sname<<", flow:"<<sflow<<", wait delay="<<waitT<<endl;
	}
};
*/

typedef vector<PTLink*>::iterator PTRTRACE;

struct PTNet_API HYPERPATHELEM
{
	HYPERPATHELEM()
	{
		 cost = 0.0;
		 transfers = -1;
		// state = -1;
		 //sIndex = -1;
		 dist = 0.0;
		 via = NULL;
		 walkcost = 0.0;
		 vialink = NULL;
		 scanstatus=0;
		 //travelStrategy.clear();
	}
	floatType				cost;
	floatType				walkcost;
	StgLinks*				via; // this is created for built strategy info
	PTLink*					vialink;// this is created for link pointer on shortest hyperapth, not necessarily create a strategy
	int                     transfers; // this variable represents the number of modal transfers from current node to destination.
	//int                     state; // represents the a combination of viable path.
	//int                     sIndex; // the index of  state.
	double                  dist;//in km
	int						scanstatus;
	floatType				tempcost;
	//floatType				walkcost;
	PTLink*					tempvialink;
	//vector<MODETYPE>        travelStrategy; //Store the sequence of used mode.
	void                    Reset()
	{
		cost = POS_INF_FLOAT;
		transfers = -1;
		//state = -1;
		//sIndex = -1;
		via = NULL;
		vialink = NULL; 
	}
};


class PTNet_API PTNode
{
public:
	PTNode()
	{
		id = -1;
		trafficnetid = -1;
		xCord = 0;
		yCord = 0;
		scanStatus = 0;
		tmpNumOfIn = 0;
		m_tmpdata = 0.0;
		m_wait = 0.0;//this is create for storing waiting delay at transfernodes
		StgElem   = new HYPERPATHELEM;
		rStgElem  = new HYPERPATHELEM;
		StgElem_next = new HYPERPATHELEM;
		//eStgElem  = new HYPERPATHELEM;
		m_stgNode = NULL;
		transfer = 0;
		walkt = 0;
		m_labels = NULL;
		m_type = 0;
		hyperpath_times = 0;

		buffer = NULL;		
	};
	LabelsMap* m_labels;
	TPHYPERPATHELEM*   GetMinLabeForFindSHP(int curTrans,floatType walklimit);
	TPHYPERPATHELEM*   GetMinLabeForFindSHPTP(int curTrans);
	TPHYPERPATHELEM*   GetMaxLabelOnState(int curTrans,floatType walklimit);
	TPHYPERPATHELEM*   GetMinAcyclicLabel();
	TPHYPERPATHELEM*   GetLabel(const string &ts);
	int				   InsertLabel(TPHYPERPATHELEM* label);
	TPHYPERPATHELEM*   GetMinLabel();
	void               PrintNodeMapLables();
	TPHYPERPATHELEM*   GetMinLabelOnTransfers(int k,floatType walklimit);
	int                m_type;//0.board  1.alight
	//void			   setType(int type);

	floatType walkt;
	int swapstate;
	enum TNTYPE {TRANSFER, ENROUTE, DWELL, CENTROID};
	int	NumOfDestOutLink();
	int	NumOfBushOutLink();

	void		 CleanStgLinksOnHyperPath();

	void         CleanStgLinks();
	void		 SetTransitNodeType(TNTYPE t = ENROUTE) {m_tnType = t;}
	TNTYPE		 GetTransitNodeType() {return m_tnType;}

	void		 SetStopPtr(PTStop *st) {m_stop = st;}
	void         setType(int type) {m_type=type;}
	PTStop*		 GetStopPtr() {return m_stop;}
	void		 SetShapePtr(PTShape *sp) {m_shape = sp;}
	void		 SetRoutePtr(PTRoute *sh) {m_route = sh;}
	double		 MeasureDist(PTNode *node)
	{
		double tx, ty;
		tx = node->xCord - xCord;
		ty = node->yCord - yCord;
		return sqrt(tx * tx + ty * ty);
	}
	inline int			id_() {return id;} 
	inline int			trafficid_() {return trafficnetid;} 

	int				id;
	int				transfer;
	int				trafficnetid; // traffic net id
	int             tmpNumOfIn; //Used in topological computation to store the incoming links
	vector<PTLink*> forwStar;   //All outgoing link pointers
	vector<PTLink*> backStar;   //All incoming link pointers
	PTShape*		m_shape;
	PTRoute*		m_route;
	PTStop*			m_stop;
	floatType		xCord;
	floatType		yCord;
	floatType		m_tmpdata;
	floatType		m_wait; //站点对应的等待时间
	vector<floatType> m_attProb;
	int				scanStatus;
	floatType		*buffer;  //this is a solution variable intended for temporary use. 
	TNTYPE			m_tnType; //transfer or not;
	GNODE			*m_stgNode;
	
	HYPERPATHELEM           *StgElem;  //Store the optimal routing policy.
	HYPERPATHELEM           *StgElem_next;
	HYPERPATHELEM           *rStgElem;  //Store the optimal routing policy.
	//HYPERPATHELEM           *eStgElem;  //Store the efficient routing policy.
	//StgLinks*				m_StgVia;//shortest tree topology
	//StgLinks*				m_rStgVia;//longest tree topology
	//floatType				m_cost;// shortest strategy cost from the tail node of the stg to destination
	//floatType				m_rcost;// longest strategy cost from the tail node of the stg to destination

	int				hyperpath_times;
};



class PTNet_API PTLink
{
public:
	PTLink(int i, PTNode* t, PTNode* h)
	{
		id = i;
		tail = t;
		head = h;
		tail->forwStar.push_back(this);
		fID = tail->forwStar.size() - 1;
		head->backStar.push_back(this);
		volume = 0.0;
		cost = 0.0;
		rLink = NULL;
		stglinkptr = NULL;
		pfdcost = 0;
		rpfdcost = 0.0;
		length = 0;
		fft = 0;
		buffer = NULL;
		markStatus = 0;
		m_probdata = 0;
		effpower = 0.2;
		efffreq = POS_INF_FLOAT;
		freq = POS_INF_FLOAT;
		m_stglink = NULL;
		hyperpath_times = 0;
		cap_cost = 0;
		related_OD = NULL;
		OD_walklink = 0;
		RATIO = 0.0;
		tiny_value = 0.00001;
	};
	enum TLTYPE {ALIGHT, ABOARD, ENROUTE, DWELL, WALK, FAILWALK,TRANSFER}; // fail walking is created for 
	enum TLSYM {SYMMTRIC,ASYMMTRIC};//SYMMTRIC对称；ASYMMTRIC不对称
	enum CAPTYPE{TL, BL};


	void		SetTransitLinkType(TLTYPE t = ENROUTE) {m_tlType = t;};
	void		SetTransitLinkSym(TLSYM t) {m_tlSym = t;};
	void     SetCapType(CAPTYPE t){m_tlCap=t;};

	TLTYPE		GetTransitLinkType() {return m_tlType;};
	string		GetTransitLinkTypeName() 
	{
		switch(m_tlType)
		{
			case TLTYPE::ABOARD:
				return "ABOARD";
				break;
			case TLTYPE::ALIGHT:
				return "ALIGHT";
				break;
			case TLTYPE::ENROUTE:
				return "ENROUTE";
				break;
			case TLTYPE::DWELL:
				return "DWELL";
				break;
			case TLTYPE::WALK:
				return "WALK";
				break;		
		}
		return "UNKNOWN";
	}
	TLSYM		GetTransitLinkSymmetry() {return m_tlSym;};
	CAPTYPE		GetTransitLinkCapType() {return m_tlCap;};
	void		UpdatePTLinkCost();
	void		UpdatePTLinkCost_const();//将link_cost更新为常数
	void		UpdatePTLinkCost_const_TEST();//将link_cost更新为常数
	void		UpdatePTDerLinkCost();
	void		UpdatePTDerLinkCost_const();//将link_dercost更新为常数(0.0)
    floatType	GetPTLinkCost();
	floatType	GetPTLinkDerCost();
	inline int			id_() {return id;} 
	
	// add for effective frequency-based TEAP
	void				UpdateEffectiveFreq();
	void				UpdateEffectiveFreq_TEST();

	floatType			PREVOLUME;//
	int					OD_walklink;
	int					id;
	int					fID;// this stored the order of forward links at the tail node
	int					seq;// it indicate the sequence number of a line, for a boarding link
	PTShape*			m_shape;
	floatType			temptemp;//
	floatType			temp_temp;//
	floatType			old_volume;//
	PTNode*				tail;
	PTNode*				head;
	floatType			tmpuse; // for temp use（共线问题求解中使用）
	floatType           m_probdata;// for storing link probability, for temorporary storage on shortest hyperapath tree
	floatType           m_hwmean; // headway value
	vector<floatType>	pars;// parameters to determine link cost
	floatType			pfdcost;//first derivative link cost respective to itself
	floatType			rpfdcost;//first derivative link cost respective to the related link
	floatType			cost;//link cost
	floatType           pre_transit_link_cost;
	floatType           pre_boarding_link_cost;
	floatType           tiny_value;
	floatType           pre_walk_link_cost;
	floatType		    RATIO;
	floatType			volume;//link flow
	floatType			temp_volume;//link flow
	floatType			length;//km
	floatType			fft;//free flow time
	floatType			*buffer;  //this is a solution variable intended for temporary use. 
	tinyInt				markStatus;
	floatType			SCvolume;//simplex combination link flow, only called in SD
	PTLink*				rLink;//cost related link pointer
	PTLink*				stglinkptr;   //pointer toward the link on the strategy
	TLTYPE				m_tlType; //transit link type
	TLSYM				m_tlSym; //indicate whether link cost is separable 
	CAPTYPE				m_tlCap; //indicate the capacitated link type

	GLINK*				m_stglink;

	//========capacity-related  attribute========
	floatType			g_cost;	//generalized cost
	floatType			g_pfdcost;//first generalized derivative link cost respective to itself
	floatType			g_rpfdcost;//first generalized derivative link cost respective to the related link
	floatType			cap;	//capacity
	floatType			mu;		//multiplier
	floatType			z;		//slack variable
	floatType			cap_cost; //利用容量计算得出的费用（用于capacited problem）
	

	floatType			multiplier;//这里用作dual cutting-plane algorithm记录link相关的乘子信息；
	vector<floatType>   Iter_volumeset;//这里用作dual cutting-plane algorithm记录迭代流量信息
	vector<floatType>   Iter_transitvolumeset;//这里用作dual cutting-plane algorithm记录迭代流量信息
	vector<vector<floatType>>   Vec_volumeset;//用于记录link每次迭代对应的volumeset（用于统计最终的volumeset）
	vector<floatType>   volumeset;//用于MSA（用于计算w_i^d）
	vector<floatType>   oldvolumeset;
	vector<floatType>   SCvolumeset;

	PTOrg*				related_OD;//for DCL初始加载流量至walklink中

	
	floatType			freq;
	floatType			efffreq;
	floatType			effpower;
	int					hyperpath_times;

	void		TL_UpdateGeneralPTLink_Cost_DerCost(floatType lambda);
	void		TL_UpdateGeneralPTLink_Cost_DerCost_const(floatType lambda);
	void		BL_UpdateGeneralPTLink_Cost_DerCost(floatType lambda);
	void		BL_UpdateGeneralPTLink_Cost_DerCost_const(floatType lambda);
	void		UpdateGeneralPTLink_Cost_DerCost(floatType lambda);


	void showinfro()
	{
		if(m_tlType == PTLink::ABOARD)
		{
			cout<<id<<" , "<<"aboard , "<<volume<<" , "<<cost<<" , "<<efffreq<<endl;
		}
		else if(m_tlType == PTLink::WALK)
		{
			cout<<id<<" , "<<"walk , "<<volume<<" , "<<cost<<" , "<<efffreq<<endl;
		}
		else
		{
			cout<<id<<" , "<<"enroute , "<<volume<<" , "<<cost<<" , "<<efffreq<<endl;
		}
	}
};


class PTNet_API GNODE
{
public:
	GNODE(PTNode* s)
	{
		m_ptnodePtr = s;
		m_data = 0.0;
		m_wait = 0.0;
		m_stgname = "";
		m_ptnodeLCNPtr = NULL;
		m_tpLevel = 0;
		status = 0;
		flow = 0.0;
	}

	vector<GLINK*>		node_link;

	inline  StgLinks* SearchStgbyName(string name)
	{	
		for (vector<StgLinks*>::iterator it =m_StgsVec.begin();it != m_StgsVec.end();it++)
		{
			if ((*it)->sname == name)
			{			
				return *it;
			}
		}
		return NULL;
	};

	inline StgLinks* SearchBoardStgByContainLink(int fid)
	{
		for (vector<StgLinks*>::iterator it =m_StgsVec.begin();it != m_StgsVec.end();it++)
		{
			if ((*it)->isboarding)
			{
				vector<int>::iterator fit = find((*it)->StgLinkPosVec.begin(), (*it)->StgLinkPosVec.end(),fid);

				if (fit != (*it)->StgLinkPosVec.end())
					return (*it); 
			}		
		}
		return NULL;
	
	}

	void					UpdateOutBoardingLam();
	void					UpdateOutBoardingLamII();
	void					UpdateOutBoardingLamIII();
	int						SolveFixedPoint();
	int						SolveFPwithMSA_gcost(floatType lambda);//利用MSA算法求解站点问题，其中link的费用更新为gost
	int						SolveFPwithGP();

	int						SolveFPwithGP_test();
	int						SolveFPwithGP_test_shuchu();

	int						SolveFPwithGP_gcost(floatType lambda);//利用GP算法求解站点问题，其中link的费用更新为gost
	int						SolveFPwithGPln();
	int						SolveFPwithGPlinesearch();
	int						SolveFPwithGPlinesearch_gcost(floatType lambda);//利用linesearch算法求解站点问题，其中link的费用更新为gost
	//TNM_HyperPath*			m_hp;
	

	inline int			id_() {return m_ptnodePtr->id;} 

	void					UpdateLinkProbability(floatType timescaler = 60);

	PTNode*					m_ptnodePtr;//Gnode对应的ptnode
	bool					m_mutliStart;//multi outgoing links
	bool					m_mutliStg; //multi stgs
	int						m_tpLevel; //topological level;
	PTNode*					m_ptnodeLCNPtr;//last common node pointer;
	floatType				m_data;// prob on a hyperpath
	floatType				m_wait;
	string					m_stgname;
	vector<StgLinks*>		m_StgsVec;//store stgs
	StgLinks*               shortestvia;
	int						status;
	floatType				minShiftflow;
	floatType				maxShiftflow;
	floatType				flow;
	floatType				exceed_flow;
	//floatType				m_cost;// shortest strategy cost from the tail node of the stg to destination
	//floatType				m_rcost;// longest strategy cost from the tail node of the stg to destination

	floatType		prediff;
	floatType		afterdiff;

	vector<floatType>   volumeset;
	
};



class PTNet_API GLINK
{
public:
	GLINK(PTLink *link, GNODE* tail, GNODE* head ) // this construction is for bush-based alg
	{
		m_linkPtr = link; 
		m_head = head; 
		m_tail = tail; 
		m_data =0.0;
		m_prob = 0.0;
		revStgLink = NULL;
		buffer = NULL;
		volume = 0;
		prevolume = 0;
		lam = 0;
		m_gflow = 0.0;
	}

	GLINK(PTLink *link)
	{
		m_linkPtr = link; 
		m_data =0.0;
		m_datav2 = 0.0;
		m_prob = 0.0;
		revStgLink = NULL;
		volume = 0;
		prevolume = 0;
		lam = 0;
		gt_cost = 0.0;
		m_mcost = 0.0;
		gap = 0.0;
		effective_freq = 0.0;
	}
	inline int	id_() {return m_linkPtr->id;} 
	void AllocateBuffer(int n)
	{
	
	
	}
	bool        rlink_already_inflow;
	GLINK*		revStgLink;
	PTLink*		m_linkPtr;
    GNODE*		m_head;
    GNODE*		m_tail;
	floatType	effective_freq;//
	floatType	max_blink_flow;//
	bool        if_exceed;

	floatType   gap;
	floatType	temp_value;
    floatType	m_data;// prob on a hyperpath
	floatType	m_datav2;
	floatType   m_prob;//approach probability
	floatType   gt_cost;//Generalized time cost、、
	floatType   m_mcost;//path-specified money cost measured in time, min、、
	floatType	volume;//store destination-based link flow
	floatType	prevolume;//前流量
	floatType	PREVOLUME;//前流量
	floatType	oldvalue;//前流量
	floatType   *buffer;
	floatType	lam;//add for additional cost on boarding links
	floatType   m_gflow;// path-specified glink flow、、
	void        UpdateGLinkCost() { gt_cost = m_linkPtr->cost + m_mcost;};
	floatType	cost;//ln(eff)    VI问题对应的cost
	floatType	der;

	//void UpdateDer()
	//{
	//	der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((0.2) * pow(m_linkPtr->volume/(m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-0.8)) * pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1)) / pow(m_linkPtr->efffreq,2);
	//}

	void UpdateDer()
	{
		if(volume < m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume)
		{
			//der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((0.2) * pow(m_linkPtr->volume / (m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), -0.8)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume) / pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 2))) / pow(m_linkPtr->efffreq, 2);

			if(m_linkPtr->efffreq > 0.00001)//0.00001
			{
				//der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((1.0) * pow(m_linkPtr->volume / (m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 0.0)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume) / pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 2))) / pow(m_linkPtr->efffreq, 2);

				der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((2.0) * pow(m_linkPtr->volume / (m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 1.0)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume) / pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 2))) / pow(m_linkPtr->efffreq, 2);

				//der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((0.2) * pow(m_linkPtr->volume/(m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),-0.8)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2))) / pow(m_linkPtr->efffreq,2);
			
				//der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((0.5) * pow(m_linkPtr->volume / (m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), -0.5)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume) / pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 2))) / pow(m_linkPtr->efffreq, 2);
				
				//der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((0.1) * pow(m_linkPtr->volume / (m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), -0.9)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume) / pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 2))) / pow(m_linkPtr->efffreq, 2);
			
				//der = (m_linkPtr->efffreq + volume * (m_linkPtr->freq) * ((0.05) * pow(m_linkPtr->volume / (m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), -0.95)) * ((m_linkPtr->cap - m_linkPtr->rLink->volume) / pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume), 2))) / pow(m_linkPtr->efffreq, 2);
			}
			else
			{
				der = 1.0 / m_linkPtr->efffreq;
			}
		}

		else
		{
			der = 1.0 / m_linkPtr->efffreq;
		}

	}

	void UpdateDerln()
	{
		//der = ((1/m_linkPtr->m_hwmean) * ((0.2) * pow(m_linkPtr->volume/(m_linkPtr->cap * 60/m_linkPtr->m_hwmean + m_linkPtr->volume - m_linkPtr->rLink->volume),-0.8)) * pow((m_linkPtr->cap * 60/m_linkPtr->m_hwmean + m_linkPtr->volume - m_linkPtr->rLink->volume),-1)) / pow(m_linkPtr->efffreq,1);
	}
};

typedef struct finder_t
{
   finder_t(int n) : id(n) { }

	bool operator()(GLINK *p)
	{ 
		return (id == p->m_linkPtr->id); 
	}

	int id;
 } finder_t;





class PTNET;
class PTNet_API TNM_HyperPath
{
public:
	TNM_HyperPath() {flow = 0.0;Preflow=0.0;cost=0.0;fdcost=0.0;WaitCost = 0.0;name="";transfer=0;tempcost=0.0;walktime=0.0;trvelTime = 0.0;Max_w = 0.0;}

		~TNM_HyperPath() {
		for (auto node : m_nodes) {
			delete node;
		}
		for (auto link : m_links) {
			delete link;
		}
	}

	floatType    pre_flow;
	floatType    pre_cost;

	floatType    in_veichle_cost;
	floatType    walking_cost;
	floatType    True_cost;//用于统计出行的ttcost
	floatType    Exceed_Flow;
	floatType    tempuse_MSA;
	floatType    flow;
	floatType    cost;
	floatType    tempcost;
	floatType	 fdcost;//first derivative cost 
	floatType	 WaitCost;
	floatType	 Max_w;
	floatType	 Preflow;
	floatType    s1;
	floatType    s2;
	int			 s3;
	int transfer;
	float walktime;
	string		 name;
	double             trvelTime;

	inline vector<GLINK*> GetGlinks() {return m_links;}

	void AddGlinks(PTNode* node)
	{
		GNODE* gnode = new GNODE(node);
		//gnode->m_hp = this;
		m_nodes.push_back(gnode);

		PTLink* link = node->StgElem->vialink;
		WaitCost += node->m_tmpdata * node->m_wait;//更新hyperpath中的总等待时间
		gnode->m_data = node->m_tmpdata;//更新hyperpath中的节点的概率
		gnode->m_wait = node->m_wait;//更新节点的等待时间
		//cout<<node->id<<" : "<<node->m_tmpdata<<" , "<<node->m_wait<<endl;
		int i = 0;
		while (link)//如果是站点的上车弧集合，就会循环；如果不是只更新一条弧
		{
			GLINK* newglink = new GLINK(link);//增加hyperpath中的link（按拓扑顺序）
			name += std::to_string(link->id) + "-";
			newglink->m_tail = gnode;
			//GNODE* hgnode = new GNODE(link->head);
			//newglink->m_head = hgnode;
			//if(newglink->m_linkPtr->id == 14)
			//{cout<<""<<endl;}
			newglink->m_data = node->m_tmpdata * node->m_attProb[i];//更新link的使用概率（hyperpath中的link概率）
			//cout<<node->m_tmpdata<<" , "<<node->m_attProb[i]<<endl;
			newglink->m_prob = node->m_attProb[i];
			m_links.push_back(newglink);
			gnode->node_link.push_back(newglink);
			link = link->stglinkptr;
			i++;
		}
		
	}

	void UpdateGLinksCost()
	{
		for(int i=0;i<m_links.size();++i)
		{
			GLINK* glink =   m_links[i];
			glink->UpdateGLinkCost();	
		}
	}

	//================hyperpath to tree==================================
	vector<Treenode*> tree;
	vector<Treenode*>::iterator tree_it;
	void Hyperpath2Tree()
	{
		for(int i = 0;i<m_links.size();i++)
		{
			GLINK* glink = m_links[i];
			for(int j = 0;j<m_nodes.size();j++)
			{
				if(m_nodes[j]->m_ptnodePtr == glink->m_linkPtr->head)
				{
					glink->m_head = m_nodes[j];
					break;
				}
			}
		}
		GNODE* gnode = m_nodes[0];
		Treenode* root = new Treenode(gnode->node_link.size(),gnode);
		root->prob = gnode->m_data;
		tree.push_back(root);
		generateTree(gnode,root);

	}
	void generateTree(GNODE* gnode,Treenode* tnode)
	{
		for (int i = 0;i<gnode->node_link.size();i++)
		{
			GLINK* glink = gnode->node_link[i];
			GNODE* tempgnode = glink->m_head;
			Treenode* temptnode = new Treenode(tempgnode->node_link.size(),tempgnode);
			temptnode->parent = tnode;
			temptnode->prob = min(tnode->prob,tempgnode->m_data);
			tree.push_back(temptnode);
			tnode->children.push_back(temptnode);
			generateTree(tempgnode,temptnode);
		}
	}
	vector<Treenode*> getleaf()
	{
		vector<Treenode*> leaf;
		for (tree_it = tree.begin();tree_it != tree.end();tree_it++)
		{
			if((*tree_it)->val == 0)
			{
				leaf.push_back((*tree_it));
			}
		}
		return leaf;
	}
	vector<string> getStopArray(Treenode* leaf)
	{
		vector<string> stop_id;
		Treenode* tnode = leaf;
		while(tnode->parent)
		{
			PTStop* stop = tnode->gnode->m_ptnodePtr->GetStopPtr();
			if(stop)
			{
				string temp = stop->m_id;
				vector<string>::iterator it = find(stop_id.begin(),stop_id.end(),temp);
				if (it == stop_id.end())//not found
				{
					stop_id.push_back(temp);
				}
			}
			tnode = tnode->parent;//
		}

		PTStop* stop = tnode->gnode->m_ptnodePtr->GetStopPtr();
		if(stop)
		{
			string temp = stop->m_id;
			vector<string>::iterator it = find(stop_id.begin(),stop_id.end(),temp);
			if (it == stop_id.end())//not found
			{
				stop_id.push_back(temp);
			}
		}
		return stop_id;

	}

	vector<string> getShapeArray(Treenode* leaf)
	{
		vector<string> shape_id;
		Treenode* tnode = leaf;
		while(tnode->parent)
		{
			if(tnode->gnode->m_ptnodePtr->GetTransitNodeType() != PTNode::TRANSFER)
			{
				PTShape* shape = tnode->gnode->m_ptnodePtr->m_shape;
				if(shape)//
				{
					string temp = shape->m_id;
					vector<string>::iterator it = find(shape_id.begin(),shape_id.end(),temp);
					if (it == shape_id.end())//not found
					{
						shape_id.push_back(temp);
					}
				}
			}
			//PTStop* stop = tnode->gnode->m_ptnodePtr->GetStopPtr();
			tnode = tnode->parent;
		}
		if(tnode->gnode->m_ptnodePtr->GetTransitNodeType() != PTNode::TRANSFER)
		{
			PTShape* shape = tnode->gnode->m_ptnodePtr->m_shape;
			if(shape)
			{
				string temp = shape->m_id;
				vector<string>::iterator it = find(shape_id.begin(),shape_id.end(),temp);
				if (it == shape_id.end())//not found
				{
					shape_id.push_back(temp);
				}
			}
		}
		
		return shape_id;

	}

	//====================================================================




	/*
	void  AddNodeLinks(PTNode* node)
	{
		GNODE *pnode = new GNODE(node);
		m_nodes.push_back(pnode);
		pnode->m_data = node->m_tmpdata;
		StgLinks* stg = node->m_StgVia;
		if (stg)
		{
			//pnode->m_wait = stg->waitT;
			pnode->m_stgname = stg->sname;		
			WaitCost += pnode->m_data * stg->waitT;

			GLINK* glink = stg->StgLinkVia;
			while (glink)
			{
				GLINK* newglink = new GLINK(glink->m_linkPtr);
				newglink->m_prob = glink->m_prob;
				newglink->m_data = pnode->m_data * glink->m_prob;
				m_links.push_back(newglink);
				glink = glink->revStgLink;
		
			}
		}
	};
	*/


	bool IsSameHyperpath(TNM_HyperPath* cpath)
	{
		if (m_nodes.size()!=cpath->m_nodes.size()) return  false;
		else
		{
			for (int i=0;i<m_nodes.size();i++)
			{
				if (m_nodes[i]->m_stgname != cpath->m_nodes[i]->m_stgname)
				return  false;
			}		
		}
		return true;
	}

	void UpdateGeneraleffHyperpathCost()
	{
		cost = WaitCost;
		fdcost = 0.0;
		for(int i = 0; i < m_links.size(); ++i)
		{
			PTLink* link =   m_links[i]->m_linkPtr;
			cost += m_links[i]->m_data * link->g_cost;//更新hyperpath中的总general费用
			
			//fdcost += m_links[i]->m_data * link ->pfdcost * m_links[i]->m_data;	

			//if (link->rLink && link->GetTransitLinkSymmetry() == PTLink::ASYMMTRIC)
			//{
			//	vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), finder_t(link->rLink->id)); 			
			//	if (it != m_links.end())
			//	{
			//		fdcost += m_links[i]->m_data *  link->rpfdcost * (*it)->m_data;//如果非对称，考虑非对称link
			//	}
			//}

			//if (  link->GetTransitLinkType() == PTLink::ENROUTE && link->volume >= (link->cap - link->mu/lambda) && link->GetTransitLinkCapType()==PTLink::TL)
			//{
			//	fdcost +=  m_links[i]->m_data *  lambda * m_links[i]->m_data; //这里的lambda是惩罚因子，link->mu是link的乘子
			//}

		}
	}


	void UpdateHyperpathCost()
	{
		cost = WaitCost;
		fdcost = 0.0;
		for(int i=0;i<m_links.size();++i)
		{
			PTLink* link =   m_links[i]->m_linkPtr;
			cost += m_links[i]->m_data * m_links[i]->m_linkPtr->cost;//更新hyperpath中的总费用（不包含等待费用）
			fdcost += m_links[i]->m_data * link ->pfdcost * m_links[i]->m_data;	//更新hyperpath的一阶导数
			if (  link->rLink &&  link->GetTransitLinkSymmetry()==PTLink::ASYMMTRIC && link->rLink  )
			{
				//vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), predP(&GLINK::id_, link->rLink->id)); 
				vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), finder_t(link->rLink->id)); 			
				if (it != m_links.end())
				{
					fdcost += m_links[i]->m_data *  link->rpfdcost * (*it)->m_data;
				}
			}
			//delete link;
		}

		
		
		//cost = WaitCost;
		//fdcost = 0.0;
		//trvelTime = WaitCost;
		//for(int i=0;i<m_links.size();++i)
		//{
		//	/*PTLink* link =   m_links[i]->m_linkPtr;
		//	cost += m_links[i]->m_data * m_links[i]->m_linkPtr->cost;*/

		//	GLINK* link =   m_links[i];
		//	link->gt_cost = link->m_linkPtr->cost + link->m_mcost;
		//	trvelTime +=  m_links[i]->m_data * link->m_linkPtr->cost;
		//	cost += m_links[i]->m_data * (link->m_linkPtr->cost + link->m_mcost);
		//	fdcost += m_links[i]->m_data * link->m_linkPtr->pfdcost * m_links[i]->m_data;	
		//	if (link->m_linkPtr->rLink && (link->m_linkPtr->GetTransitLinkSymmetry()==PTLink::ASYMMTRIC))
		//	{
		//		//vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), predP(&GLINK::id_, link->rLink->id)); 
		//		vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), finder_t(link->m_linkPtr->rLink->id)); 
		//		
		//		if (it != m_links.end())
		//		{
		//			/*if((link->m_linkPtr->mode != MODETYPE::TAXI_RIDESOURCE || link->m_linkPtr->mode != MODETYPE::PRIVATE_CAR))
		//			{
		//				fdcost += m_links[i]->m_data *  link->m_linkPtr->rpfdcost * (*it)->m_data;
		//			}*/
		//			fdcost += m_links[i]->m_data *  link->m_linkPtr->rpfdcost * (*it)->m_data;
		//		}
		//	}
		//}
	}

	void UpdateHyperpathCostV2()
	{
		cost = WaitCost;
		fdcost = 0.0;
		for(int i=0;i<m_links.size();++i)
		{
			PTLink* link =   m_links[i]->m_linkPtr;
			cost += m_links[i]->m_datav2 * m_links[i]->m_linkPtr->cost;
			//fdcost += m_links[i]->m_data * link ->pfdcost * m_links[i]->m_data;	
			//if (  link->rLink &&  link->GetTransitLinkSymmetry()==PTLink::ASYMMTRIC && link->rLink  )
			//{
			//	//vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), predP(&GLINK::id_, link->rLink->id)); 
			//	vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), finder_t(link->rLink->id)); 			
			//	if (it != m_links.end())
			//	{
			//		fdcost += m_links[i]->m_data *  link->rpfdcost * (*it)->m_data;
			//	}
			//}
		}
	}

	//=====================effective frequency=================================
	void UpdateWaitcostV1();
	void UpdateWaitcost();
	void UpdateWaitcost_TEST();
	void Updateinfro();
	void UpdateinfroV2();
	void Showeff();//输出hyperpath中每个站点上车弧对应的有效发车频率
	void loadingflowv2();
	void loadingflow();//将hyperpath中的流量加载到glink中


	void LoadingFlow_test();//将hyperpath中的流量加载到glink中

	int LoadingFlow_Test();//将hyperpath中的流量加载到glink中

	bool loadingflow_V3();//将hyperpath中的流量加载到glink中(返回是否有超容路段)
	void loadingflow_gcost(floatType lambda);//将hyperpath中的流量加载到glink中(其中弧费用为general_cost)
	//=============================================================================

	void BL_UpdateCapHyperpathCost_for_test(floatType lambda);

	void BL_UpdateCapHyperpathCost(floatType lambda);
	
	void UpdateCapHyperpathCost(floatType lambda);

	void TL_UpdateCapHyperpathCost(floatType lambda);

	//{
	//	cost = WaitCost;
	//	fdcost = 0.0;
	//	for(int i=0;i<m_links.size();++i)
	//	{
	//		PTLink* link =   m_links[i]->m_linkPtr;
	//		cost += m_links[i]->m_data * link ->g_cost;
	//		fdcost += m_links[i]->m_data * link ->g_pfdcost * m_links[i]->m_data;	
	//		if (link->rLink)
	//		{
	//			vector<GLINK*>::iterator it = find_if(m_links.begin(), m_links.end(), finder_t(link->rLink->id)); 
	//			
	//			if (it != m_links.end())
	//			{
	//				fdcost += m_links[i]->m_data *  link->g_rpfdcost * (*it)->m_data;
	//			}
	//
	//		}
	//	}
	//}


	std::vector<GNODE*> m_nodes;//hyperpath中使用的node
    std::vector<GLINK*> m_links;//hyperpath中使用的Link


	bool		InitializeHP(PTNode *org, PTNode *dest);	
	bool		InitializeHP1(PTNode *org, PTNode *dest);
	
	//bool		InitializeHP(PTNode *org, PTNode *dest);	
	void		print()
	{
		double t = WaitCost;
		int k=0;
		floatType time=0.0;
		//cout<<"od:"<<org->id<<"->"<<dest->id<<endl;
		if(m_links.size()>0)
		{PTNode* org=m_links[0]->m_linkPtr->tail;
		
		for (int i=0;i<m_links.size();i++)
		{
			cout<<m_links[i]->m_linkPtr->id<<"("<<m_links[i]->m_data<<","<<m_links[i]->volume<<"):"<<m_links[i]->m_linkPtr->efffreq<<"->";
			t += m_links[i]->m_linkPtr->cost * m_links[i]->m_data;
		}
		cout<<endl;
		cout<<"WaitCost:"<<WaitCost<<", pathcost:"<<t<<", flow:"<<flow<<endl;}
	}

	void		printv2()
	{
		double t = WaitCost;
		int k=0;
		floatType time=0.0;
		//cout<<"od:"<<org->id<<"->"<<dest->id<<endl;
		if(m_links.size()>0)
		{PTNode* org=m_links[0]->m_linkPtr->tail;
		
		for (int i=0;i<m_links.size();i++)
		{
			cout<<m_links[i]->m_linkPtr->id<<"->";
			t += m_links[i]->m_linkPtr->cost * m_links[i]->m_data;
		}
		cout<<endl;
		cout<<"WaitCost:"<<WaitCost<<", pathcost:"<<t<<", flow:"<<flow<<endl;
		}
	}

	double			ComputePfdWaitcost()
	{
		double ans = 0.0;
		//cout<<m_nodes.size()<<endl;
		for(int i = 0;i<m_nodes.size();i++)
		{
			if(m_nodes[i]->m_ptnodePtr->GetTransitNodeType() == PTNode::TRANSFER)
			{
				double temp = 0.0;
				for(int j = 0;j<m_nodes[i]->node_link.size();j++)
				{
					GLINK* glink = m_nodes[i]->node_link[j];
					PTLink* link = glink->m_linkPtr;
					//这个是根据eff的计算来的
					if(link->GetTransitLinkType() == PTLink::ABOARD && (link->cap * 60/link->m_hwmean - link->rLink->volume) > 0)
					{
						//temp += glink->m_data * (1/link->m_hwmean) * ((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)) * (link->cap * 60/link->m_hwmean - link->rLink->volume) * pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-2);
						temp += glink->m_data * (1/link->m_hwmean) * ((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)) * pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-1);
						//cout<<glink->m_data<<","<<(1/link->m_hwmean)<<","<<((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8))<<","<<(link->cap * 60/link->m_hwmean - link->rLink->volume)<<","<<pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-2)<<endl;
						//cout<<pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)<<endl;//link->volume = 0 ?????

						/*if(link->rLink)
						{
							temp += glink->m_data * (1/link->m_hwmean) * ((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)) * (link->volume) * pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-2);
						}*/
					}
				}
				//cout<<"---------------------------------------------------------------------"<<endl;
				//cout<<m_nodes[i]->m_data<<","<<temp<<","<<pow(m_nodes[i]->m_wait,2)<<endl;

				ans += (m_nodes[i]->m_data) * temp * pow(m_nodes[i]->m_wait,2);
			}
			
		}
		return ans;
	}

	double			ComputePfdWaitcostForShow()
	{
		double ans = 0.0;
		//cout<<m_nodes.size()<<endl;
		for(int i = 0;i<m_nodes.size();i++)
		{
			if(m_nodes[i]->m_ptnodePtr->GetTransitNodeType() == PTNode::TRANSFER)
			{
				double temp = 0.0;
				for(int j = 0;j<m_nodes[i]->node_link.size();j++)
				{
					GLINK* glink = m_nodes[i]->node_link[j];
					PTLink* link = glink->m_linkPtr;
					if(link->GetTransitLinkType() == PTLink::ABOARD && (link->cap * 60/link->m_hwmean - link->rLink->volume) > 0)
					{
						//temp += glink->m_data * (1/link->m_hwmean) * ((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)) * (link->cap * 60/link->m_hwmean - link->rLink->volume) * pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-2);
						temp += glink->m_data * (1/link->m_hwmean) * ((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)) * pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-1);
						//cout<<glink->m_data<<","<<(1/link->m_hwmean)<<","<<((0.2) * pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8))<<","<<(link->cap * 60/link->m_hwmean - link->rLink->volume)<<","<<pow((link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-2)<<endl;
						//cout<<pow(link->volume/(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume),-0.8)<<endl;//link->volume = 0 ?????
						//cout<<link->volume<<" , "<<(link->cap * 60/link->m_hwmean + link->volume - link->rLink->volume)<<endl;
					}
				}
				//cout<<"---------------------------------------------------------------------"<<endl;
				cout<<m_nodes[i]->m_data<<","<<temp<<","<<pow(m_nodes[i]->m_wait,2)<<endl;

				ans += (m_nodes[i]->m_data) * temp * pow(m_nodes[i]->m_wait,2);
				//cout<<"ans: "<<ans<<endl;
			}
			
		}
		//cout<<"ans: "<<ans<<endl;

		return ans;
	}
};

class PTNet_API TNM_HyperPathTP:public TNM_HyperPath
{
public:
	TNM_HyperPathTP();
	int                transfers;
	int                state;
	double             moneySpent;//yuan
	
	double             trvelDist;//distance,km
	//vector<MODETYPE>   travelScheme;
	bool			   InitializeHP_TP(PTNode *org, PTNode *dest,int k,floatType walklimit);
	void               AddGlinksTP(TPHYPERPATHELEM* label);
	PTLink*	           CatchLinkPtrTP(PTNode* tail, PTNode *head);
	//int				   GetAttractiveSetAndState(vector<PTLink*> links, vector<PTLink*> &attractivelinks,floatType &ec,floatType &ew);
	int                UpdateBoardingStgTP(PTNode* tailNode,vector<PTLink*> attlinks,double ew,double ec);
	void               printTP();
	void               PrintVialinksTP(TPHYPERPATHELEM* curlabel);
	void               UpdateHyperpathCostTP();
	void               UpdateGLinksCostTP();
};

struct PTNet_API TPHYPERPATHELEM:public HYPERPATHELEM
{
	TPHYPERPATHELEM();
	//~TNM_HyperPathTP();
	TPHYPERPATHELEM* viaNodeLabel;
	TPHYPERPATHELEM* stgNodeLabel;
	vector<TPHYPERPATHELEM*> stgLabels;

	PTNode* node;
	int scanStatus;
	vector<floatType> m_attProb;
	vector<int>             m_attStates;
	double                  curModeTravelDist;
	double                  m_wait; //only for bus waiting
	double                  moneySpent;//Corresponding travel strategy
	double                  trvelTime;// in min
	double                  trvelDist;//total distance,km 
	vector<floatType>       via_m_cost;// vialinks money cost measured in time, min
	double                  tempuse;
	bool                    isInserted;
	
	void                    ResetLabel();
	void                    CleanStgLabelOnHyperPath();
	void                    PrintStg();
	void                    PrintStgLabels();
};



class PTNet_API PTOrg
{
	public:
		PTOrg() { org = NULL; dest = NULL; related_dest = NULL; assDemand = 0; buffer = NULL; maxPathGap = 0.0; minIx = 0; state = true; OD_walklink = NULL; CriticalOD = false; } //constructor

/*=======================================================================================
                  data members
  ======================================================================================*/
	PTLink*         OD_walklink;//该OD对对应的walklink
	PTNode*         dest;
	PTDestination* related_dest;
	PTNode*         org;//node on which the origin destination rest	
	bool			state;
	floatType           assDemand;//assignment O-D demand
	floatType           maxPathGap;//(max hyperpath cost - min hyperpath cost)
	floatType           *buffer;
	floatType			currentTotalCost;
	double				currentRelativeGap;
	vector<TNM_HyperPath*> pathSet;
	vector<TNM_HyperPathTP*> pathSetTP;
	int					minIx;// minimum cost of path index in the pathset  
	bool                Initialize(PTNode *destnode, PTNode *orgnode, floatType ass)
	{
		if(destnode == NULL || orgnode  == NULL) return false;
		
		dest   =   destnode;
		org = orgnode;
		assDemand = ass;
		return true;
	}
	void				ComputePathSetCostdiff(double &mincost, double &maxcost, double &ttcost);
	void				UpdatePathSetCost();
	void				UpdatePathSetCost_eff();
	void				ODpathset_loadingflow();
	void				UpdatePathSetCost_effv2();
	void				UpdatePathSetCostTP();
	void				ClearFlow();
	void				UpdatePathSetCost_TP();
	void				PathFlowConservation();//peiping
	void				PathFlowConservationTP();//peiping
	void				PathFlowConservation_eff();
	void				PathFlowConservation_eff_gcost(floatType lambda);//确保流量守恒，其中loading过程使用的是general_cost
	void				PathFlowConservation_effv3();
	TNM_HyperPath*		walkpath;


	void				TL_UpdateCapPathSetCost(floatType lambda); //for capacited problem 
	void				TL_UpdateCapeffPathSetCost(); //for capacited eff problem 
	void				BL_UpdateCapPathSetCost(floatType lambda);
	void				UpdateCapPathSetCost(floatType lambda);


	void				UpdatePathMax_w();
	void				Pretreatment();

	bool                CriticalOD;
};



class PTNet_API PTDestination
{
	public:
	PTDestination();  /*constructor*/

	PTDestination(PTNode* dest, int nd)
	{
		destination = dest;
		numOfOrg = nd;

		if(numOfOrg > 0) 
		{
			orgVector = new PTOrg*[numOfOrg];
			for (int i = 0;i<numOfOrg;i++)  orgVector[i] = new PTOrg;
		}
		else
		{
			orgVector = NULL;
		}
		m_tdmd = 0.0;
		m_trimmed = false;
		m_expanded = false;
	}
	inline int			id_() {return destination->id;} //return id of the dest node 
	//add for path-based algorithm
	void		InitialSubNet(PTNET *net);
	void		DestAllOrNothing();
	bool        TopologOrder(PTNET *net);
	void		RemarkBushLinksOnNet();
	void		MarkBushLinksOnNet();
	int			ExpandSubNet(PTNET *net);
	double      UpdateNodePotential(bool iscontributing = false);
	double		NodeFlowShift(PTNode *node,PTNET *net);
	int			TrimSubNet();
	


	//add for effective frquency model
	void		EffStgSubNetAllorNothing(PTNET *net);
	void		InitialStgNodeLinks(PTNET *net);
	void		AssignEffStgFlow(PTNode* org, PTNode* dest, double dmd);
	void		EffMarkStgLinksOnNet();
	void        EffRemarkStgLinksOnNet();
	int			EffExpandSubNet(PTNET *net);
	floatType	EffEquilibriumSubNet(PTNode *node,PTNET *net);
	void		EffUpdateSubNetStgFlow();
	void		EffStgFlowShift(PTNET *net);
	GNODE*		FindNearestFPD(GNODE* onode, vector<GNODE*> &M, bool isshort, double &hyperpathcost, double &maxshiftflow );
	void		EffShiftFlowEPHS(vector<GNODE*> Q, vector<GNODE*> rQ, double shift);
	//hyperbush algorithm
	void		InitialStgSubNet(PTNET *net);
	void		AssignInitialStgFlow(PTNode* org, PTNode* dest, double dmd);
	bool        StgTopologOrder(PTNET *net);
	void        RemarkStgLinksOnNet();
	void		MarkStgLinksOnNet();
	void        UpdateStgSPTreeOnly(); //only sp tree is updated.
	void        TrimStgSubNet(PTNET *net);
	floatType   StgNodeCostDiff(PTNode *node,PTNET *net);
	floatType	StgNodeFlowShift(PTNode *node,PTNET *net);	
	floatType	eStgNodeFlowShift(PTNode *node,PTNET *net);	// shitflow from maxflow stg 
	floatType	StgNodeFlowShiftI(PTNode *node,PTNET *net);	//insert into a list by topological order
	floatType	StgNodeFlowShiftII(PTNode *node,PTNET *net);	//do not use multimap, turn to vector 
	double      UpdateStgPotential(bool iscontributing = false);
	int         ExpandStgSubNet(PTNET *net);

	map<int, PTOrg*> node2org;////建立org->id（org->node）与dest->org之间的关系(在MSA_DCL中的build中使用)


	void		UpdateEfficientStgFlow();

	bool		SetOrg(int id, PTNode *orgnode, floatType demand)
	{
		if(id>numOfOrg||id<=0) 
		{
			cout<<"\n\tSetOrg in Destination Object: dest index exceeds the rannge!"
				<<"\n\trequired index = "<<id<<endl;;
			return false;
		}
		return orgVector[id - 1]->Initialize(destination, orgnode, demand);
	}

	PTNode*					destination;       //node pointer
	int						numOfOrg;
	PTOrg					**orgVector;  //origins
	double					m_tdmd;       //total demand
	double					avgCOV;
	vector<GNODE*>			tplNodeVec;// a node vector follows topological order
	vector<GLINK*>			tplLinkVec;//add to store destination-based link flow
	bool					m_trimmed; //whether or not subnetwork is trimmed since last trim operation
	bool					m_expanded; //whether or not a subnetwork is expanded since last expanded operation. 
	bool					m_reduced; //whether or not a subnetwork is reduced for a strategy


	// capacited TEAP model
	double      UpdateCapStgPotential(bool iscontributing = false);
	int         ExpandCapStgSubNet(PTNET *net);
	floatType	CapStgNodeFlowShift(PTNode *node,PTNET *net);	
	void        UpdateCapStgSPTreeOnly();
	void        TrimCapStgSubNet(PTNET *net);


	int			order;//记录dest在dest_vec中的顺序
};



struct PTNet_API PTITERELEM
{
	PTITERELEM(){innerlooptime = 0.0;};
	int         iter; //current iteration number
	double      convGap; //current gap
	double	   convIF;//infeasible flow
	double      convRGap; //current relative gap
	double      stepsize; //current step size
	float       time; //current cputimeprint
	float		mainlooptime;
	float		innerlooptime;
	int			innerIters;
	int			numberofhyperpaths;
	float		multiplierconv;//add for cap TEAP
	float		penalty;//add for cap TEAP
	int			numof_infeasible_arcs;
};

class PTNet_API GRIDPTNETPAR
{
public:
	GRIDPTNETPAR()
	{
		seed = 1;
		nx   = 10;
		ny   = 10;
		zRatio=0.125;
		dLevel = 0.5;
		gridLen= 0.5;
		randLen= false;
		commonLines= 1;
        loop    = true;

	}
	GRIDPTNETPAR(const GRIDPTNETPAR &rhs)
	{
		seed = rhs.seed;
		nx   = rhs.nx;
		ny   = rhs.ny;
		zRatio = rhs.zRatio;
		dLevel  = rhs.dLevel;
		gridLen = rhs.gridLen;
		randLen = rhs.randLen;
		commonLines = rhs.commonLines;
        loop     = rhs.loop;
	}
	void   Print();
	void   Input();
    int    Input(const string &fileName);
	int   Save(const string &fileName);
public:
	int    seed;
	int    nx; //x - number of stops
	int    ny; //y - number of stops
	float  zRatio; //zone ratio
	float  dLevel;//demand level between 0 and 1
	float  gridLen; //grid length in mile
	bool   randLen; //random length.
	int    commonLines; //common lines of two adjacent stop
    bool   loop;
};

typedef map<string,PTStop*, less<string> > PTStopMap;
typedef map<string,PTStop*, less<string> >::iterator PTStopMapIter;
typedef map<string,PTRoute*, less<string> > PTRouteMap;
typedef map<string,PTRoute*, less<string> >::iterator PTRouteMapIter;
typedef map<string,PTShape*, less<string> > PTShapeMap;
typedef map<string,PTShape*, less<string> >::iterator PTShapeMapIter;

struct PTPostgresConfig
{
	string host;
	string port;
	string database;
	string user;
	string password;
	string schema;
	int connectTimeoutSeconds;
};

struct PTDbRouteRecord
{
	string routeId;
};

struct PTDbStopRecord
{
	string stopId;
	string stopName;
};

struct PTDbShapeStopRecord
{
	string shapeId;
	string routeId;
	string dir;
	int group;
	double frequency;
	int stopSequence;
	string stopId;
};

struct PTDbTransitRecord
{
	string shapeId;
	string fromStop;
	string toStop;
	double fft;
};

struct PTDbWalkRecord
{
	string fromStop;
	string toStop;
	double fft;
	double length;
};

struct PTDbTripRecord
{
	string originStop;
	string destinationStop;
	double demand;
};

struct PTDbWayRecord
{
	string linkId;
	string routeId;
	string dir;
	int group;
	int linkSequence;
	string initNode;
	string termNode;
};




class PTNet_API PTNET
{
public:
    PTNET(const string &name)
	{
		networkName = name;
		numOfNode = 0;
		numOfLink = 0;
		numOfPTDest=0;
		numOfPTOD=0;
		numOfPTTrips = 0;
		transfer_times = 100000;
		ttwalktime=300000;
		tttraveltime=6000000000000;
		bestLastSol = new TPHYPERPATHELEM;
		bestLastSol->cost = POS_INF_FLOAT;	


		numaOfHyperpath = 0;
		curIter = 0;
		InnerIters = 0;
		IterMainlooptime = 0;
		IterInnerlooptime = 0;
		netTTwaitcost = 0;
		max_w = 0;
		flowPrecision = 1e-10;
		m_capacity = 30;
		inputStabilizerAddedStops = 0;
		inputStabilizerDroppedRoutes = 0;
		inputStabilizerDroppedShapeRows = 0;
		inputStabilizerDroppedShapes = 0;
		inputStabilizerRebuiltTransitRows = 0;
		inputStabilizerDroppedTransitRows = 0;
		inputStabilizerDroppedWalkRows = 0;
		inputStabilizerDroppedTripRows = 0;
		inputStabilizerMergedTripRows = 0;
		lastAonCpuTimeSeconds = 0.0;
		writeCsvResults = true;
		lastErrorMessage.clear();

		m_walkNum = 0;
		m_maxWalkTime = 5.0; //5 minutes. 
		m_alightLoss  = 0.1; //0.1 minute
		RGapIndicator = 1.0;
		tempRGapIndicator = 1.0;
		buffer =NULL;

		m_symLinks = false; 
	};

	typedef enum {
     /* Naming conventions:
	 PCTAE_{A/P/B}_* = arc-based/hyperpath-based/strategic bush-based respectively
	*/
     PCTAE_A_SD, //Simplicial decomposition algorithm 
	 PCTAE_A_GFW,//General frank-wolf algorithm 
	 PCTAE_A_MSA,//MSA method, step size = 1 / numer of iters
	 PCTAE_A_MSA_eff,//MSA for effective frequency,step size = 1 / numer of iters
	 PCTAE_A_MSA_eff_v2,
	 PCTAE_A_GFW_TP,//use topological

	 PCTAE_P_AON,// all or nothing
	 PCTAE_P_iGreedy, // 'i' mean with innerloops
	 PCTAE_P_iGreedy_TP,
	 PCTAE_P_Greedy,
	 PCTAE_P_NGP,	// gradient proj with new VI formula
	 PCTAE_P_iNGP,
	 PCTAE_P_iNGP_TP,
	 PCTAE_P_GP_eff,//GP for effective frequency
	 PCTAE_P_GP_eff_without_GSM,//GP for effective frequency without GSM

	 PCTAE_B_DSB,	// strategic bush-based algorithm, following dial

	 CAP_PCTAE_B_TL,		// Capacited that solved in bush structure with Method of mulplier，capacity imposed on transit link(TL)
	 CAP_PCTAE_P_Greedy_TL,		// Capacited that solved in hyperpath-based formulation with Method of mulplier，capacity imposed on transit link
	 CAP_PCTAE_P_GP_TL, //capacity imposed on transit link
	 CAP_PCTAE_P_GP_eff_TL, //capacity imposed on transit link for effective frequency（path alg）
	 CAP_PCTAE_A_MSA_eff_TL, //capacity imposed on transit link for effective frequency（arc alg：MSA）
	 CAP_PCTAE_A_MSA_DCL_eff_TL, //capacity imposed on transit link for effective frequency（arc alg：MSA_DCL）

	 CAP_PCTAE_B_BL, // capacity is imposed on the boarding link (BL)
	 CAP_PCTAE_P_Greedy_BL,
	 CAP_PCTAE_P_GP_BL,
	 CAP_PCTAE_P_Greedy, // capacity constriant imposed on boarding links by the associated transit and alight link


	 PCTAE_B_Path // add for path-based algorithm to solve TEAP





	} PCTAE_algorithm; 

	
	typedef enum {
		FCTAE_A_MSA,//it doesn't work in the space of link flow
		FCTAE_B_MSA,
		FCTAE_B_HYPERPATH
	}FCTAE_algorithm; 


	int                special_num;
	void               ReleaseMLElemMap();
	TPHYPERPATHELEM*   bestLastSol;
	int                tempcountc; 
	int                tempuse;
	int                DominanceCheck(PTNode*tail,TPHYPERPATHELEM* &tElem,TPHYPERPATHELEM* pElem,int curTrans,PTLink* link,floatType curwalkcost);
	int                UpdateLabel(TPHYPERPATHELEM* tElem,TPHYPERPATHELEM* pElem);
	int                ReInsertLabelML(TPHYPERPATHELEM* tElem,TPHYPERPATHELEM* pElem,multimap<double, TPHYPERPATHELEM*,less<double>> &Q,double hvalue = 0.0);// for LS algorithm
	int                EraseLabelML(TPHYPERPATHELEM* tElem,multimap<double, TPHYPERPATHELEM*,less<double>> &Q,double hvalue = 0.0);// for LS algorithm
	int                UpdateLabelLSML(PTNode* tailNode,PTLink* link,TPHYPERPATHELEM* &tElem,TPHYPERPATHELEM* pElem,multimap<double, TPHYPERPATHELEM*,less<double>> &Q_now,multimap<double, TPHYPERPATHELEM*,less<double>> &Q_next,double hvalue = 0.0);
	int                ClearQNow(multimap<double, TPHYPERPATHELEM*,less<double>> &Q_now);
	int                GetAttSetAndStateMLMMEAP(PTNode* tnode,map<int,TPHYPERPATHELEM*> links, TPHYPERPATHELEM* tempElem,TPHYPERPATHELEM* hElem);
	TPHYPERPATHELEM*   GetMaxPreferredLabel(PTNode*tail,TPHYPERPATHELEM* pElem,int curTrans);
	void               PrintCurNodeLable(TPHYPERPATHELEM* curLabel);
	int				   count;
	///these are IO related to Transit assignment problem.
	int  ReadTEAPStops(bool ispos=false);
	int  ReadTEAPRoutes();
	int  ReadTEAPShapes();
	int  ReadTEAPODdemand();
	int  ReadTEAPODdemandTP();
	int  CreateTEAPNodeLinks();
	int  CreateTEAPNodeLinksTP();
	int  ReadTEAPTransitData();
	int  CreateTEAPWalks(bool Traj = false);
	int  CreateTEAPWalksTP(bool Traj = false);
	int  CreateTransferlink();
	int  ReadODWALK();//add for capacitated problem for feasibility
	int  ReadPostgresInputs(
		int projectId,
		int userId,
		int caseId,
		const PTPostgresConfig& config);
	int  ReadCsvInputs(
		const string& inputDirectory,
		int projectId,
		int userId,
		int caseId);
	int  WritePostgresResults(
		int projectId,
		int userId,
		int caseId,
		const PTPostgresConfig& config,
		double cpuTimeSeconds);
	int  StabilizePostgresInputs();
	int  BuildFromPostgresInputs();
	void SetProjection(bool con) {m_IsPorjected=con;} 
	void ReprojectNodeCoordinates();
	void GetVicinityNodes(COORDMAP &xmap, COORDMAP &ymap, vector<PTNode *> &nvec, PTNode *node, long threshold);
	


	void UpdateNodeNum() {numOfNode = nodeVector.size();}
	void UpdateLinkNum() {numOfLink = linkVector.size();}

	void PrintNetLinks()//print link的详细信息，包括有效发车频率
	{
		for (int i = 0; i<numOfLink; i++)
		{
			PTLink* link = linkVector[i];
			cout<<"link id:"<<link->id<<", tailnode:"<<link->tail->id<<",head:"<<link->head->id<<", flow:"<<link->volume<<",cost:"<<link->cost<<",eff:"<<link->efffreq<<endl;
		}
	};

	void PrintNetStgs()
	{
		PTDestination* dest;
		for (int i = 0;i<numOfPTDest;i++)
		{
			dest = PTDestVector[i];
			cout<<"stgs for dest:"<<dest->destination->id<<endl;
			dest->MarkStgLinksOnNet();
			// zero stg flows
			for(vector<GNODE *>::iterator po = dest->tplNodeVec.begin(); po != dest->tplNodeVec.end(); po++)
			{
				GNODE* gnode = *po;
				for (vector<StgLinks*>::iterator pt = gnode->m_StgsVec.begin();pt != gnode->m_StgsVec.end();pt++)
				{
					StgLinks* stg = *pt;
					cout<<"tail node:"<<gnode->m_ptnodePtr->id<<",stg:"<<stg->sname<<",waitcost:"<<stg->waitT<<",flow:"<<stg->sflow<<endl;

					for (int j = 0;j<stg->StgLinkPosVec.size();j++)
					{
						int ix = stg->StgLinkPosVec[j];			
						gnode->m_ptnodePtr->forwStar[ix]->buffer[3] += stg->StgLinkProbVec[j]*stg->sflow;
						cout<<"\t link:"<<gnode->m_ptnodePtr->forwStar[ix]->id<<",prob:"<<stg->StgLinkProbVec[j]<<endl;
					}

				}
			}
			dest->RemarkStgLinksOnNet();
		}
	
	}



	PTLink*	CatchLinkPtr(PTNode* tail, PTNode *head)
	{
		for (vector<PTLink*>::iterator pl = tail->forwStar.begin(); pl!=tail->forwStar.end(); pl++)
		{
			if((*pl))
			{
				if((*pl)->head == head)
					return *pl;
			}
		}
		return NULL;
	
	};

	int				    nodeBufferSize;
	int			        linkBufferSize;
	int				    pathBufferSize;
	int				    destBufferSize;
	int					transfer_times;//换乘次数
	floatType			ttwalktime;//总步行时间限制
	floatType			tttraveltime;//总出行时间限制
	int					AllocateLinkBuffer(int size);
	int					AllocateNodeBuffer(int size);
	int				    AllocateNetBuffer(int size);

	PTShape* GetShapePtr(const string &id)
	{	
		PTShapeMapIter si = m_shapes.find(id);
		if(si == m_shapes.end()) return NULL;
		else                     return si->second;
	};
    PTRoute* GetRoutePtr(const string &id)
	{
		PTRouteMapIter si = m_routes.find(id);
		if(si == m_routes.end()) return NULL;
		else                     return si->second;
	};
    PTStop*  GetStopPtr(const string &id)
	{
		PTStopMapIter si = m_stops.find(id);
		if(si == m_stops.end()) return NULL;
		else                     return si->second;
	};

	PTLink*	CatchLinkPtr(int id)
	{
		vector<PTLink*>::iterator pl;
		pl = find_if(linkVector.begin(), linkVector.end(), predP(&PTLink::id_, id));
		if(pl == linkVector.end()) return NULL;
		else                         return *pl;
	
	};

	PTNode*	CatchNodePtr(int id)
	{
		vector<PTNode*>::iterator pv;
		pv = find_if(nodeVector.begin(), nodeVector.end(), predP(&PTNode::id_, id));
		if(pv == nodeVector.end()) return NULL;
		else                         return *pv;
	}

	PTNode*	CatchNodePtrbyTrafficid(int id)
	{
		vector<PTNode*>::iterator pv;
		pv = find_if(nodeVector.begin(), nodeVector.end(), predP(&PTNode::trafficid_, id));
		if(pv == nodeVector.end()) return NULL;
		else                         return *pv;
	}

	PTDestination*	CatchDestPtr(int id)
	{
		vector<PTDestination*>::iterator pv;
		pv = find_if(PTDestVector.begin(), PTDestVector.end(), predP(&PTDestination::id_, id));
		if(pv == PTDestVector.end()) return NULL;
		else                         return *pv;
	}


	void							PCTAE_SetAlgorithm(PCTAE_algorithm algorithm){PCTAE_ALG = algorithm;};
	void							FCTAE_SetAlgorithm(FCTAE_algorithm algorithm){FCTAE_ALG = algorithm;};
	string							GetAlgorithmName();
	int								PCTAE_Solver(PCTAE_algorithm algorithm);//partial congested model
	int								FCTAE_Solver(FCTAE_algorithm alg);//full congested model
	void							SetConv(floatType e){convCriterion=e;};
	void							SetMaxIter(int i){maxMainIter = i;};
	void							SetMaxIterTime(floatType i){maxIterTime = i;};
	void							SetInnerConv(floatType c = 0.001) {m_innerConv = c;}
	void							Settimescaler(floatType i){timescaler = i;};
	void							SetWriteCsvResults(bool enabled){writeCsvResults = enabled;};
	void							SetSymLinkType(bool a) {m_symLinks  = a;};
	inline  bool					ReachAccuracy(double g) {return g<=convCriterion;}
	inline  bool					ReachMaxIter()  {return curIter>=maxMainIter;} //test if maximum iteration is attained
	inline  bool					ReachMaxTime(floatType t) {return t>=maxIterTime;}
	void							RecordTEAPCurrentIter();
	void							ReportIter();
	void							ReportCapIter_V2();//输出迭代过程中的信息（考虑容量约束的有效发车频率问题）
	void							ReportCapeffIter();//输出迭代过程中的信息（考虑容量约束的有效发车频率问题）
	void							ReportPTlinkflow();
	void							ReportAONCsvResults(double cpuTimeSeconds);
	void							ReportCapeffPTlinkflow();//输出plink信息（考虑容量约束的有效发车频率问题）
	void							ReportPTHyperpaths();
	void							ReportCapeffPTHyperpaths();//输出hyperpath信息（考虑容量约束的有效发车频率问题）
	void							ReportODPath();//输出hyperpath信息（考虑容量约束的有效发车频率问题）
	void							ReportPTHyperpathsTP();
	void							ReportLinkCap();
	void							ReportPTStrategies();

	// output functions for SZ project
	void							SZ_OutputOnOffLinkFlow();
	void							SZ_OutputTransitLinkFlow();
	void							SZ_OutputWALKLinkFlow();
	void							SZ_OutputHyperpathFlow();
	//input functions
	int								SZ_BuildAN();
	int								SZ_ReadStop();
	int								SZ_ReadRoute();
	int								SZ_ReadShape();
	int								SZ_ReadWalk();
	int								SZ_ReadTEAPTransitData();
	int								SZ_ReadStopTrip();
	int								SZ_ReadNodeTrip();
	//======Functions to solve transit assignment problem//
	int								BuildAN(bool stopPos=false /*whether has gis pos*/, bool walkfile = false,bool isTP=false);//build network for assginment 
	int								GenerateRandGridAN(GRIDPTNETPAR par,string filepath);// build grid network
	void							GenerateRandGripTrip(GRIDPTNETPAR par);// write trip info, called after net has built
	void							UpatePTNetworkLinkCost();
	void							UpatePTNetworkLinkCost_for_test();
	void							ConnectAsymmetricLinks();
	void							SetLinksAttribute(bool isSym);
	PTDestination*					CreatePTDestination(int nid, int noo);//node id, number of origins 
	PTDestination*					CreatePTDestination(PTNode* rootDest, int noo);//node id, number of origins 
	int								InitializeHyperpathLC(PTNode* rootDest);
	int								InitializeHyperpathLS(PTNode* rootDest);
	int								Topological(PTNode* rootdest);
	int								InitializeHyperpathLS_TP(PTNode* rootdest);
	int								InitializeHyperpathLS_TP2(PTNode* rootdest);
	int								Topological_2(PTNode* rootdest);
	int								InitializeHyperpathLS_1(PTNode* rootOrg,PTNode* rootDest);
	int								Swap_stl(multimap<double,PTNode*,less<double>>,multimap<double,PTNode*,less<double>>);

	StgLinks*						GenerateNonBoardingStg(PTLink* link);						
	StgLinks*						GenerateBoardingStg(multimap<double, PTLink*,less<double>> plines);			
	StgLinks*						GenerateBoardingStg(multimap<double, int,less<double>> linkids);			
	StgLinks*						GenerateBoardingStg(vector<PTLink*> links);		
	void							GenerateBoardingStg(vector<PTLink*> links, vector<PTLink*> &attractivelinks,floatType &ec/*expected cost*/,floatType &ew/*expected waiting delays*/);
	
	
	void							GenerateEffBoardingStg(vector<PTLink*> links, vector<PTLink*> &attractivelinks,floatType &ec/*expected cost*/,floatType &ew/*expected waiting delays*/);
	StgLinks*						GenerateEffNonBoardingStg(PTLink* link);				

	void							ComputeConvGap();
	void							ComputeConvGap_TP();
	void							ComputeConvGap_eff();


	
	clock_t							m_startRunTime; //this is the time when SolveTAP just called.
	
	void							GenerateSPP();//Generate shortest path 
	void							GenerateODWalkFile();							
	//=======================Method of mulplier for path-based algorithm for TEAP
	void							SolvePathBushTEAP();
	void							InitialPathBushSubNet();
	void							UpdateLCShortestPath(PTNode* dest);
	void							UpdateLCGeneralShortestPath(PTNode* dest);
	void							BushFlowShift(PTDestination* dest);
	void							AggregateDestFlows(PTDestination* dest); // this is very important before trim operation, mainly because of precision errors 
	void							ComputeGAPFunc();
	void							ComputeGAPFuncII();
	void							ComputeGAPFuncIII();
	//=================Methods for effective frequency-based TEAP
	int					SolveFTEAP_A_MSA();
	void				UpdateNetworkPath_gcost();
	void				UpdateEffNetLinkCost();
	void				UpdateEffFrequency();
	int  				EffAllOrNothing();
	int					InitializeEffHyperpathLS(PTNode* rootDest);
	
	int					SolveFTEAP_B_MSA();
	void				EffStgNetInitialize();
	void				EffBushAllOrNothing();//all-or-nothing assignment
	void				SaveBushStgFlow(int col=1);
	void				SaveBushLinkFlow(int col = 1);

	void				UpdateLinkFlowSolution(int col = 1);
	void				UpdateBushLinkFlowSolution(int col = 1);
	


	int					SolveFTEAP_B_HYPERPATH();
	void				UpdateSubNetStgFlow(); //given destination-based link flow, update stg flow
	void				ExpandStgSubNet();



	void				ComputeEffConvGap();
	void				ComputeEffGAPFunc();
	//=======================Method of mulplier for capacited TEAP in bush 
	int					SolveCapacityBush();
	void				CapacityBushSubProblem();
	void				IniTransitLinkMultiplier();
	void				IniTransitLinkMultiplier_V2();//for capacited eff_hyperpath alg
	void				IniBoardLinkMultiplier();
	void				IniCapacityPars();
	void				ComputeCapSubConvGap();
	void				ComputeCapeffSubConvGap();//针对有效发车频率，计算带容量约束问题的RGP
	void				UpdatePTNetGeneralCost_Dercost();
	int					InitializeGeneralHyperpathLS(PTNode* rootDest);
	int					InitializeGeneraleffHyperpathLS(PTNode* rootDest);//使用general_cost为有效发车频率问题求解最短超路径
	floatType			OBCapStgFlowShift(PTDestination* dest);
	void				CapAggregateDestStgFlows(PTDestination* dest);
	void				UpdateMultiplier();
	void				UpdateBushPenalty();
	floatType			ComputeNormFlowInfeasibility(bool islastflow);
	floatType			ComputeNormSolution();
	floatType			ComputeError();
	void				SetCurrentSolution();
	void				PrintCapTransitLinkFlow();
	floatType			ComputeBoundGAP();
	void				RecordCapTEAPCurrentIter(float GAP, float IF, float SN);
	void				ReportCapIter();

	//=======================Method of dual Lagrangian methods

	ofstream			fileout;
	int					SolveCapacity_EffectiveMSA_DCL();//求解考虑有效发车频率的容量约束问题（arc alg：DCL）
	//void				Capacityeff_DCLProblem();//利用dual cutting-plane算法求解带容量约束的问题
	//void				CapacityeffMasterProblem_DCL();//构造dual cutting-plane的主问题
	void				Updatelinkgcost();//更新link的general_cost
	vector<PTLink*>		transitlinkVector;
	void				compute_maxw_DCL();//在DCL问题中利用volumeset计算max_w


	void				ClearLinkVec()
	{
		for (int i = 0; i < numOfLink; i++)
		{
			PTLink* link = linkVector[i];
			link->Iter_volumeset.clear();
			link->Vec_volumeset.clear();
			link->Iter_transitvolumeset.clear();
		}
	}

	void				Compute_Tsum()
	{
		TSUM = 0.0;
		for (int i = 0; i < numOfLink; i++)
		{
			PTLink* link = linkVector[i];
			TSUM += link->volume * link->cost;//（t_a * v_a）
		}
	}

	void				AssignDemandBlink();//将OD对的需求加载到boarding_link中

	void				ComputeCapeffSubConvGap_DCL();

	int  				PTAllOrNothing_Capeff_forDCL();//针对有效发车频率，利用general_cost搜索最短路，并加载上去
	int  				PTAllOrNothing_Capeff_forDCL(PTDestination* dest);//针对一个dest下的有效发车频率问题，利用general_cost搜索最短路，并加载上去

	map<int, PTDestination*>    node2dest;//建立dest->id（dest->node）与dest之间的关系（在build中使用）
	floatType			obj_value;//当前目标函数值
	floatType			TSUM;//当前迭代的TSUM
	floatType			WSUM;//当前迭代的WSUM
	floatType			Total_SUM;//TSUM+WSUM
	vector<floatType>	Lagrange_Vec;//记录每个约束对应乘子
	int					total_iter;//dual cutting-plane algorithm的总迭代次数
	vector<floatType>	T_sum;//sum_(t_a * v_a)
	vector<floatType>	W_sum;//sum_(W_i^d)





	//=======================Method of mulplier for capacited TEAP in hyperpath 
	bool                if_Multiplier_wrong;//判断乘子问题中，乘子的更新是否正确？（不超容link对应的乘子为0）
	bool                if_use_linesearch;
	bool				InitialCapHyperpathNetFlow();
	int					SolveCapacityHyperpath();
	int					SolveCapacity_EffectiveMSA();//求解考虑有效发车频率的容量约束问题（arc alg：MSA）
	int					SolveCapacity_EffectiveHyperpath();//求解考虑有效发车频率的容量约束问题（path alg）
	void				CapacityHyperpathSubProblem();
	void				CapacityeffHyperpathSubProblem();//求解考虑有效发车频率的无容量约束子问题（path alg）
	void				CapacityeffMSASubProblem();//求解考虑有效发车频率的无容量约束子问题（arc alg：MSA）
	void				CapacityeffMSASubProblem_first();//求解考虑有效发车频率的无容量约束子问题（arc alg：MSA）
	void				UpdateCapHyperPathGreedyFlow(PTDestination* dest,PTOrg* org);// constraint on transit link (TL)
	void				UpdateCapHyperpathGPFlow(PTDestination* dest,PTOrg* org);
	void				UpdateCapHyperpathGPFlow_linesearch(PTDestination* dest,PTOrg* org);//使用linesearch法为Capacited eff problem转移流量
	void				BL_UpdateCapHyperpathGPFlow(PTDestination* dest,PTOrg* org);
	void				HyperpathCapInnerLoop(int maxiters = 200);
	void				HyperpathCapeffInnerLoop(int maxiters = 200);//考虑有效发车频率的子循环问题求解

	void				CheckWalkingHyperPath();
	void				CreateODLink();
	// initialize a feasible solution
	void				ZeroTransitLinkFlow();
	void				EnlargeLineCapacityII();//enlarge capacity on transit links
	void				EnlargeLineCapacity();//enlarge capacity on boarding links
	void				EnlargeLineCapacityIII();//enlarge the capacity once detecting a block path 

	floatType			AssignCapFeasibleFlow(PTDestination* dest,PTOrg* org);
	bool				InitializeFeasiblePathLS(PTNode* rootDest);
	void				UpdateLinkCapacity();


	//parameters
	int					number_of_infeasible_arcs;//每次迭代中不可行link的数量
	floatType			cb_xi;		//penalty enlarge parameter, recommended as [2,10] (Bertsekas)
	floatType			cb_sigma;	// infeasible flow reduction criteria, recommended as 0.25 (Bertsekas), 0.7 (Feng), 0.8 (Nie)
	floatType			cb_lambda;	// penalty coefficient
	long double			infeasibleflow;	// penalty coefficient

	floatType			SubRG; //relative gap in sub-problem
	long double			SubRGCriterion;
	int					SubIter;
	int					MaxSubIter;

	floatType			MaxFlowInfeasibility;


	//=======================Strategic bush-based methods
	int					SolveBushTEAP();// all at once: firstly, for each bush, improve it; secondly, for each bush, shift flow
	void				StgNetInitialize();//stg all-or-nothing assignment
	floatType			OBStgFlowShift(PTDestination* dest);
	floatType			eOBStgFlowShift(PTDestination* dest);
	void				AggregateDestStgFlows(PTDestination* dest); // this is very important before trim operation, mainly because of precision errors 
	void				ExportStgUEsolution();
	bool				ImportStgUESolution();

	void				ExportStgUEsolutionII();
	bool				ImportStgUESolutionII();

	void				InitialSubnetStgNode(); //create stg node for each subnetwork 
	StgLinks*			GenerateStgByName(PTNode* tnode,string name);

	//=======================fw methods
	int					SolveFWTEAP();
	int					SolveFWTEAP_TP();
	int  				PTAllOrNothing();
	int					PTAllOrNothing_TP();
	int  				PTAllOrNothing(PTDestination* dest);
	int					PTAllOrNothing_TP(PTDestination* dest);

	

	void				SaveLinkWaitVariables(int col = 1);
	int                 BisecSearch(floatType maxStep = 1.0);  //line search: bisection
	floatType			ComputePTDz(); //overload computeDz for bisec line search
	void				NewSolution(int col);        //volume = buffer[col - 1] + (volume - buffer[col - 1])*stepSize ; flow[ll] = flow[ll] + alpha * (y_flow[ll] - flow[ll])

	//===================hyperpath-based methods
	int					SolvePathTEAP_AON();
	int					SolvePathTEAP();
	int					SolvePathTEAP_TP();
	bool				InitialHyperpathNetFlow();
	bool				InitialHyperpathNetFlow_TP();
	void				ColumnGeneration(PTDestination* dest,PTOrg* org);
	void				UpdateHyperPathGreedyFlow(PTDestination* dest,PTOrg* org);
	void				UpdateHyperPathGreedyFlowTP(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathGPFlow(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathGPFlow_original(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathGPFlowline(PTDestination* dest,PTOrg* org);

	void				OD_FlowLoading_TESTV2(PTDestination* dest, PTOrg* org);
	void				OD_FlowLoading(PTDestination* dest, PTOrg* org);
	void				OD_FlowLoading_TEST_EXCEED(PTDestination* dest, PTOrg* org);
	void				OD_FlowLoading_TESTV1(PTDestination* dest, PTOrg* org);

	void				UpdateHyperpathFlow_GPandBBstepsize_TEST(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathFlow_BBstepsize(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathFlow_Linesearch(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathFlow_GPandBBstepsize(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathGPFlowlinesearch_TESTEXCEED(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathGPFlowlinesearch(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathGPFlowlinesearch_TESTRATIO(PTDestination* dest, PTOrg* org,floatType ratio);
	void				UpdateHyperpathGPFlowlinesearch_TEST(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathGPFlowlinesearch_TEST_V2(PTDestination* dest, PTOrg* org);
	void				UpdateHyperpathGPFlowlinesearchv2(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathGPFlowlinesearchv3(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathGPFlowTP(PTDestination* dest,PTOrg* org);
	void				UpdateHyperpathLineSearchFlow(PTDestination* dest,PTOrg* org);
	void				HyperpathInnerLoop(int maxiters = 200);
	
	void				HyperpathInnerLoopTP(int maxiters = 200);
	void				FlowReAssignment();

	void				ExportHyperpathUEsolution();
	bool				ImportHyperpathUESolution();
	//=======================effective frequency=========================
	int					SolveMSATEAP();
	int					SolveMSAeffTEAP();
	int					SolveMSAeffTEAP_v2();
	void				InitialForFREQUENCY();
	void				UpdatePTNetworkLinkFrequency();
	//void				RecordlinkPrevolume();
	void				ComputeConvGap_Frequency();
	void				compute_first_iter_maxw();
	void				compute_maxw();
	void				compute_maxwV2();
	void				InitalMSA();
	void				InitalGP();
	int					SolvePatheffTEAP();
	int					SolvePatheffTEAPv2();
	int					SolvePatheffTEAP_v3();//按照超路径的概率加载流量，加载后更新超路径的概率，重新计算费用
	int					SolvePatheffTEAPV4();
	bool				InitialHyperpathNetFlow_eff();
	bool				InitialODdemand_on_walklink();
	bool				InitialHyperpathNetFlow_effv2();
	bool				InitialHyperpathNetFlow_effv3();//按照超路径的概率加载流量，加载后更新超路径的概率，重新计算费用
	void				UpdateHyperpathGPFlow_eff(PTDestination* dest,PTOrg* org);
	void				sum_volume();
	void				Saveoldvolume();
	void				computeMax_wForGP();//这里是为了计算max{volume_arc^dest / eff_freq}
	void				HyperpathInnerLoop_eff(int maxiters = 200);
	void				HyperpathInnerLoop_effv2(int maxiters = 200);
	void				HyperpathInnerLoop_effv3(int maxiters = 200);
	void				Path_innerloop(int maxiters = 200);
	int  				PTAllOrNothing_eff();
	int  				PTAllOrNothing_Capeff();//针对有效发车频率，利用general_cost搜索最短路，并加载上去
	int  				PTAllOrNothing_eff(PTDestination* dest);
	int  				PTAllOrNothing_Capeff(PTDestination* dest);//针对一个dest下的有效发车频率问题，利用general_cost搜索最短路，并加载上去
	int  				PTAllOrNothing_effv2();
	int  				PTAllOrNothing_effv2(PTDestination* dest);
	int					InitializeHyperpathLS_eff(PTNode* rootDest);
	void				GenerateBoardingStg_eff(vector<PTLink*> links, vector<PTLink*> &attractivelinks,floatType &ec/*expected cost*/,floatType &ew/*expected waiting delays*/);
	void				ComputeConvGap_MSAeff();
	void				ComputeConvGap_MSACapeff();

	floatType			linesearch_time;
	int					linesearch_count;
	floatType			GSMtime;
	floatType			tt_innerlopptime;
	floatType			tt_linesearchtime;
	floatType			tt_lstime;
	floatType			tt_mainlooptime;
	floatType			tt_computegaptime;
	floatType			TEAP_RATIO;
	bool				if_RGP_down;
	bool                if_linesearch;

	//==================simplicial decomposition method
	int					SolveSDTEAP();//SD algorithm with first-order approximation
	void				SDInitializedCH();// initialize convex hull to get two extreme solution
	bool				SDAddCurrentSolution2CH();
	void				SDMasterProblem();
	vector<floatType*>	c_h;//convex hull set, only called in SD-type algorithms
	floatType			SDProjGap;// the gap criterion in SD master problem

	string							networkName;
	PCTAE_algorithm					PCTAE_ALG;  
	FCTAE_algorithm					FCTAE_ALG;
	vector<PTNode*>					nodeVector;
	vector<PTLink*>					linkVector;
	vector<PTLink*>					OD_walklinkVector;
	int								numOfNode;
	int								numOfLink;
	vector<PTDestination*>			PTDestVector;
	int								numOfPTDest;
	int								numOfPTOD;		// number of O-D pairs
	floatType						numOfPTTrips;		// number of O-D pairs
	int								numaOfHyperpath;// number of hyperpaths
	int								numaOfShift;// number of hyperpaths
	int								m_walkNum;
    double							m_maxWalkTime; /*in minutes*/
    double							m_alightLoss;
	floatType						m_capacity;
	bool							m_symLinks;//add to repsent whether a link has a separable cost function
	floatType						m_innerConv;
	floatType						RGapIndicator; //convergence indicator
	floatType						tempRGapIndicator; 
	floatType						convCriterion; // convergence gap
	floatType						GapIndicator; // absolute gap indicator
	floatType						IterInnerlooptime;//time for each inner loop
	floatType						IterMainlooptime;//time for each inner loop
	int								InnerIters;
	floatType						maxIterTime;   //maximum allowed iteration time in min
	int								maxMainIter;   //maximum allowed iteration number
	floatType						timescaler;
	int								curIter;       //current iteration
	vector<PTITERELEM*>				iterRecord;		//a vector record iteration history
	floatType						stepSize;      // current step size
	floatType						*buffer;    //this data is added as a working space for outside using.
	floatType						netTTwaitcost; //this data is added as the network total waiting cost
	floatType						max_w;//for MSA eff problem
	floatType						temp_max_w;//for MSA eff problem
	floatType						SCnetTTwaitcost;//simplex combination waiting cost, only called in SD
	floatType						flowPrecision;
	//floatType						tt;
	floatType						lastTT_eff;


	PTStopMap						m_stops;
    PTRouteMap						m_routes;
    PTShapeMap						m_shapes;
	string							inputdir;

	

	bool							m_IsPorjected;// indicate if the coordinate has been projection or not
	//CSHPInterface					m_geoIF; //geo interface. initialized in ReadPTNode; check if average stop x is between -180 and 180.


	vector<PTStop*>					PTstopvec;
	vector<PTDbRouteRecord>			dbRouteRecords;
	vector<PTDbStopRecord>			dbStopRecords;
	vector<PTDbShapeStopRecord>		dbShapeStopRecords;
	vector<PTDbTransitRecord>		dbTransitRecords;
	vector<PTDbWalkRecord>			dbWalkRecords;
	vector<PTDbTripRecord>			dbTripRecords;
	vector<PTDbWayRecord>			dbWayRecords;
	vector<string>					inputStabilizerLogs;
	int								inputStabilizerAddedStops;
	int								inputStabilizerDroppedRoutes;
	int								inputStabilizerDroppedShapeRows;
	int								inputStabilizerDroppedShapes;
	int								inputStabilizerRebuiltTransitRows;
	int								inputStabilizerDroppedTransitRows;
	int								inputStabilizerDroppedWalkRows;
	int								inputStabilizerDroppedTripRows;
	int								inputStabilizerMergedTripRows;
	double							lastAonCpuTimeSeconds;
	bool							writeCsvResults;
	string							lastErrorMessage;
};


