#ifndef TNM_ALGORITHM_H
#define TNM_ALGORITHM_H

#include <string>
#include "mat_header.h"
using namespace std;

#ifdef _WIN32
#define TNM_CDECL __cdecl
#else
#define TNM_CDECL
#endif
typedef void (TNM_CDECL *TNM_ProgressCallback)(int, double);

#ifdef __cplusplus
TNM_EXT_CLASS void TNM_ResetLastError();
TNM_EXT_CLASS void TNM_SetLastError(const string& message);
TNM_EXT_CLASS const string& TNM_GetLastError();

// 预检查/提示信息累积与读取（用于拼接到最终 JSON message）
TNM_EXT_CLASS void TNM_ResetMessageNotes();
TNM_EXT_CLASS void TNM_AppendMessageNote(const string& note);
TNM_EXT_CLASS const string& TNM_GetMessageNotes();

// OFW 不可达 OD 对 warnings 示例条数（0 表示仅输出汇总不列示例）
TNM_EXT_CLASS void TNM_SetOfwUnreachableWarningExamples(int n);
#endif

#define TNM_DEFAULT_ROLE "user_project"
#define TNM_NETWORK_TABLE_SUFFIX "road_way"
#define TNM_OD_TABLE_SUFFIX "other_od"
#define TNM_OBSERVED_TABLE_SUFFIX "other_observation"

inline string TNM_ComposeTableName(const string& role, const char* suffix)
{
        string prefix = role.empty() ? string(TNM_DEFAULT_ROLE) : role;
        string tail = (suffix == NULL) ? string() : string(suffix);
        return string("\"") + prefix + "\".\"" + tail + "\"";
}

inline string TNM_ComposeTableName(const string& role, const string& suffix)
{
        return TNM_ComposeTableName(role, suffix.c_str());
}

inline string TNM_DefaultNetworkTable(const string& role)
{
        return TNM_ComposeTableName(role, TNM_NETWORK_TABLE_SUFFIX);
}

inline string TNM_DefaultODTable(const string& role)
{
        return TNM_ComposeTableName(role, TNM_OD_TABLE_SUFFIX);
}

inline string TNM_DefaultObservedTable(const string& role)
{
        return TNM_ComposeTableName(role, TNM_OBSERVED_TABLE_SUFFIX);
}

class TNM_EXT_CLASS TNM_TAP
{
public:
	TNM_TAP();
	~TNM_TAP();

	string            inFileName; //this is a string that normally attached with input and output of the object-related information.
	string            outFileName;
	TNM_SNET*		  network;
	TNM_LINKTYPE	  lpf;     //this is to provide the user an interface to determine the type of the links on the network
	                           //some build functions in network class, like buildfort, builddanet2, may need this.
	floatType		  costScalar;//
	floatType         OFV; //objective function value;
	int               curIter;       //current iteration
	float             cpuTime;       //used to stored the total time of solving
	TERMFLAGS         termFlag;   //termination Flag;
	TNM_SPATH*		  yPath;
	static int        intWidth;
	static int        floatWidth; 
	bool              reportIterHistory; //determine if the Iteration history will be retained.
	bool			  reportLinkDetail;
	bool			  reportPathDetail;

private:
	bool              m_needRebuild;
protected:
	clock_t           m_startRunTime; //this is the time when SolveRec() just called.
	//bool              m_storeResult; 
	int               numLineSearch;
	MATOBJID          objectID;  //this is an unique id to tell who is this class.
	int               maxMainIter;  //maximum allowed iteration number
	int				  m_maxInnerIter;
	//float             m_maxCPUTime; // in hours;
	floatType         convIndicator; //convergence indicator
	floatType         convCriterion; // convergence criterion
	floatType		  m_innerConv; 
	floatType         stepSize;      // current step size
	vector<ITERELEM*> iterRecord; //a vector record iteration history
	int               maxLineSearchIter;   //maxallowed line search iteration
	TNM_TOLLTYPE	  m_tlType;  //tolltype: determine equilibrium type
	bool              m_resetNetworkOnSolve;
	ofstream          iteFile;    //iteration history.
	ofstream		  lfpFile;// link detail
	ofstream		  pthFile;//path detail
	string				m_dbConnStr;
	TNM_ProgressCallback m_progressCb; // 进度回调函数
	bool              m_progressWindowActive;
	int               m_progressWindowIterHint;
	double            m_progressWindowStart;
	double            m_progressWindowEnd;
/*========================================================================================
	I-O methods.
  ========================================================================================*/
public:
	void          SetLPF(TNM_LINKTYPE l)
	{
		if(l!=lpf) PostRebuild();
		lpf = l;
	}
	void			  SetCostScalar(floatType c)
	{
			if(c!=costScalar) PostRebuild();
			costScalar = c;
	}
	void              PostRebuild(bool r = true) {m_needRebuild = r;}
	//bool              IsStoreResult() {return m_storeResult;}
	inline  bool      ReachAccuracy() {return convIndicator<=convCriterion;} //test if required accuracy is attained.
	inline  bool      ReachMaxIter() {return curIter>=maxMainIter;} //test if maximum iteration is attained
	inline  bool      ReachError() {return termFlag == ErrorTerm;} //test if termFlat is set to error, which could happen in the process of Solve function
	inline  bool      ReachUser() {return termFlag == UserTerm;}
	//inline  bool      ReachMaxCPU() {if(watchTime) return GetElapsedTime() > m_maxCPUTime * HR2SEC; else return false;}
	void              SetConv(floatType);
	void			  SetInnerConv(floatType c = 0.001) {m_innerConv = c;}
	void              SetMaxLsIter(int);
	void			  SetMaxInnerIter(floatType i = 15) {m_maxInnerIter = i;}
	void              SetMaxIter(int);
	int				  SetTollType(TNM_TOLLTYPE tl);
	void              SetCentroidsBlocked(bool cb) {
		network->centroids_blocked=cb;
		if(network->centroids_blocked)
		{
			for(int i=0;i<network->numOfOrigin;i++)
			{
				TNM_SORIGIN* orgn=network->originVector[i];
				//cout<<"o"<<orgn->origin->id<<" ";
				orgn->origin->SkipCentroid=true;
				for(int j=0;j<orgn->numOfDest;j++)
				{
					TNM_SDEST* dest=orgn->destVector[j];
					dest->dest->SkipCentroid=true;
				}
			}
		}
	}//If or not the center is closed when searching for paths
	void			  SetCostCoef(double t, double d) 
	{
		if(t > 0) timeCostCoefficient = t;
		if(d >=0) distCostCoefficient = d;
	}
    void              ConfigureProgressWindow(int iterHint, double startProgress, double endProgress);
    void              ClearProgressWindow();
        virtual int       Report(); //All report functions are packed in it.
        void              SetDbConnStr(const char* db) { m_dbConnStr = (db ? db : ""); }
        void              SetRoleName(const string& role);
        void              SetNetworkTableName(const string& name);
        void              SetODTableName(const string& name);
        void              SetObservedTableName(const string& name);
        const string&     GetRoleName() const { return roleName; }
        const string&     GetNetworkTableName() const { return networkTableName; }
        const string&     GetODTableName() const { return odTableName; }
        const string&     GetObservedTableName() const { return observedTableName; }
        void              SetProgressCallback(TNM_ProgressCallback cb) { m_progressCb = cb; }
protected:
	void			  EmitProgressWindowHeartbeat();
	void			  ReportIter(ofstream &out); //report iteration history to a file
	virtual void	  ReportLink(ofstream &); 
	virtual void	  ReportPath(ofstream &); 

	double			  timeCostCoefficient;
	double			  distCostCoefficient;
	string			  roleName;
	string			  networkTableName;
	string			  odTableName;
	string			  observedTableName;

/*========================================================================================
	Major methods: finding optimal solutions to the given model with given algorithm.
  ========================================================================================*/
public:
	virtual int       Build(const string& inFile, const string& outFileName, MATINFORMAT);
	TERMFLAGS		  Solve();     //public function: find optimal solution 
	virtual void      PreProcess(); //this function is used for performing some operation before initialze
	virtual void      Initialize() {;} //must be overloaded in derived classes for solve
	virtual void      MainLoop() {;}   //must be overloaded in derived classes for solve
	virtual bool      Terminate();     //must be overloaded in derived classes for solve
	virtual void	  PostProcess() {;} //this function is used for performing some operation after Solve
	virtual TERMFLAGS TerminationType();         //determined termination type.
	ITERELEM*         RecordCurrentIter();  //record the information of current iterations
	//virtual void      StoreResult() {;}
	void              ClearIterRecord();
	void			  ColumnGeneration(TNM_SORIGIN* pOrg,TNM_SDEST* dest);
	floatType		  RelativeGap(bool scale = true);
	floatType		  RelativeGap2(bool scale = true);
	virtual void	  ComputeOFV(); // overloaded objective function computation
	double			  ComputeBeckmannObj(bool toll = false); //compute and return the classic beckmann objective function vlaue
	

};

class TNM_EXT_CLASS TAP_Greedy: public TNM_TAP
{
public:
	TAP_Greedy();
	~TAP_Greedy();

	virtual void      PreProcess();
	virtual void      Initialize();
	virtual void	  MainLoop();
	virtual void	  UpdatePathFlowGreedy(TNM_SORIGIN* pOrg,TNM_SDEST* dest);
	virtual void      UpdatePathFlowLazy(TNM_SORIGIN* pOrg,TNM_SDEST* dest);
	virtual void	  QuickSortPath(vector<TNM_SPATH*> &,int low, int high);
	virtual void	  PostProcess();

	double			  TotalFlowChange;
	int				  numOfPathChange;
	double			  totalShiftFlow;
	double			  maxPathGap;
	bool			  columnG;
	double			  innerShiftFlow;
	int				  numOfD;
	vector<TNM_SPATH*> dePathSet;
	double			  aveFlowChange;
	int				  nPath;
	int				  count;
};

/** Greedy + 点对点 Dijkstra（与 base_motor_network 的 TAP_Greedy_dijk 一致） */
class TNM_EXT_CLASS TAP_Greedy_dijk : public TAP_Greedy
{
public:
	TAP_Greedy_dijk();
	~TAP_Greedy_dijk();

	virtual void      Initialize();
	virtual void	  MainLoop();
	virtual void      ColumnGeneration(TNM_SORIGIN* pOrg, TNM_SDEST* dest);
};

class TNM_EXT_CLASS OD_ESTIMATION: public TAP_Greedy_dijk
{
public:
	OD_ESTIMATION();
	~OD_ESTIMATION();

	virtual void Initialize();//Replace the initialize part of the original Greedy and no longer allocate buffer in the initialization
	void odes_preprocess();//Pretreatment and initialize to generate the expected demand and initial demand, do the equilibrium procedure
	virtual void search_direction();//Computation of a search direction: find the descent direction and projection
	void main_line_search();//Armijo-type Generate an appropriate step size in main loop
	virtual void estimation_update();//Use the search direction and step size to update the OD matrix and do the equilibrium procedure
	bool quadratic_prob(TNM_SDEST*);//Embedded into the search direction to solve a quadratic problem for one OD pair
	int  Random(int, int);//Generate a random value from a range
	void emit_estimation_progress(int iterHint, double progress);//Emit fine-grained heartbeat progress during long solves
	virtual void overall_process();//The overall procedure of the OD estimation
	floatType calc_RMSE();//Calculate the root mean squared error (Estimated_flow vs Observed_flow)
	floatType upper_objective();//Calculate the objective function (upper level)

	floatType gamma1;//Parameter in upper objective function(represents the weight of the difference between the estimated OD matrix and the historical OD matrix)
	floatType gamma2;//Parameter in upper objective function(represents the weight of the error between the estimated link flow and the observed link flow)
	int       l;//Main loop indicator
	int       L;//The maximum main loop iteration
	floatType conv_Criterion;//Mainloop convergence criterion
	int       p;//Quadratic problem indicator
	int       P;//The maximum quadratic problem general iteration
	int       ls_j;//j in Pseudo code, line search indicator
	int       ls_J;//J in Pseudo code, the maximum line search iteration
	floatType epsilon_1;//The projection allowable accuracy
	floatType epsilon_2;//Armijo-type line search stop criterion
	floatType Theta;//Armijo parameter	
	floatType alpha_max;//The upper bound of the main loop step size
	floatType alpha_l;//Main loop step size
	floatType oblink_ratio;//The proportion of observed link flow to the total link flow
	int       solveMaxIterFast;
	int       solveMaxIterStrict;
	floatType solveConvFast;
	floatType solveConvStrict;
	bool      finalStrictSolve;

	floatType up_obj;//Upper level objective function value
	floatType RMSE;//The root mean squared error (Estimated_flow vs Observed_flow)
	floatType RGP;//The relative gap
	vector<floatType> up_obj_vec;//Upper level objective function value vector
	vector<floatType> RMSE_vec;//The root mean squared error vector
	vector<floatType> RGP_vec;//The relative gap vector
	vector<floatType> Time_vec;//The time vector

// 进度平滑：RMSE 阈值（默认 0 表示禁用基于 RMSE 的平滑项）
	// floatType rmseThreshold;

	void	  SetGamma(floatType);
	void      SetGamma(floatType, floatType);
	void      SetOblink_ratio(floatType);
	void	  SetMainloop_maxiter(int);
	void	  SetMainloop_conv(floatType);
	void	  SetArmijo_maxiter(int);
	void	  SetArmijo_stopcriterion(floatType);
	void      SetArmijo_coefficient(floatType);
	void      SetArmijo_alphamax(floatType);
	/** 下层 Greedy 分配：fast 用于主循环内试探，strict 用于收尾均衡 */
	void      SetSolveFastParams(int maxIter, floatType conv);
	void      SetSolveStrictParams(int maxIter, floatType conv);
	void      SetFinalStrictSolve(bool enabled) { finalStrictSolve = enabled; }
	TERMFLAGS SolveOdAssignment(bool strict);

	// void      SetRmseThreshold(floatType v) { rmseThreshold = v; }

	vector<TNM_SLINK*> observed_link;//The vector of the observed link
	map<pair<TNM_SDEST*, TNM_SLINK*>, floatType> V;//Used to store the allocation ratio of OD demand corresponding to each link
	virtual int        Report(); //All report functions are packed in it
	bool      reportDemandDetail;//Determine if the estimated demand will be retained
    bool      reportlinkinforDetail;//Determine if the Link flow file will be retained
	bool      reportUpperObjective;//Determine if the Upper objective file will be retained
	virtual int        ReportPG();
	void      SetUseExternalObservedFlow(bool v) { useExternalObservedFlow = v; }
	bool      useExternalObservedFlow;
	/** od_estimate_local / TNADriver：初始 OD 不加噪、跳过首轮均衡，全量 other_observation */
	void      SetLocalEstimateMode(bool v) { localEstimateMode = v; }
	bool      localEstimateMode;
	void      SetResultTablePrefix(const string& prefix) { resultTablePrefix = prefix; }
	void      SetUseRoadwayObserved(bool v);
	void      SetObservedFlowScale(double s);

protected:
        ofstream  demandFile;//Estimated demand file
        ofstream  linkinforFile;//Link flow file
        ofstream  upobjFile;//Upper objective file
        virtual void      ReportDemand(ofstream &out);
        virtual void      ReportLinkInfor(ofstream &);
        virtual void      ReportUpperObjective(ofstream &);
        string    resultTablePrefix;
	bool      useRoadwayObserved;
	double    observedFlowScale;
	void      finalize_strict_equilibrium();
};
#endif

