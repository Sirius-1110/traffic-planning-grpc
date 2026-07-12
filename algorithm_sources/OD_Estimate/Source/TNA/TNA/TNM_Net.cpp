#include "header/stdafx.h"
#include "header/TNM_Algorithm.h"
#include <iostream>
#include <iomanip>
#include <string.h>
#include <math.h>
#include <cassert>
#include <cstdlib>
#include <stack>
#include <algorithm>
#include <libpq-fe.h>
#include <set>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
//#include <my_predicate.h>
using namespace std;

static bool g_relax_sp_expand_all = true;

#ifdef _WIN32
#include <windows.h>
#endif

// 跨编译单元共享的“已删除路径指针”注册表
set<TNM_SPATH*>& TNM_DeletedPathRegistry()
{
    // 使用堆分配并故意不在 atexit 时析构，避免卸载阶段销毁顺序导致的堆破坏
    static set<TNM_SPATH*>* s_deleted = NULL;
    if (s_deleted == NULL) s_deleted = new set<TNM_SPATH*>();
    return *s_deleted;
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

smallInt TNM_SPATH::pathBufferSize = 0;
IDManager& TNM_SPATH::GetIdManager()
{
    static IDManager* s = NULL;
    if (s == NULL) s = new IDManager();
    return *s;
}

TNM_SNODE::TNM_SNODE()
{
	id        =  -1;
	type      =  BASND;
	xCord      = 0;
	yCord      = 0;
	scanStatus = 0;
	pathElem   = new PATHELEM;
	rPathElem  = new PATHELEM;
	buffer     = NULL;
	//pthHeap    = NULL;
	//dummy      = false;
	//guiNode    = NULL;
	//m_spTree   = NULL;
	//kspPEvector = NULL;
	//kspPathElem = NULL;
    attachedOrg= 0;
	attachedDest=0;
	m_isThrough = true;
	is_centroid =false;
	SkipCentroid=false;

}

TNM_SNODE::~TNM_SNODE()
{
	// 只清空向量，不在节点析构中删除链路对象，避免与 TNM_SNET::UnBuild() 重复释放
	if(!forwStar.empty()) forwStar.clear();
	if(!backStar.empty()) backStar.clear();
	if(buffer!=NULL)
	{
		delete [] buffer;
		buffer = NULL;
	}
	delete pathElem;
	delete rPathElem;
}

void TNM_SNODE::InitPathElem()
{
	pathElem->cost = POS_INF_FLOAT;
	pathElem->via  = NULL;
}

bool TNM_SNODE::Initialize(const NODE_VALCONTAINER &cont)
{
	id    = cont.id;
	dummy = cont.dummy;
	xCord = cont.xCord;
	yCord = cont.yCord;
	return true;
}

void TNM_SNODE::SearchMinInLink(SCANLIST *list)
{
	LINK_COST_TYPE dp, curTT;
	vector<TNM_SLINK *>::iterator pv;
	TNM_SNODE *scanNode;
	for (pv = backStar.begin();pv!=backStar.end();pv++)
	{
		if(*pv!=NULL)
		{
			//cout<<"\tsearch link "<<(*pv)->id<<endl;
			scanNode = (*pv)->tail; // get upstream node
			curTT    = (*pv)->cost; //get link cost
            dp       = pathElem->cost; //get the label of current node
		/*	cout<<"cost of its tail node "<<scanNode->id<<" = "<<scanNode->pathElem->cost
				<<"\t curTT + previous cost = "<<curTT + dp<<endl;*/
				if(scanNode->pathElem->cost > curTT + dp)
				{ // if so, update the routing table
                    scanNode->pathElem->cost = curTT + dp;
					scanNode->pathElem->via  = *pv;
    				list->InsertANode(scanNode);
				}// end if
		}
	}
}

void TNM_SNODE::SearchMinOutLink(SCANLIST *list)
{
	LINK_COST_TYPE dp, curTT;
	vector<TNM_SLINK *>::iterator pv;
	TNM_SNODE *scanNode;
	for (pv = forwStar.begin();pv!=forwStar.end();pv++)
	{
		if(*pv!=NULL)
		{
			scanNode = (*pv)->head; // get upstream node
			curTT    = (*pv)->cost; //get link cost
            dp       = pathElem->cost; //get the label of current node
				if(scanNode->pathElem->cost > curTT + dp)
				{ // if so, update the routing table
                    scanNode->pathElem->cost = curTT + dp;
					scanNode->pathElem->via  = *pv;
    				list->InsertANode(scanNode);
				}// end if
		}
	}
}

TNM_SLINK::TNM_SLINK()
{
	id         = 0;
	orderID    = 0;
	type       =  BASLK;
	head       =  NULL;         /*starting node of the link*/
	tail       =  NULL;         /*ending node of the link */
        capacity   =  0.0;          /*link capacity*/
        volume     =  0.0;          /*link volume*/
        observed_volume = 0.0; //外部观测流量
        length     =  0.0;          /*link length*/
	ffs        =  0.0;          /*free flow speed*/
	fft        =  0.0;          /*free flow travel time*/
	cost       =  0;
	toll       =  0.0;
	fdCost     =  0.0;
	//m_classid  =  -1;   //no clasification. 
	markStatus =  0;
	db_ltype   =  -1;
	oLinkPtr   =  NULL;
	//revLink    =  NULL;
	buffer     =  NULL;
	//dummy      =  false;
	//guiLink    =  NULL;
	//m_tlType   =  TT_NOTOLL;
	m_timeCostCoefficient = 1.0;
	m_distCostCoefficient = 0.0;
}

TNM_SLINK::~TNM_SLINK()
{
	DisconnectFW(); //remove forward connection
	DisconnectBK(); //remove backward connection
	//if (revLink!=NULL) 	revLink->revLink = NULL;//set its reverse link's revLink field as NULL;
	if (buffer!=NULL) 
	{
		delete [] buffer;
		buffer = NULL;
	}
}

void TNM_SLINK::DisconnectFW()
{
	//cout<<"SLINK: disconnect FW"<<endl;
	vector<TNM_SLINK *>::iterator pv;
	pv = find(tail->forwStar.begin(), tail->forwStar.end(), this); //directly compare the pointer, this is, somehow
                                                                   //dangerous, because it is possible that the
	                                                               //object has been deleted. so make sure if you want
	                                                               //to delete a link,erase its connection first.
	if(pv!=tail->forwStar.end()) 	tail->forwStar.erase(pv);
}

void TNM_SLINK::DisconnectBK()
{	
	//cout<<"SLINK: disconnect FW"<<endl;
	vector<TNM_SLINK *>::iterator pv;
	pv = find(head->backStar.begin(), head->backStar.end(), this);
	if(pv!=head->backStar.end()) 		head->backStar.erase(pv);
}

bool TNM_SLINK::Initialize(const LINK_VALCONTAINER cont)
{
    //cout<<"\tinitializing slink"<<endl;
	id         = cont.id;
	head       = cont.head;
	tail       = cont.tail;
	capacity   = cont.capacity;
	length     = cont.length;
	ffs        = cont.ffs;
	fft        = length/ffs;
	cost       = fft;
	dummy      = cont.dummy;
	if(tail == NULL)
	{
		cout<<TNM_AcpToUtf8("Creating a new link: tail node pointer is invalid")<<endl;
		return false;
	}
	if(head == NULL)
	{
		cout<<TNM_AcpToUtf8("Creating a new link: head node pointer is invalid")<<endl;
		return false;
	}
	if(capacity<0)    
	{
		cout<<TNM_AcpToUtf8("Warning: negative cap found when constructing a link.")<<endl;
		return false;
	}
	if(length<0)    
	{
		cout<<TNM_AcpToUtf8("Warning: negative length found when constructing a link. length  = ")<<length<<endl;
		return false;
	}
	if(ffs<0)  
	{
		cout<<TNM_AcpToUtf8("Warning: negative speed found when constructing a link. speed = ")<<ffs<<endl;
		return false;
	}
	if(!CheckParallel()) return false;
	ConnectFW();
	ConnectBK(); //connect foward star and backward star
	return true;
	
}

void TNM_SLINK::ConnectFW()
{
	tail->forwStar.push_back(this);
}

void TNM_SLINK::ConnectBK()
{
	head->backStar.push_back(this);
}

bool TNM_SLINK::CheckParallel()
{
	TNM_SLINK *link;
		for (PTRTRACE pv = tail->forwStar.begin(); pv!=tail->forwStar.end(); pv++)
		{
			link = *pv;
			if(link!=NULL)
			{
				if(link->head == head) 
				{
					cout<<TNM_AcpToUtf8("Parallel links found: link ")<<link->id<<TNM_AcpToUtf8(" and ")<<id<<endl;
					return false;
				}
				if(link->id   == id)   
				{
					cout<<TNM_AcpToUtf8("Duplicate links found: link ")<<link->id<<TNM_AcpToUtf8(" existed ")<<endl;
					return false;
				}
			}

		}
		for (PTRTRACE pv = head->backStar.begin(); pv!=head->backStar.end(); pv++)
		{
			link = *pv;
			if(link!=NULL)
			{
				if(link->tail == tail)
				{
					cout<<TNM_AcpToUtf8("Parallel links found: link ")<<link->id<<TNM_AcpToUtf8(" and ")<<id<<endl;
					return false;
				}
				if(link->id   == id)   
				{
					cout<<TNM_AcpToUtf8("Duplicate links found: link ")<<link->id<<TNM_AcpToUtf8(" existed ")<<endl;
					return false;
				}
			}

		}
	return true;
}

floatType TNM_SLINK::GetCost(bool ftoll)
{
	double t, mc = 0.0;
	//double v = volume;
	//if(m_classid >0)
	//{
	//	volume += m_mcHolder.TotalFlowExceptOne(m_classid);
	//	mc = m_mcHolder.GetClassPtr(m_classid)->m_smcost;
	//}
	//switch(m_tlType)
	//{
	//case TT_NOTOLL:
		t =  GetCost_() * m_timeCostCoefficient + length * m_distCostCoefficient + mc;
	//	break;
	//case TT_MTTOLL:
	//	t =  (GetCost_() + GetDerCost_() * volume)*m_timeCostCoefficient + (length * m_distCostCoefficient + mc);
	//	break;
	//case TT_MCTOLL:
	//	if(m_classid == -1) t =  (GetCost_() + GetDerCost_()*volume) * m_timeCostCoefficient + length * m_distCostCoefficient + mc; //sigle class case:
	//	else  t =  GetCost_()  * m_timeCostCoefficient + GetDerCost_() * m_mcHolder.WeightedFlow(m_classid, v) + length * m_distCostCoefficient + mc;
	//		
	//	break;
	//case TT_FXTOLL:
	//	t =  GetCost_() * m_timeCostCoefficient + length * m_distCostCoefficient + toll + mc;
	//	break;
	//}
	//if(m_classid > 0) volume = v;
	//return t;

	return t;
}

//for derivative, for both fixed toll and no toll case, we don't even need to consider other costs, as they are constant
//for first-best toll, they need to be included. 
floatType TNM_SLINK::GetDerCost(bool ftoll)
{
	//double t, v = volume;
	//if(m_classid >0)
	//{
	//	volume += m_mcHolder.TotalFlowExceptOne(m_classid);
	//}
	//switch(m_tlType)
	//{
	//case TT_MTTOLL:
	//	t= (2.0* GetDerCost_() + volume * GetDer2Cost_())*m_timeCostCoefficient;
	//	break;
	//case TT_MCTOLL:
	//	if(m_classid == -1) t= (2.0* GetDerCost_() + volume * GetDer2Cost_())*m_timeCostCoefficient;
	//	else         t = GetDerCost_()  * (m_timeCostCoefficient + m_mcHolder.WeightedVOT(m_classid, v)) + GetDer2Cost_() * m_mcHolder.WeightedFlow(m_classid, v);
	//	break;
	//default:
	//	t = GetDerCost_()* m_timeCostCoefficient;
	//}
	//if(m_classid > 0) volume = v;
	//return (t<1e-15? 1e-15:t);

	double t=GetDerCost_();
	return (t<1e-15? 1e-15:t);
}

floatType TNM_SLINK::GetIntCost(bool ftoll) //when you call this function, make sure volume is the total volume. 
{
	double t;
	//switch(m_classid)
	//{
	//	case -1: //single class;
	//		switch(m_tlType)
	//		{
	//			case TT_NOTOLL:
				t = GetIntCost_()*m_timeCostCoefficient  + length * m_distCostCoefficient * volume; //in NOTOLL equilibrium case, the to
	//			break;
	//		case TT_MTTOLL:
	//			//volume = v; //for first best toll, we reset volume back to just plain volume.  GetDerCost_() * total weight flow is the toll
	//			t=  (GetCost_() + GetDerCost_()*volume) * volume  + length * m_distCostCoefficient * volume/m_timeCostCoefficient; //total travel time already includes the margincal cost toll. 
	//			break;
	//		case TT_MCTOLL:
	//			t=  (GetCost_() + GetDerCost_()*volume) * volume * m_timeCostCoefficient + length * m_distCostCoefficient * volume;
	//			break;
	//		case TT_FXTOLL:
	//			t = GetIntCost_() * m_timeCostCoefficient + (length * m_distCostCoefficient + toll) * volume;
	//			break;
	//		}
	//		break;
	//	case 0: //mulitiple class wthen link is not set into a particular call state.
	//		switch(m_tlType)
	//		{
	//		case TT_NOTOLL:
	//			t = GetIntCost_() + length * m_distCostCoefficient * m_mcHolder.InverseWeightedFlow() + m_mcHolder.TotalSMTime(); //in NOTOLL equilibrium case, the to
	//			break;
	//		case TT_MTTOLL:
	//			t = (GetCost_() + GetDerCost_() * volume) * volume + length * m_distCostCoefficient * m_mcHolder.InverseWeightedFlow() + m_mcHolder.TotalSMTime();
	//			break;
	//		case TT_MCTOLL:
	//			//volume = v; //for first best toll, we reset volume back to just plain volume.  GetDerCost_() * total weight flow is the toll
	//			//t=  (GetCost_() + GetDerCost_()*volume) * volume * m_timeCostCoefficient + length * m_distCostCoefficient * volume; //total travel time already includes the margincal cost toll. 
	//			double v;
	//			v = m_mcHolder.WeightedFlow();
	//			t=  (GetCost_() + GetDerCost_()*v) *v  + length * m_distCostCoefficient * volume + m_mcHolder.TotalSMCost();
	//			break;
	//		case TT_FXTOLL:
	//			t = GetIntCost_() + (length * m_distCostCoefficient + toll) * m_mcHolder.InverseWeightedFlow() + m_mcHolder.TotalSMTime();
	//			break;
	//		}
	//		break;
	//	default: //
	//		cout<<"\tWarning, calling GetIntCost when classID > 0 is unexpected. All calls will receive 0 as return"<<endl;
	//		t = 0.0;
	//		break;
	//}
	return t;
}

double TNM_SLINK::GetToll()
{
	double t;
	//switch(m_classid)
	//{
	//	case -1: //single class;
	//		switch(m_tlType)
	//		{
	//			case TT_NOTOLL:
				t = 0.0;
	//			break;
	//		case TT_MTTOLL:
	//		case TT_MCTOLL:
	//			t = GetDerCost_() * volume* m_timeCostCoefficient; //note that volume should be the current total volume!
	//			break;
	//		case TT_FXTOLL:
	//			t = toll; //directly return the current toll;
	//			break;
	//		}
	//		break;
	//	default: //
	//		
	//		switch(m_tlType)
	//		{
	//		case TT_NOTOLL:
	//			t = 0;
	//			break;
	//		case TT_MCTOLL:		
	//			{
	//			double v = volume;
	//			volume += m_mcHolder.TotalFlowExceptOne(m_classid);
	//			t = GetDerCost_() * m_mcHolder.WeightedFlow(m_classid, v); //note that volume should be the current total volume!
	//			volume = v;
	//			break;
	//			}
	//		case TT_MTTOLL:
	//			{
	//				double v = volume;
	//				volume += m_mcHolder.TotalFlowExceptOne(m_classid);
	//				t = GetDerCost_() * volume *m_timeCostCoefficient;
	//				volume = v;
	//			break;
	//			}
	//		case TT_FXTOLL:
	//			t = toll; //directly return the current toll;
	//			break;
	//		}
	//		break;
	//}
	return t;

	
}

bool TNM_BPRLK::Initialize(const LINK_VALCONTAINER cont)
{
	if (cont.par.size()==2) //in general this is not in use.
	{
		if (cont.par[0]>=0.0 && cont.par[0]<999.0) alpha = cont.par[0];
		if (cont.par[1]>=0.0 && cont.par[1]<99.0)  beta  = cont.par[1];
	}
	return TNM_SLINK::Initialize(cont);
}

floatType TNM_BPRLK::GetCost_()
{
	if(volume <= 0.0) return fft;
	else             return fft*(1.0 + alpha * pow(volume/capacity,beta));
	
//	return fft*(1.0 + 0.15 * powf(volume/capacity,4));
}

floatType TNM_BPRLK::GetDerCost_()
{
	if (volume<= 0.0) return 0.0;
	else return (fabs(beta - 0.0) < 1e-6? 0: fft* alpha * beta * pow(volume/capacity, beta-1.0)/capacity);
	//return fft* alpha * beta * pow(volume/capacity, beta-1.0)/capacity;
	//return fft* 0.6 * pow(volume/capacity, 3)/capacity;
}

floatType TNM_BPRLK::GetIntCost_()
{
	if(volume <= 0.0) return 0.0;
	else              return fft*volume *(1.0 + alpha * pow(volume/capacity, beta)/(beta+1.0));
	//return fft*volume *(1.0 + 0.15 * pow(volume/capacity, 4)/(5.0));
}

floatType TNM_BPRLK::GetDer2Cost_()
{
	if(volume<=0.0) return 0.0;
	else            return (fabs(beta - 1.0) < 1e-6? 0.0: fft * alpha * beta * (beta - 1.0) * pow(volume/capacity,beta - 2.0) /(capacity * capacity));
}

TNM_SPATH::TNM_SPATH()
{
	//SetID();
	id = GetIdManager().SelectANewID();
	GetIdManager().RegisterID(id);
	flow       = 0.0; 
	cost       = 0.0; 
	//active     = true;
	//reIte      =0;
	if (pathBufferSize == 0) buffer     = NULL;
	else
	{
		buffer = new floatType[pathBufferSize];
		for (int i = 0; i<pathBufferSize;i++)
			buffer[i] = 0.0;
	}
	markStatus = 0;
	//m_refAsnElem = NULL;
	//preFlow =0.0;
	//preRatio =0.0;
	//curRatio =0.0;
	fdCost =0.0;
	estCost =0.0;
}

TNM_SPATH::~TNM_SPATH()
{
    if (buffer !=NULL) delete [] buffer;
    buffer =NULL;
    path.clear();
    if (GetIdManager().FindID(id)) GetIdManager().UnRegisterID(id);
}

TNM_SORIGIN::TNM_SORIGIN()
{
	origin = NULL;
	numOfDest = 0;
	destVector = NULL;
	m_class    = 1; 
	m_tdmd     = 0.0;
	//m_smcost   = 0.0;
	//m_id = 0;
	//m_converged = false;
}

TNM_SORIGIN::TNM_SORIGIN(TNM_SNODE *org, int nd)
{
	origin    = org;
	org->attachedOrg++;
	numOfDest = nd;
	if(numOfDest > 0) 
	{
		destVector = new TNM_SDEST*[numOfDest];
		for (int i = 0;i<numOfDest;i++)
			destVector[i] = new TNM_SDEST;
	}
	else
		destVector = NULL;
	//m_orderSubNet = true;
	//m_trimmed = false;
	//m_expanded = false;
	m_class = 1;
	m_tdmd  = 0.0;
	//m_smcost = 0.0;
	//m_count++;
	//m_id = m_count;
}

TNM_SORIGIN::~TNM_SORIGIN()
{
	//cout<<"Deleting a static origin object"<<endl;
	//m_count--;
	//origin->attachedOrg--;
	for (int i = 0;i<numOfDest;i++)
		delete destVector[i];
	//cout<<"destinations are deleted."<<endl;
	if(destVector) delete [] destVector;
	DeleteBush();
}

void TNM_SORIGIN::DeleteBush()
{
for (vector<ORGLINK*>::iterator pv = obLinkVector.begin();
	     pv!=obLinkVector.end(); pv++)
			 delete *pv;
    obLinkVector.clear();
	for (vector<ORGNODE*>::iterator pl = tplNodeVector.begin();
	     pl!=tplNodeVector.end(); pl++)
			 delete *pl;
	tplNodeVector.clear();
}

bool TNM_SORIGIN::SetDest(int id, TNM_SNODE *node, floatType demand)
{
	if(id>numOfDest||id<=0) 
	{
		cout<<TNM_AcpToUtf8("\n\tSetDest in Origin Object: dest index exceeds the rannge!")
			<<TNM_AcpToUtf8("\n\trequired index = ")<<id<<endl;;
		return false;
	}
	return destVector[id - 1]->Initialize(origin, node, demand);
}

//Get all destinatio vectors from destVector to bDestVector
void TNM_SORIGIN::LoadDestVector()
{
	if(destVector == NULL) return;
	bDestVector.clear();
	bDestVector.insert(bDestVector.begin(), destVector, destVector + numOfDest);
	if(destVector!=NULL) 
	{
		delete [] destVector;
		destVector = NULL;
	}
	numOfDest = bDestVector.size();
}

int TNM_SORIGIN::DeleteZeroOD()
{
	vector<TNM_SDEST *>::iterator pv;
	pv = bDestVector.begin();
	while(pv != bDestVector.end())
	{
		if((*pv)->assDemand <=0) 
		{
			delete *pv;
			pv = bDestVector.erase(pv);
		}
		else pv++;
		
	}
	numOfDest = bDestVector.size();
	return 0;

}

void TNM_SORIGIN::UnLoadDestVector()
{
	if(destVector!=NULL) return;
	numOfDest = bDestVector.size();
	if(numOfDest>0)
	{
		destVector = new TNM_SDEST*[numOfDest];
		for (int i = 0;i<numOfDest;i++)
		{
			destVector[i] = bDestVector[i];
		}
		bDestVector.clear();
	}
}

TNM_SDEST::TNM_SDEST()
{
	dest = NULL; 
	origin = NULL;
	assDemand = 0; 
	buffer = NULL;
	yPath = NULL;
	//exist = false;
	costDif = 100;
	shiftFlow =0.0;

	ifZeroDemand = false;
}

TNM_SDEST::~TNM_SDEST()
{
    if (dest && dest->attachedDest > 0) dest->attachedDest--;
    EmptyPathSet();
    if(buffer!=NULL) 
    {
        delete [] buffer;
        buffer = NULL;
    }
	//
	//for (int fi=0;fi<iteInfoVector.size();fi++)
	//{
	//	FWRoute* fwr = iteInfoVector[fi];
	//	delete fwr;
	//}
	//iteInfoVector.clear();
}

void TNM_SDEST::EmptyPathSet()
{
    if (pathSet.empty()) return;
    set<TNM_SPATH*> seen;
    for (PTRPATH pv = pathSet.begin(); pv != pathSet.end(); ++pv)
    {
        TNM_SPATH* p = *pv;
        if (p && seen.insert(p).second) TNM_SafeDeletePath(p);
    }
    pathSet.clear();
}

bool TNM_SDEST::Initialize(TNM_SNODE *org, TNM_SNODE *dt, floatType dmd)
{
	if(org == NULL) return false;
	if(dt  == NULL) return false;
	origin = org;
	dest   =   dt;
	dest->attachedDest++;
	assDemand = dmd;
	return true;

}

TNM_SNET::TNM_SNET(const string& netName)
{
	//m_regime      = RM_STATIC;
	networkName   =  netName;
	numOfNode     =        0;
	numOfLink     =        0;
	numOfOrigin   =		   0;
	numOfOD       =		   0;
	scanList      =     NULL;
	//ChooseSPAlgorithms(QUEUE);
	ChooseSPAlgorithms(DEQUE);  //initialize ScanList;
	buildStatus   = 0;
	initialStatus = 0;
	linkCostScalar = 1.0;
	//m_progressMessage = "TNM in progress";
	//m_progressIndicator = 0.0;
	centroids_blocked=false;
	timeCostCoefficient = 1.0;
	distCostCoefficient = 0.0;
}

TNM_SNET::~TNM_SNET()
{
	//cout<<"beging to delete tnm_snet object\n"<<endl;
	UnBuild();
	if(scanList) delete scanList;
	scanList = NULL;
}

void TNM_SNET::Reset()
{
	TNM_SLINK *link;
	UnInitialize();
	//EmptyPathSet();
	for (int i = 0;i<numOfLink;i++)
	{
		link             = linkVector[i];
		link->volume     = 0.0;
		link->markStatus = 0;
	}
	ClearLink2PathPtr();
	UpdateLinkCost();

}

void TNM_SNET::UpdateLinkCost(bool fToll)
{
	TNM_SLINK *link;
	for(int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		link->cost = link->GetCost(fToll);
	}

}

void TNM_SNET::UpdateLinkCostDer(bool fToll)
{
	TNM_SLINK *link;
	for(int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		link->fdCost = link->GetDerCost(fToll);
	}

}

void TNM_SNET::ClearLink2PathPtr()
{
	for (int i = 0;i<numOfLink;i++)
		linkVector[i]->pathInciPtr.clear();
}

int TNM_SNET::AllocateLinkBuffer(int size)
{
	TNM_SLINK *link;
	if (size <=0)
	{
		cout<<TNM_AcpToUtf8("\tInvalid size of link buffer array")<<endl;
		return 1;
     }
	else
	{
		linkBufferSize = size;
	}

	for (int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		if(link->buffer) delete [] link->buffer;
		link->buffer = new floatType[size];
		if(link->buffer == NULL)
		{
			cout<<TNM_AcpToUtf8("\tCannot allocate memory for link buffer!")<<endl;
			return 1;
		}
		for (int j = 0;j<size;j++)
			link->buffer[j] = 0.0;
	}
	return 0;
}

int TNM_SNET::AllocateNodeBuffer(int size)
{
	TNM_SNODE *node;
	if (size <=0)
	{
		cout<<TNM_AcpToUtf8("\tInvalid size of node buffer array")<<endl;
		return 1;
     }
	else
	{
		nodeBufferSize = size;
	}

	for (int i = 0;i<numOfNode;i++)
	{
		node = nodeVector[i];
		if(node->buffer) delete [] node->buffer;
		node->buffer = new floatType[size];
		if(node->buffer == NULL)
		{
			cout<<TNM_AcpToUtf8("\tCannot allocate memory for node buffer!")<<endl;
			return 1;
		}
			for (int j = 0;j<size;j++)
			node->buffer[j] = 0.0;
	}
	return 0;
}

int TNM_SNET::AllocatePathBuffer(int size)
{
	if (size <=0)
	{
		cout<<TNM_AcpToUtf8("\tInvalid size of path buffer array")<<endl;
		return 1;
     }
	else
	{
		pathBufferSize = size;
		TNM_SPATH::SetPathBufferSize(size);  
	}
	TNM_SORIGIN *org;
	TNM_SDEST *dest;
	PTRPATH pv;
    for (int i = 0;i<numOfOrigin;i++)
	{
		org = originVector[i];
		for (int j = 0;j<org->numOfDest;j++)
		{
			dest = org->destVector[j];
			for (pv = dest->pathSet.begin(); pv != dest->pathSet.end(); pv++)
			{
				if((*pv)->buffer) delete [] (*pv)->buffer;
				(*pv)->buffer = new floatType[size];
				if((*pv)->buffer == NULL)
				{
					cout<<TNM_AcpToUtf8("\tCannot allocate memory for link buffer!")<<endl;
					return 1;
				}
				for (int j = 0;j<size;j++)
					(*pv)->buffer[j] = 0.0;
			}
		}
	}
	return 0;
}

int TNM_SNET::AllocateDestBuffer(int size)
{
	if (size <=0)
	{
		cout<<TNM_AcpToUtf8("\tInvalid size of dest buffer array")<<endl;
		return 1;
     }
	else
	{
		destBufferSize = size;
	}
	TNM_SORIGIN *org;
	TNM_SDEST *dest;
    for (int i = 0;i<numOfOrigin;i++)
	{
		org = originVector[i];
		for (int j = 0;j<org->numOfDest;j++)
		{
			dest = org->destVector[j];
			if(dest->buffer) delete [] dest->buffer;
			dest->buffer = new floatType[size];   
			if (dest->buffer == NULL)
			{
					cout<<TNM_AcpToUtf8("\tCannot allocate memory for dest buffer!")<<endl;
					return 1;
			}
			for (int k = 0;k<size;k++)
			 dest->buffer[k] = 0.0;
		}
	}
	return 0;
}


int TNM_SNET::ReleaseLinkBuffer()
{
	TNM_SLINK *link;
	for (int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		if(link->buffer != NULL)
		{
			delete[] link->buffer;
			link->buffer = NULL;
		}
	}
	linkBufferSize = 0;
	return 0;
}

int TNM_SNET::ReleaseNodeBuffer()
{
	TNM_SNODE *node;
	for (int i = 0;i<numOfNode;i++)
	{
		node = nodeVector[i];
		if(node->buffer != NULL)
		{
			delete[] node->buffer;
			node->buffer = NULL;
		}
	}
	nodeBufferSize = 0;
	return 0;
}

int TNM_SNET::ReleasePathBuffer()
{
	TNM_SORIGIN *org;
	TNM_SDEST *dest;
	PTRPATH pv;
    for (int i = 0;i<numOfOrigin;i++)
	{
		org = originVector[i];
		for (int j = 0;j<org->numOfDest;j++)
		{
			dest = org->destVector[j];
			for (pv = dest->pathSet.begin(); pv != dest->pathSet.end(); pv++)
			{
				if((*pv)->buffer != NULL)
				{
					delete[] (*pv)->buffer;
					(*pv)->buffer = NULL;
				}
			}
		}
	}
	pathBufferSize = 0;
	TNM_SPATH::SetPathBufferSize(0);
	return 0;
}

int TNM_SNET::ReleaseDestBuffer()
{
	TNM_SORIGIN *org;
	TNM_SDEST *dest;
    for (int i = 0;i<numOfOrigin;i++)
	{
		org = originVector[i];
		for (int j = 0;j<org->numOfDest;j++)
		{
			dest = org->destVector[j];
			if (dest->buffer != NULL)
			{
				delete[] dest->buffer;
				dest->buffer = NULL;
			}
		}
	}
	destBufferSize = 0;
	return 0;
}

void TNM_SNET::ChooseSPAlgorithms(DATASTRUCT type)
{
	switch (type)
	{
	case QUEUE:
		if(scanList != NULL) delete scanList;
		scanList  = new SCAN_QUEUE;
		break;
	case DEQUE:
		if (scanList != NULL) delete scanList;
		scanList = new SCAN_DEQUE;
		break;
	default:
		cout<<TNM_AcpToUtf8("Undefined data structure for shortest path search")<<endl;
		break;
	}

}

int TNM_SNET::UnBuild()
{
	//cout<<"\nUnBuilding network object..."<<endl;
	//if(CheckBuildStatus(false) ==0) return 0; //no neeed to do it should the net has not built yet.
	UnInitialize();
	vector<TNM_SORIGIN*>::iterator po;
	for (po = originVector.begin();po!=originVector.end();po++)
	{
		//cout<<"I am deleting origin "<<(*po)->origin->id_()<<endl;
		delete *po;				
	}
	vector<TNM_SLINK*>::iterator pl;
	for (pl = linkVector.begin(); pl!=linkVector.end(); pl++)
	{
		//cout<<"\tbegin to delete link "<<(*pl)->id<<" which is a "<<(*pl)->type<<endl;
	//	cout<<*linkVector[i]<<endl;
		//linkVector[i]->Print();
		delete *pl;
	}
	//cout<<"links are destroyed."<<endl;
	vector<TNM_SNODE *>::iterator pn;
	for (pn = nodeVector.begin(); pn!=nodeVector.end(); pn++)
	{
	//	cout<<"I am deleting node "<<(*pn)->id<<endl;
		delete *pn;
	}
	//cout<<"nodes are destroyed."<<endl;
	if(!linkVector.empty()) linkVector.clear();
	if(!nodeVector.empty()) nodeVector.clear();
	if(!originVector.empty()) originVector.clear();
	if(!destNodeVector.empty()) destNodeVector.clear();
	numOfNode     =        0;
	numOfLink     =        0;
	numOfOrigin   =		   0;
	numOfOD       =		   0;
	buildStatus   = 0;
	initialStatus = 0;
	return 0;
}

int TNM_SNET::UnInitialize()
 {
     bool hasPaths = false;
     for (vector<TNM_SORIGIN*>::iterator po = originVector.begin(); po != originVector.end() && !hasPaths; ++po)
     {
         TNM_SORIGIN* origin = *po;
         if (origin)
         {
             for (int j = 0; j < origin->numOfDest && !hasPaths; ++j)
             {
                 TNM_SDEST* dest = origin->destVector[j];
                 if (dest && !dest->pathSet.empty()) hasPaths = true;
             }
         }
     }
     if (hasPaths)
     {
         EmptyPathSet();
     }
     initialStatus = 0; 
     return 0;
 }

void TNM_SNET::EmptyPathSet()
{
    TNM_SORIGIN *origin;
    TNM_SDEST *dest;
// #ifdef _DEBUG
//     cout << "[debug] TNM_SNET::EmptyPathSet begin origins=" << numOfOrigin << " this=" << this << endl;
// #endif
    for (vector<TNM_SORIGIN*>::iterator po = originVector.begin(); po != originVector.end(); ++po)
    {
        if (*po != NULL)
    {
            origin = *po;
            for (int j = 0; j < origin->numOfDest; j++)
            {
                dest = origin->destVector[j];
                if (dest) dest->EmptyPathSet();
            }
        }
    }
// #ifdef _DEBUG
//     cout << "[debug] TNM_SNET::EmptyPathSet end" << endl;
// #endif
}

void TNM_SNET::SetLinkCostScalar(floatType s)
{
	if(s<=0.01 || s> 99999)
	{
		cout<<TNM_AcpToUtf8("link cost scalar out of range")<<endl;
		linkCostScalar = 1.0;
	}
	linkCostScalar = s;
}

int TNM_SNET::CheckBuildStatus(bool noteBuilt)
{
	switch(buildStatus)
	{
	case 0:
		//if(noteBuilt) cout<<"\tNetwork "<<networkName<<" has not been built."<<endl;
		break;
	case 1:
		if(noteBuilt) cout<<TNM_AcpToUtf8("\tNetwork ")<<networkName<<TNM_AcpToUtf8(" has been built.  You are not allowed to rebuild it")<<endl;
		break;
	default:
		cout<<TNM_AcpToUtf8("\tUnknown build status!")<<endl;
	}
	return buildStatus; 
}

TNM_SLINK* TNM_SNET::CreateNewLink(const LINK_VALCONTAINER lval)
{
   // cout<<"creating a new link"<<endl;
	TNM_SLINK *link = AllocateNewLink(lval.type);
	if(link==NULL) return NULL;
    //cout<<"initailizing a new link "<<endl;
	if(!link->Initialize(lval))		
	{
		delete link;
		return NULL;
	}
	linkVector.push_back(link);
	link->orderID = linkVector.size();
	return link;
}

TNM_SLINK* TNM_SNET::AllocateNewLink(const TNM_LINKTYPE &pType)
{
	TNM_SLINK* link;
	switch(pType)
	{
		case BPRLK:
		{
			TNM_BPRLK *bprlk = new TNM_BPRLK;
			link             = (TNM_SLINK *) bprlk; //dynamic casting
			break;
		}
		//case CPBPR:
		//	{
		//	TNM_CPBPR *cpbpr = new TNM_CPBPR;
		//	link             = (TNM_SLINK *) cpbpr; //dynamic casting
		//	break;
		//	}
		//case ACHLK:
		//{
		//	TNM_ACHLK *achlk = new TNM_ACHLK;
		//	link             = (TNM_SLINK *) achlk;
		//	break;
		//}
		//case CSTLK:
		//	{
		//	TNM_CSTLK *cstlk = new TNM_CSTLK;
		//	link             = (TNM_SLINK *) cstlk;
		//	break;
		//	}
		//case LINLK:
		//	{
		//		TNM_LINLK *linlk = new TNM_LINLK;
		//		link = (TNM_SLINK*) linlk;
		//		break;				
		//	}
		//case EXPLK:
		//	{
		//		TNM_EXPLK *explk = new TNM_EXPLK;
		//		link = (TNM_SLINK*) explk;
		//		break;
		//	}
		default:
            // 静默回退：为未实现的静态链路类型统一回退到 BPRLK，避免创建失败与日志噪音
            {
                TNM_BPRLK *bprlk = new TNM_BPRLK;
                link = (TNM_SLINK *)bprlk;
                break;
            }
	}
	return link;
}

TNM_SNODE* TNM_SNET::CreateNewNode(const NODE_VALCONTAINER nval)
{
	TNM_SNODE *node  = AllocateNewNode(nval.type);
	if(node == NULL) return NULL;
	if(!node->Initialize(nval) )
	{
		delete node;
		return NULL;
	}
	nodeVector.push_back(node);
//	numOfNode++;
	return node;
}

void TNM_SNET::ScaleDemand(floatType r)
{
	if(r< 1e-5 || r > 1e5) 
	{
		cout<<"\tDemand scalar should range between 1e-5 and 1e5! No scaling is done."<<endl;
		return;
	}
	//floatType dr = r;
	TNM_SORIGIN *org;
	TNM_SDEST   *dest;
	for (int i = 0;i<numOfOrigin;i++)
	{
		org = originVector[i];
		for (int j = 0;j<org->numOfDest;j++)
		{
			dest = org->destVector[j];
			dest->assDemand *= r;
		}
	}
}

void TNM_SNET::SetLinkTollType(TNM_TOLLTYPE tl)
{
	for(int i = 0;i<numOfLink;i++)
	{
		if(!linkVector[i]->dummy) //we do not allow you set toll type on dummy links. 
			linkVector[i]->SetTollType(tl); //override all link toll type.
	}
}

TNM_SNODE* TNM_SNET::AllocateNewNode(const TNM_NODETYPE &nType)
{
	TNM_SNODE *node;
	switch(nType)
	{
	case BASND:
		{
		TNM_SNODE *basnd = new TNM_SNODE;
		node = (TNM_SNODE*) basnd;
		break;
		}
	default:
		cout<<nType<<" is not a valid static node type"<<endl;
		return NULL;
	}
	return node;
}

int TNM_SNET::ClearZeroDemandOD()
{
	TNM_SORIGIN *org;
	vector<TNM_SORIGIN*>::iterator ov;
	for (ov = originVector.begin(); ov != originVector.end(); ov++)
	{
		org = *ov;
		org->LoadDestVector();
		org->DeleteZeroOD();
		org->UnLoadDestVector();
		if(org->numOfDest == 0)
			if(DeleteAnOrigin(org->id_())==0) ov --;
	}
	UpdateOriginNum();
	return 0;
}

int TNM_SNET::DeleteAnOrigin(int id)
{
	vector<TNM_SORIGIN *>::iterator pv;
	pv = find_if(originVector.begin(), originVector.end(), predP(&TNM_SORIGIN::id_, id));
	if(pv == originVector.end()) return 1;
	else
	{
		delete *pv;
		originVector.erase(pv);
	}
	numOfOrigin = originVector.size();
	return 0;
}

int TNM_SNET::BuildTAPAS(bool loadod, const TNM_LINKTYPE ltype)
{
	if(CheckBuildStatus(true)!=0) return 7;
	string netfilename, odfilename, nodefilename, tolfilename;
	NODE_VALCONTAINER nval;
	LINK_VALCONTAINER lval;
    netfilename = networkName + "_net.tntp";
	odfilename  = networkName + "_trips.tntp";
	nodefilename = networkName + "_node.tntp";
	tolfilename = networkName + "_tol.dat";
	int nn, nl, nz,nft;
	bool readNode = true;
	ifstream netFile, odFile, nodeFile,tolfile;
	if (!TNM_OpenInFile(netFile, netfilename))    return 1;
    if(loadod && !TNM_OpenInFile(odFile, odfilename))      return 2;
	if(!TNM_OpenInFile(nodeFile, nodefilename))
	{
		cout<<"\tCannot find node coordinates file, ignored!"<<endl;
		readNode = false;
	}

	/*Build node objects*/
	cout<<"\tReading "<<netfilename<<"..."<<endl;	
	TNM_SkipString(netFile, 3);
	netFile>>nz;
	TNM_SkipString(netFile, 3);
	netFile>>nn;
	TNM_SkipString(netFile, 3);
	netFile>>nft;
	TNM_SkipString(netFile, 3);
	netFile>>nl;
	string line;
	int tn;
	for(int i  = 0;i<nn;i++)
	{
		nval.type = BASND;
		nval.id = i + 1; 
		nval.dummy = false;
		if(CreateNewNode(nval) == NULL) return 3;
	}
	for(int i = 1;i<=nft-1 ;i++)
	{
		nodeVector[i-1]->m_isThrough = false;
	}


	/*Build link objects*/
	int tail, head;
	floatType cap, len, fft, t,ffs, B,P, spd;
	vector<string> words;
	int lid = 0;
	while(!netFile.eof())
	{
		getline(netFile, line);
		TNM_GetWordsFromLine(line, words);
		if(words.size() >=1)
		{
			if(words[0].find_first_of("~") == -1 && words.size() >= 10)
			{
				lid++;
				TNM_FromString<int>(tail, words[0], std::dec);
				TNM_FromString<int>(head, words[1], std::dec);
				TNM_FromString<floatType>(cap, words[2], std::dec);
				TNM_FromString<floatType>(len, words[3], std::dec);
				TNM_FromString<floatType>(fft, words[4], std::dec);
				TNM_FromString<floatType>(B, words[5], std::dec); 
				TNM_FromString<floatType>(P, words[6], std::dec);
				TNM_FromString<floatType>(spd, words[7], std::dec);
				lval.type     = ltype;
				lval.id       = lid;
				lval.dummy    = false;
				if(tail !=head) //if tail = head, the link is ignored.
				{
					lval.tail     = CatchNodePtr(tail, true);
					lval.head     = CatchNodePtr(head, true);
					
					if(fft> 0)
					{
						lval.length = len;
						lval.ffs    = lval.length * 60 /fft/linkCostScalar;
					}
					else
					{
						lval.length = len;
						lval.ffs    = 25.0;
					}
					
					//check if the link uses special types.
					TNM_LINKTYPE tltype;				
					if(words[9].size()>=4) //type string larger than
					{
						std::istringstream  str(words[9]); //read it into ltType.
						str>>tltype; //temparary link type
						lval.type = tltype;
					}
					//check if capacity is abnormal, often centriod connectors's capacity are set to 0.0. 
					if(cap <=1e-6)
					{
						cout<<"Warning: link "<<tail<<" - "<<head<<"'s capacity = "<<cap<<", its link performance function is forced to be constant."<<endl;					
						lval.type = ACHLK;
						B = 0.0;
						P = 0.0;
					}
					lval.par.clear();
					lval.par.push_back(B);
					lval.par.push_back(P);
					lval.capacity  = cap;	
					TNM_SLINK *link =CreateNewLink(lval);
					if(link == NULL) return 4;
					if(fft == 0) link->fft=fft;
					TNM_FromString<floatType>(link->cost, words[8], std::dec); //read in toll into cost, note that this is only temperary.
					TNM_FromString<floatType>(link->toll, words[8], std::dec);//now read toll into the permanet cost. 
					//set toll type.
					link->SetTollType(TT_NOTOLL); //inialize all to be no toll.  In most cases, toll type will be specified globablly when solving TAP.
					if(words.size() > 10 ) //allow you to specify a toll type here (note: maybe useful for small examples, in which case you can set toll type link by link. not typically used.
					{
						if(words[10].size() >= 4)
						{
							TNM_TOLLTYPE tt;
							std::istringstream  str(words[10]);
							str>>tt; //temparary link type
							link->SetTollType(tt);
						}
					}
					link->InitializeCostCoef(timeCostCoefficient,distCostCoefficient);
				}
				
			}
		}
	}

	if(readNode)
	{
		/*Read the coordinates of the node*/
		cout<<"\tReading "<<nodefilename<<", please wait..."<<endl;
		getline(nodeFile, line);
		//m_progressMessage = "Reading nodes...";
		for(int i = 0;i<nn;i++)
		{
			//m_progressIndicator = 1.0*i/nn;
			TNM_SNODE *node = nodeVector[i];
			floatType xcol, ycol;
			int id;
			nodeFile>>id>>xcol>>ycol;
			node->xCord = xcol ; //*52.8 is for philadolphia network.
			node->yCord = ycol;
		}
		nodeFile.close();
	}
	
	/*Build origin and destination objects*/
	if(loadod && nz >0)
	{
		cout<<"\tReading "<<odfilename<<", please wait..."<<endl;
		for(int i =0;i<3;i++) getline(odFile,line);
		string tStr;
		odFile>>tStr;
		//this is to pass potential comments lines.
		while(tStr.compare("Origin") !=0 && !odFile.eof())
		{
			odFile>>tStr;
		}
		
		int orgID, nd, destID;
		floatType dmd;
		TNM_SNODE* node;
		vector<int> dvec;
		vector<floatType> dmdvec;
		TNM_SORIGIN *pOrg;
		
		for(int i = 0;i<nz;i++)
		{
			odFile>>orgID;
			if((i+1)%100 ==0)
			{
				cout<<"\t"<<setw(4)<<100*i/nz<<"% completed"<<endl;
			}
		
			odFile>>tStr;
			nd = 0;
			if(!dvec.empty()) dvec.clear();
			if(!dmdvec.empty()) dmdvec.clear();
			while(tStr.compare("Origin") != 0 && !odFile.eof()) //not
			{
				if(TNM_FromString<int>(destID, tStr, std::dec))
				{
					odFile>>tStr;
					odFile>>tStr;
					TNM_FromString<floatType>(dmd, tStr, std::dec);
					odFile>>tStr;
					if(tStr == ";") odFile>>tStr; // test if ; has an space before it.
					dvec.push_back(destID);
					dmdvec.push_back(dmd);
					nd++;
				}
				else
				{
					cout<<"destID = "<<destID<<endl;
					cout<<"OD trip file includes unrecognized format!"<<endl;
					return 5;
				}
			}
			
			if((pOrg = CreateSOrigin(orgID, nd))==NULL) 
			{
				cout<<"cannot create static origin object!"<<endl;
				return 6;
			}
			pOrg->m_tdmd = 0.0;
			pOrg->origin->is_centroid=true;
			if(centroids_blocked) pOrg->origin->SkipCentroid=true;
			for(int j = 0;j<nd;j++)
			{
				node = CatchNodePtr(dvec[j], true);
				pOrg->SetDest(j + 1, node, dmdvec[j]);
				pOrg->m_tdmd += dmdvec[j];
				node->is_centroid=true;
				if(centroids_blocked) node->SkipCentroid=true;
			}
			
		}
	}
	
	UpdateLinkNum();
	UpdateNodeNum();
	if(loadod) UpdateOriginNum();
	else       numOfOrigin = nz;
	buildStatus = 1;

	if (TNM_OpenInFile(tolfile, tolfilename)) 
	{
		cout<<"\tReading toll information from "<<tolfilename<<endl;
		for(int i = 0;i<numOfLink;i++) linkVector[i]->toll = 0.0;
		int ntl, ncount;
		floatType lowb, uppb;
		tolfile>>ntl>>lowb>>uppb;
		for(int i = 0;i<ntl;i++)
		{
			int from, to;
			floatType toll;
			tolfile>>from>>to>>toll;
			TNM_SNODE *fromnode, *tonode;
			if(from >0 && from<= numOfNode && to > 0 && to <= numOfNode)
			{
			
				fromnode = CatchNodePtr(from, true);
				tonode = CatchNodePtr(to, true);
				if(from  && to) 
				{
							
					TNM_SLINK *link = CatchLinkPtr(fromnode, tonode);
					if(link)
					{
						if(toll<lowb)
						{
							link->toll = lowb;
						}
						else if (toll > uppb)
						{
							link->toll = uppb;
						}
						else
						{
							link->toll = toll;
							//ncount++;
						}

					}
					else
					{
						cout<<"\tCannot locate link "<<from<<" - "<<to<<endl;
					}
				}
				

			}
			else
			{
				cout<<"\tCannot locate link "<<from<<" - "<<to<<endl;
			}
		}
		tolfile.close();
	}
	return 0;
}

int TNM_SNET::BuildPostgreSQL(const char* dbConnStr, bool loadod, const TNM_LINKTYPE ltype, const string& networkTableName, const string& odTableName)
{
        // 若网络已构建则与 BuildTAPAS 一致返回 7
        TNM_ResetLastError();
        if (CheckBuildStatus(true) != 0)
        {
                TNM_SetLastError("网络已构建，请先释放当前网络再重新构建");
                return 7;
        }

        auto clean_error_text = [](const char* text) -> string
        {
                string msg = text ? string(text) : string();
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                {
                        msg.pop_back();
                }
                return msg;
        };

        auto to_string_local = [](int v) -> string
        {
                ostringstream oss;
                oss << v;
                return oss.str();
        };

            // —— 连接 PostgreSQL ——
    PGconn* conn = PQconnectdb(dbConnStr);
    if (PQstatus(conn) != CONNECTION_OK) {
        cerr << TNM_AcpToUtf8("\tPostgreSQL ") << TNM_AcpToUtf8("连接失败: ") << PQerrorMessage(conn) << endl;
        PQfinish(conn);
        TNM_SetLastError(string("PostgreSQL 连接失败: ") + clean_error_text(PQerrorMessage(conn)));
        return 1;
    }

        // —— 本地工具：安全数值转换（空指针返回 0） ——
        auto to_int = [](const char* s)->int { return s ? (int)strtol(s, nullptr, 10) : 0; };
        auto to_double = [](const char* s)->double { return s ? strtod(s, nullptr) : 0.0; };

        auto pg_quote_ident = [](const string& name) -> string
        {
                if (name.find('"') != string::npos) return name;
                size_t dot = name.find('.');
                if (dot != string::npos)
                {
                        return string("\"") + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
                }
                return string("\"") + name + "\"";
        };

        string netTable = networkTableName;
        if (netTable.empty()) netTable = TNM_DefaultNetworkTable(string());
        string odTable = odTableName;
        if (odTable.empty()) odTable = TNM_DefaultODTable(string());
        string qNetTable = pg_quote_ident(netTable);
        string qOdTable = pg_quote_ident(odTable);

        // —— CentroidPrebuild（与 Greedy 机动车构网一致）：无 type=10 时从 road_community/road_point 生成 ——
        {
                auto trim_copy_local = [](string s) {
                        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
                        return s;
                };
                struct SchemaTable { string schema; string table; };
                auto split_schema_table = [&](const string& qname) -> SchemaTable {
                        SchemaTable st;
                        size_t dot = qname.find('.');
                        if (dot == string::npos) { st.schema.clear(); st.table = qname; }
                        else { st.schema = qname.substr(0, dot); st.table = qname.substr(dot + 1); }
                        return st;
                };
                auto table_exists = [&](const string& qname) -> bool {
                        SchemaTable st = split_schema_table(qname);
                        string q;
                        if (!st.schema.empty())
                                q = "SELECT 1 FROM information_schema.tables WHERE table_schema='" + st.schema
                                    + "' AND table_name='" + st.table + "' LIMIT 1";
                        else
                                q = "SELECT 1 FROM information_schema.tables WHERE table_schema=current_schema() AND table_name='"
                                    + st.table + "' LIMIT 1";
                        PGresult* tr = PQexec(conn, q.c_str());
                        bool ok = (tr && PQresultStatus(tr) == PGRES_TUPLES_OK && PQntuples(tr) > 0);
                        if (tr) PQclear(tr);
                        return ok;
                };
                auto column_exists = [&](const string& qname, const string& col) -> bool {
                        SchemaTable st = split_schema_table(qname);
                        string q;
                        if (!st.schema.empty())
                                q = "SELECT 1 FROM information_schema.columns WHERE table_schema='" + st.schema
                                    + "' AND table_name='" + st.table + "' AND column_name='" + col + "' LIMIT 1";
                        else
                                q = "SELECT 1 FROM information_schema.columns WHERE table_schema=current_schema() AND table_name='"
                                    + st.table + "' AND column_name='" + col + "' LIMIT 1";
                        PGresult* cr = PQexec(conn, q.c_str());
                        bool ok = (cr && PQresultStatus(cr) == PGRES_TUPLES_OK && PQntuples(cr) > 0);
                        if (cr) PQclear(cr);
                        return ok;
                };
                auto extract_schema_from_conn = [&](const string& connStr) -> string {
                        size_t p = connStr.find("role=");
                        if (p == string::npos) return string();
                        p += 5;
                        size_t q = p;
                        while (q < connStr.size() && connStr[q] != ' ' && connStr[q] != '\'' && connStr[q] != '"') ++q;
                        string role = connStr.substr(p, q - p);
                        while (!role.empty() && (role.back() == '\'' || role.back() == '"')) role.pop_back();
                        return trim_copy_local(role);
                };
                auto normalize_prefix = [&](string p) -> string {
                        p = trim_copy_local(p);
                        while (!p.empty() && p.back() == '_') p.pop_back();
                        return p;
                };

                string scenario_prefix;
                {
                        string bare = netTable;
                        SchemaTable stn = split_schema_table(netTable);
                        bare = stn.table.empty() ? netTable : stn.table;
                        const string suf = "road_way";
                        size_t pos = bare.rfind(suf);
                        if (pos != string::npos && pos + suf.size() == bare.size())
                                scenario_prefix = bare.substr(0, pos);
                }
                // 质心小区表必须与 networkTable 同源；勿信任进程内残留的 PG_SCENARIO_PREFIX
                // （例如先跑慢行 case2 再跑工具 OD 会留下 project{p}_user{u}_case{cid}_slow_）
                if (scenario_prefix.empty()) {
                        const char* env_scn = getenv("PG_SCENARIO_PREFIX");
                        if (env_scn && *env_scn) scenario_prefix = string(env_scn);
                }
                scenario_prefix = normalize_prefix(scenario_prefix);

                int existing_centroid = 0;
                {
                        string qcnt = "SELECT COUNT(*)::int FROM " + qNetTable + " WHERE \"type\" = 10";
                        PGresult* rcnt = PQexec(conn, qcnt.c_str());
                        if (rcnt && PQresultStatus(rcnt) == PGRES_TUPLES_OK && PQntuples(rcnt) > 0)
                                existing_centroid = to_int(PQgetvalue(rcnt, 0, 0));
                        if (rcnt) PQclear(rcnt);
                }

                auto fail_centroid_od = [&](const string& msg) -> int {
                        cout << "[TNA_DLL][CentroidPrebuild] ERROR: " << msg << endl;
                        TNM_SetLastError(msg);
                        PQfinish(conn);
                        return 2;
                };

                // 硬性校验：无论是否已有 type=10，road_way 必须有 geometry 列且每条路段 geometry 非 NULL
                if (!column_exists(netTable, "geometry"))
                        return fail_centroid_od("质心连杆失败：路网表缺少 geometry 列");
                {
                        int geom_null_cnt = 0;
                        string qnull = "SELECT COUNT(*)::int FROM " + qNetTable + " WHERE geometry IS NULL";
                        PGresult* rnull = PQexec(conn, qnull.c_str());
                        if (rnull && PQresultStatus(rnull) == PGRES_TUPLES_OK && PQntuples(rnull) > 0)
                                geom_null_cnt = to_int(PQgetvalue(rnull, 0, 0));
                        if (rnull) PQclear(rnull);
                        if (geom_null_cnt > 0)
                                return fail_centroid_od(
                                        "质心连杆失败：路网表存在 geometry 为 NULL 的路段（共 "
                                        + to_string(geom_null_cnt)
                                        + " 条），请补全几何或重建质心连杆");
                }

                const char* centroid_mode_env = getenv("TNA_CENTROID_CONNECTOR_MODE");
                bool multi_osm_mode = (centroid_mode_env && string(centroid_mode_env) == "multi_osm");
                int max_connectors_per_zone = 5;
                const char* max_conn_env = getenv("TNA_CENTROID_MAX_CONNECTORS");
                if (max_conn_env && *max_conn_env) {
                        max_connectors_per_zone = atoi(max_conn_env);
                        if (max_connectors_per_zone < 1) max_connectors_per_zone = 1;
                        if (max_connectors_per_zone > 20) max_connectors_per_zone = 20;
                }

                const char* skip_prebuild_env = getenv("TNA_SKIP_CENTROID_PREBUILD");
                const bool skip_prebuild = (skip_prebuild_env && string(skip_prebuild_env) == "1");
                if (skip_prebuild && existing_centroid > 0) {
                        cout << "[TNA_DLL][CentroidPrebuild] skip_centroid_prebuild=1: reuse type=10 links="
                             << existing_centroid << endl;
#ifdef _WIN32
                        _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
                        setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
                } else if (!skip_prebuild && existing_centroid > 0 && !multi_osm_mode) {
                        cout << "[TNA_DLL][CentroidPrebuild] skip: existing type=10 links=" << existing_centroid
                             << " in " << netTable << endl;
#ifdef _WIN32
                        _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
                        setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
                } else if (!scenario_prefix.empty()) {
                        if (skip_prebuild && existing_centroid <= 0) {
                                cout << "[TNA_DLL][CentroidPrebuild] skip_centroid_prebuild=1 but no type=10: "
                                     << "auto-building connectors (mode="
                                     << (multi_osm_mode ? "multi_osm" : "single") << ")" << endl;
                        }
                        SchemaTable stn = split_schema_table(netTable);
                        string schema = stn.schema;
                        if (schema.empty()) schema = extract_schema_from_conn(dbConnStr ? string(dbConnStr) : string());
                        string communityTable = schema.empty()
                                ? (scenario_prefix + "_road_community")
                                : (schema + "." + scenario_prefix + "_road_community");
                        string pointTable = schema.empty()
                                ? (scenario_prefix + "_road_point")
                                : (schema + "." + scenario_prefix + "_road_point");
                        cout << "[TNA_DLL][CentroidPrebuild] centroid source: ST_Centroid(" << communityTable << ")\n";
                        string zones_cte =
                                "zones AS (\n"
                                "  SELECT ra.area_id::bigint AS area_id, ST_Centroid(ra.\"geometry\") AS gc\n"
                                "  FROM " + pg_quote_ident(communityTable) + " ra\n"
                                "  WHERE ra.\"geometry\" IS NOT NULL\n"
                                "),\n";

                        if (multi_osm_mode && existing_centroid > 0) {
                                cout << "[TNA_DLL][CentroidPrebuildMulti] rebuild: existing type=10 links="
                                     << existing_centroid << " (multi_osm)" << endl;
                        } else {
                                cout << "[TNA_DLL][CentroidPrebuild] enabled. scenario_prefix='" << scenario_prefix << "'" << endl;
                        }

                        if (!table_exists(communityTable))
                                return fail_centroid_od("质心连杆失败：缺少交通小区表 " + communityTable);
                        if (!table_exists(pointTable))
                                return fail_centroid_od("质心连杆失败：缺少路网节点表 " + pointTable);
                        if (!column_exists(netTable, "type") || !column_exists(netTable, "centroid_matched_node"))
                                return fail_centroid_od("质心连杆失败：路网表缺少 type 或 centroid_matched_node 字段");
                        if (!column_exists(communityTable, "geometry"))
                                return fail_centroid_od("质心连杆失败：交通小区表缺少 geometry 列");
                        if (!column_exists(pointTable, "geometry"))
                                return fail_centroid_od("质心连杆失败：路网节点表缺少 geometry 列");
                        if (!column_exists(netTable, "geometry"))
                                return fail_centroid_od("质心连杆失败：路网表缺少 geometry 列，无法写入质心连杆");

                        string delWay = "DELETE FROM " + qNetTable + " WHERE \"type\" = 10;";
                        PGresult* rdel = PQexec(conn, delWay.c_str());
                        bool del_ok = (rdel && PQresultStatus(rdel) == PGRES_COMMAND_OK);
                        if (rdel) PQclear(rdel);
                        if (!del_ok)
                                return fail_centroid_od("质心连杆失败：清理路网表中旧质心连杆失败");

                        string ins;
                        if (multi_osm_mode) {
                                string osm_key_expr = column_exists(netTable, "link_osmid")
                                        ? "COALESCE(NULLIF(w.\"link_osmid\"::text, ''), w.\"link_id\"::text)"
                                        : "w.\"link_id\"::text";
                                string route_key_expr = column_exists(netTable, "route_ref")
                                        ? "COALESCE(NULLIF(w.\"route_ref\"::text, ''), '')"
                                        : "''";
                                string name_key_expr = column_exists(netTable, "name")
                                        ? "COALESCE(NULLIF(w.\"name\"::text, ''), '')"
                                        : "''";
                                string max_conn_str = to_string(max_connectors_per_zone);
                                ins =
                                        "WITH bm AS (SELECT GREATEST("
                                        "  COALESCE(MAX(init_node),0),"
                                        "  COALESCE(MAX(term_node),0),"
                                        "  COALESCE((SELECT MAX(node_id) FROM " + pg_quote_ident(pointTable) + "),0)"
                                        ") AS base_max FROM " + qNetTable + " WHERE \"type\" <> 10),\n"
                                        "lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM " + qNetTable + "),\n"
                                        + zones_cte +
                                        "link_candidates AS (\n"
                                        "  SELECT z.area_id, w.link_id, w.init_node, w.term_node, w.geometry AS link_geom,\n"
                                        "         w.\"type\"::int AS road_type,\n"
                                        "         " + osm_key_expr + " AS osm_key,\n"
                                        "         " + route_key_expr + " AS route_key,\n"
                                        "         " + name_key_expr + " AS name_key,\n"
                                        "         ST_Distance(ST_Transform(z.gc, 3857), ST_Transform(w.geometry, 3857)) AS dist_m,\n"
                                        "         z.gc,\n"
                                        "         CASE WHEN ST_Distance(ST_Transform(z.gc,3857), ST_Transform(ST_StartPoint(w.geometry),3857))\n"
                                        "                   <= ST_Distance(ST_Transform(z.gc,3857), ST_Transform(ST_EndPoint(w.geometry),3857))\n"
                                        "              THEN w.init_node ELSE w.term_node END AS anchor_node\n"
                                        "  FROM zones z\n"
                                        "  CROSS JOIN bm\n"
                                        "  JOIN " + qNetTable + " w ON w.\"type\" IN (1,2,3) AND w.geometry IS NOT NULL\n"
                                        "),\n"
                                        "link_dedup AS (\n"
                                        "  SELECT DISTINCT ON (area_id, osm_key, route_key, name_key)\n"
                                        "         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, route_key, name_key, anchor_node, road_type\n"
                                        "  FROM link_candidates\n"
                                        "  ORDER BY area_id, osm_key, route_key, name_key, dist_m\n"
                                        "),\n"
                                        "anchor_dedup AS (\n"
                                        "  SELECT DISTINCT ON (area_id, anchor_node)\n"
                                        "         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, route_key, name_key, anchor_node, road_type\n"
                                        "  FROM link_dedup\n"
                                        "  ORDER BY area_id, anchor_node, dist_m\n"
                                        "),\n"
                                        "mandatory_by_type AS (\n"
                                        "  SELECT DISTINCT ON (area_id, road_type)\n"
                                        "         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, route_key, name_key, anchor_node, road_type\n"
                                        "  FROM anchor_dedup\n"
                                        "  WHERE road_type IN (1,2,3)\n"
                                        "  ORDER BY area_id, road_type, dist_m\n"
                                        "),\n"
                                        "mandatory_cnt AS (\n"
                                        "  SELECT area_id, COUNT(*)::int AS n_mandatory FROM mandatory_by_type GROUP BY area_id\n"
                                        "),\n"
                                        "extra_candidates AS (\n"
                                        "  SELECT a.*, ROW_NUMBER() OVER (PARTITION BY a.area_id ORDER BY a.dist_m) AS rn\n"
                                        "  FROM anchor_dedup a\n"
                                        "  WHERE NOT EXISTS (\n"
                                        "    SELECT 1 FROM mandatory_by_type m WHERE m.area_id = a.area_id AND m.link_id = a.link_id\n"
                                        "  )\n"
                                        "),\n"
                                        "picked AS (\n"
                                        "  SELECT area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, route_key, name_key, anchor_node\n"
                                        "  FROM mandatory_by_type\n"
                                        "  UNION ALL\n"
                                        "  SELECT e.area_id, e.link_id, e.init_node, e.term_node, e.link_geom, e.dist_m, e.gc, e.osm_key, e.route_key, e.name_key, e.anchor_node\n"
                                        "  FROM extra_candidates e\n"
                                        "  JOIN mandatory_cnt mc ON mc.area_id = e.area_id\n"
                                        "  WHERE e.rn <= GREATEST(0, " + max_conn_str + " - mc.n_mandatory)\n"
                                        "),\n"
                                        "anchors AS (\n"
                                        "  SELECT p.area_id, p.gc, p.dist_m, p.anchor_node,\n"
                                        "         ST_MakeLine(p.gc, ST_ClosestPoint(p.link_geom, p.gc)) AS geometry\n"
                                        "  FROM picked p\n"
                                        "),\n"
                                        "base AS (\n"
                                        "  SELECT (a.area_id + bm.base_max + 1)::bigint AS init_node, a.anchor_node::bigint AS term_node,\n"
                                        "         9999999::float8 AS capacity, 0.15::float8 AS b, 4::float8 AS power, 0::float8 AS toll, 10::bigint AS type,\n"
                                        "         0.000001::float8 AS fft, 30::float8 AS speedlimit,\n"
                                        "         (a.dist_m / 1000)::numeric(10,4) AS length,\n"
                                        "         a.geometry,\n"
                                        "         a.area_id AS centroid_matched_node\n"
                                        "  FROM anchors a\n"
                                        "  CROSS JOIN bm\n"
                                        "),\n"
                                        "both_dirs AS (\n"
                                        "  SELECT * FROM base\n"
                                        "  UNION ALL\n"
                                        "  SELECT term_node AS init_node, init_node AS term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node FROM base\n"
                                        ")\n"
                                        "INSERT INTO " + qNetTable + " (link_id, init_node, term_node, capacity, b, power, toll, \"type\", fft, speedlimit, length, geometry, centroid_matched_node)\n"
                                        "SELECT (lm.link_max + row_number() OVER ())::bigint AS link_id, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node\n"
                                        "FROM both_dirs, lm;";
                        } else {
                        ins =
                                "WITH bm AS (SELECT GREATEST("
                                "  COALESCE(MAX(init_node),0),"
                                "  COALESCE(MAX(term_node),0),"
                                "  COALESCE((SELECT MAX(node_id) FROM " + pg_quote_ident(pointTable) + "),0)"
                                ") AS base_max FROM " + qNetTable + " WHERE \"type\" <> 10),\n"
                                "lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM " + qNetTable + "),\n"
                                + zones_cte +
                                "base AS (\n"
                                "  SELECT (z.area_id::bigint + bm.base_max + 1)::bigint AS init_node, n.node_id::bigint AS term_node,\n"
                                "         9999999::float8 AS capacity, 0.15::float8 AS b, 4::float8 AS power, 0::float8 AS toll, 10::bigint AS type,\n"
                                "         0.000001::float8 AS fft, 30::float8 AS speedlimit,\n"
                                "         (ST_Distance(ST_Transform(z.gc, 3857), ST_Transform(n.geometry, 3857)) / 1000)::numeric(10,4) AS length,\n"
                                "         ST_MakeLine(z.gc, n.geometry) AS geometry,\n"
                                "         z.area_id::bigint AS centroid_matched_node\n"
                                "  FROM zones z\n"
                                "  CROSS JOIN bm\n"
                                "  CROSS JOIN LATERAL (\n"
                                "    SELECT node_id, geometry FROM " + pg_quote_ident(pointTable)
                                + " ORDER BY z.gc <-> geometry LIMIT 1\n"
                                "  ) AS n\n"
                                "),\n"
                                "both_dirs AS (\n"
                                "  SELECT * FROM base\n"
                                "  UNION ALL\n"
                                "  SELECT term_node AS init_node, init_node AS term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node FROM base\n"
                                ")\n"
                                "INSERT INTO " + qNetTable
                                + " (link_id, init_node, term_node, capacity, b, power, toll, \"type\", fft, speedlimit, length, geometry, centroid_matched_node)\n"
                                "SELECT (lm.link_max + row_number() OVER ())::bigint AS link_id, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node\n"
                                "FROM both_dirs, lm;";
                        }
                        PGresult* rins = PQexec(conn, ins.c_str());
                        bool ins_ok = (rins && PQresultStatus(rins) == PGRES_COMMAND_OK);
                        string em = PQerrorMessage(conn) ? string(PQerrorMessage(conn)) : string();
                        if (rins) PQclear(rins);
                        if (!ins_ok)
                                return fail_centroid_od("质心连杆失败：写入路网表失败" + (em.empty() ? "" : ("：" + clean_error_text(em.c_str()))));

                        existing_centroid = 0;
                        PGresult* rcnt2 = PQexec(conn, ("SELECT COUNT(*)::int FROM " + qNetTable + " WHERE \"type\" = 10").c_str());
                        if (rcnt2 && PQresultStatus(rcnt2) == PGRES_TUPLES_OK && PQntuples(rcnt2) > 0)
                                existing_centroid = to_int(PQgetvalue(rcnt2, 0, 0));
                        if (rcnt2) PQclear(rcnt2);
                        if (existing_centroid <= 0)
                                return fail_centroid_od("质心连杆失败：生成后 type=10 连杆数量为 0，请检查小区面与节点几何是否有效");

#ifdef _WIN32
                        _putenv_s("TNA_OD_MODE", multi_osm_mode ? "centroid_connector_multi_osm_prebuilt" : "centroid_connector_prebuilt");
#else
                        setenv("TNA_OD_MODE", multi_osm_mode ? "centroid_connector_multi_osm_prebuilt" : "centroid_connector_prebuilt", 1);
#endif
                        cout << "[TNA_DLL][CentroidPrebuild] ok: mode="
                             << (multi_osm_mode ? "multi_osm" : "single")
                             << " type=10 links=" << existing_centroid << endl;
                } else if (existing_centroid <= 0) {
                        return fail_centroid_od("质心连杆失败：无法识别方案表前缀，且路网中无既有质心连杆");
                } else {
#ifdef _WIN32
                        _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
                        setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
                }
        }

	// —— 读取网络规模（nl 行数、nn 节点编号上界） ——
        cout << TNM_AcpToUtf8("\tReading ") << netTable << TNM_AcpToUtf8(" ...") << endl;
        string sqlCount = "SELECT COUNT(*) AS nl, GREATEST(COALESCE(MAX(init_node),0), COALESCE(MAX(term_node),0)) AS nn FROM " + qNetTable;
        PGresult* r = PQexec(conn, sqlCount.c_str());
            if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("查询网络规模失败: ") << PQerrorMessage(conn) << endl;
        PQclear(r); PQfinish(conn);
        TNM_SetLastError(string("查询网络规模失败: ") + clean_error_text(PQerrorMessage(conn)));
        return 1;
    }
	int nl = to_int(PQgetvalue(r, 0, 0)); // 仅用于日志/校验，可不直接使用
	int nn = to_int(PQgetvalue(r, 0, 1));
	PQclear(r);

	// —— FIRST THRU NODE 固定为 1（全部节点允许穿行） ——
	int nft = 1;

	// —— 构建节点对象（编号从 1..nn） ——
	NODE_VALCONTAINER nval;
	for (int i = 0; i < nn; ++i) {
		nval.type = BASND;
		nval.id = i + 1;
		nval.dummy = false;
                if (CreateNewNode(nval) == NULL) {
                        PQfinish(conn);
                        TNM_SetLastError(string("创建节点失败，节点编号 ") + to_string_local(i + 1));
                        return 3;
                }
	}
	// 若 nft>1 则把 [1..nft-1] 设为非穿行；此处 nft=1，循环为空，行为与 BuildTAPAS 一致

	// —— 读取链路表，并包含质心辅助列 ——
	string sqlLink = string("SELECT init_node, term_node, capacity, length, fft, b, power, speedlimit, toll, type, link_id, centroid_matched_node FROM ") + qNetTable + string(" ORDER BY link_id");
	r = PQexec(conn, sqlLink.c_str());
	if (PQresultStatus(r) != PGRES_TUPLES_OK) {
		cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("读取链路表失败: ") << PQerrorMessage(conn) << endl;
		PQclear(r); PQfinish(conn);
		TNM_SetLastError(string("读取链路表失败: ") + clean_error_text(PQerrorMessage(conn)));
		return 1;
	}
	else cout << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("已读取链路信息") << endl;

	LINK_VALCONTAINER lval;
	int rows = PQntuples(r);
	int lid = 0;
	int maxLinkId = 0;
	unordered_set<long long> edgeAll;
	struct TNM_CentroidLinkRaw
	{
		int tail;
		int head;
		floatType cap;
		floatType len;
		floatType fft;
		floatType B;
		floatType P;
		floatType ffs;
		int type_val;
	};
	vector<TNM_CentroidLinkRaw> centroidLinks;
	// 构建质心映射所需容器
	long long baseMaxNodeId = 0;
	map<int,int> zone2centroid;
	map<int,int> zone2anchor;
	for (int i = 0; i < rows; ++i) {
		int tail = to_int(PQgetvalue(r, i, 0)); // init_node 作为 tail
		int head = to_int(PQgetvalue(r, i, 1)); // term_node 作为 head
		floatType cap = (floatType)to_double(PQgetvalue(r, i, 2));
		floatType len = (floatType)to_double(PQgetvalue(r, i, 3));
		floatType fft = (floatType)to_double(PQgetvalue(r, i, 4));
		floatType B = (floatType)to_double(PQgetvalue(r, i, 5));
		floatType P = (floatType)to_double(PQgetvalue(r, i, 6));
		floatType spd = (floatType)to_double(PQgetvalue(r, i, 7));
		// toll 列当前未直接使用
		bool has_type = !PQgetisnull(r, i, 9);
		int   type_val = has_type ? to_int(PQgetvalue(r, i, 9)) : -1;
		int   link_id_db = to_int(PQgetvalue(r, i, 10));

		// 质心映射：基于 type=10 与 centroid_matched_node
		if (has_type) {
			if (type_val != 10) {
				if (tail > baseMaxNodeId) baseMaxNodeId = tail;
				if (head > baseMaxNodeId) baseMaxNodeId = head;
			} else {
				const char* cmnStr = PQgetvalue(r, i, 11);
				long long z = (cmnStr && *cmnStr) ? atoll(cmnStr) : -1;
				if (z >= 0) {
					int centroidNodeId = (tail > head) ? tail : head;
					int anchorNodeId   = (tail > head) ? head : tail;
					zone2centroid[(int)z] = centroidNodeId;
					zone2anchor[(int)z]   = anchorNodeId;
				}
			}
		}

		if (tail == head) continue; // 自环忽略

		lval.type = ltype;
		lval.id = (link_id_db != 0 ? link_id_db : (++lid));
		lval.dummy = false;
		lval.tail = CatchNodePtr(tail, true);
		lval.head = CatchNodePtr(head, true);
		lval.par.clear();
		lval.par.push_back(B);
		lval.par.push_back(P);
		lval.capacity = cap;
		// 令 ffs 与给定 length/fft 或 speedlimit 对齐
		double ffs_val = (fft > 0.0 && len > 0.0) ? (double)len / (double)fft : (double)spd;
		if (ffs_val <= 0.0) ffs_val = 1.0;
		lval.ffs = (floatType)ffs_val;
		lval.length = len;

		TNM_SLINK* link = CreateNewLink(lval);
		if (link == NULL) {
			TNM_SetLastError(string("创建链路失败，链路 ") + to_string_local(tail) + " -> " + to_string_local(head));
			PQclear(r);
			PQfinish(conn);
			return 4;
		}
		// 记录数据库的原始类型，用于后续排除质心连杆
		link->db_ltype = type_val;
		// 如数据库提供 fft，则覆盖
		if (fft > 0.0) { link->fft = fft; link->cost = link->fft; }
		if (lval.id > maxLinkId) maxLinkId = lval.id;
		{
			long long k = ((long long)tail << 32) ^ (unsigned int)head;
			edgeAll.insert(k);
		}
		if (type_val == 10)
		{
			TNM_CentroidLinkRaw raw;
			raw.tail = tail;
			raw.head = head;
			raw.cap = cap;
			raw.len = len;
			raw.fft = fft;
			raw.B = B;
			raw.P = P;
			raw.ffs = (floatType)ffs_val;
			raw.type_val = type_val;
			centroidLinks.push_back(raw);
		}
	}

	PQclear(r);

	{
		int addedReverse = 0;
		for (size_t i = 0; i < centroidLinks.size(); ++i)
		{
			const TNM_CentroidLinkRaw& e = centroidLinks[i];
			long long krev = ((long long)e.head << 32) ^ (unsigned int)e.tail;
			if (edgeAll.find(krev) != edgeAll.end()) continue;
			lval.type = ltype;
			lval.id = ++maxLinkId;
			lval.dummy = false;
			lval.tail = CatchNodePtr(e.head, true);
			lval.head = CatchNodePtr(e.tail, true);
			lval.par.clear();
			lval.par.push_back(e.B);
			lval.par.push_back(e.P);
			lval.capacity = e.cap;
			lval.ffs = e.ffs;
			lval.length = e.len;
			TNM_SLINK* link2 = CreateNewLink(lval);
			if (link2 != NULL)
			{
				link2->db_ltype = e.type_val;
				if (e.fft > 0.0) { link2->fft = e.fft; link2->cost = link2->fft; }
				edgeAll.insert(krev);
				addedReverse++;
			}
		}
		if (addedReverse > 0)
		{
			ostringstream oss;
			oss << "Info: 自动补建质心连杆反向边 " << addedReverse << " 条";
			TNM_AppendMessageNote(oss.str());
		}
	}

	// —— 读取 OD（若 loadod=true） ——
	int nz = 0; // 统计唯一起点数
	if (loadod) {
		// 先做一次轻量查询，确保表存在
		string sqlCheckOd = string("SELECT COUNT(*) FROM ") + qOdTable;
		PGresult* rc = PQexec(conn, sqlCheckOd.c_str());
		if (PQresultStatus(rc) != PGRES_TUPLES_OK) {
			cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("未找到 OD 表或查询失败: ") << PQerrorMessage(conn) << endl;
			PQclear(rc); PQfinish(conn);
			TNM_SetLastError(string("未找到 OD 表或查询失败: ") + clean_error_text(PQerrorMessage(conn)));
			return 2;
		}
		PQclear(rc);

		cout << TNM_AcpToUtf8("\tReading ") << odTable << TNM_AcpToUtf8(", please wait...") << endl;
		string sqlOd = string("SELECT f_id, t_id, demand FROM ") + qOdTable + string(" WHERE demand > 0 ORDER BY 1,2");
		PGresult* ro = PQexec(conn, sqlOd.c_str());
		if (PQresultStatus(ro) != PGRES_TUPLES_OK) {
			cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("读取 OD 失败: ") << PQerrorMessage(conn) << endl;
			PQclear(ro); PQfinish(conn);
			TNM_SetLastError(string("读取 OD 失败: ") + clean_error_text(PQerrorMessage(conn)));
			return 2;
		}

		map<int, vector<pair<int, floatType>>> od_map;
		int nrow = PQntuples(ro);
		for (int i = 0; i < nrow; ++i) {
			int org = to_int(PQgetvalue(ro, i, 0));
			int dst = to_int(PQgetvalue(ro, i, 1));
			floatType dm = (floatType)to_double(PQgetvalue(ro, i, 2));
			if (dm > 0) od_map[org].push_back(make_pair(dst, dm));
		}
		PQclear(ro);

		// 映射统计日志
		cout << TNM_AcpToUtf8("\t[DEBUG] Base max road node id (type!=10): ") << (int)baseMaxNodeId << endl;
		cout << TNM_AcpToUtf8("\t[DEBUG] Centroid mapping pairs (zone -> centroid_node(new), anchor_node(road)): ") << (int)zone2centroid.size() << endl;
		for (map<int,int>::iterator it = zone2centroid.begin(); it != zone2centroid.end(); ++it) {
			int z = it->first; int cn = it->second; int an = (zone2anchor.count(z) ? zone2anchor[z] : -1);
			cout << TNM_AcpToUtf8("\t  zone ") << z << TNM_AcpToUtf8(" -> centroid_node(new) ") << cn << TNM_AcpToUtf8(", anchor_node(road) ") << an << endl;
		}

		centroidNodeToZone.clear();
		for (map<int,int>::iterator it = zone2centroid.begin(); it != zone2centroid.end(); ++it) {
			if (it->second > 0 && it->first > 0) {
				centroidNodeToZone[it->second] = it->first;
			}
		}

		bool hasCentroid = !zone2centroid.empty();
		if (!hasCentroid) {
			cout << TNM_AcpToUtf8("\t[DEBUG] No centroid connectors (type=10) detected. Using direct node IDs for OD mapping.") << endl;
		}

		for (map<int, vector<pair<int, floatType>>>::iterator kv = od_map.begin(); kv != od_map.end(); ++kv) {
			int orgZone = kv->first;
			vector<pair<int, floatType> >& pairs = kv->second;
			int nd = (int)pairs.size();
			if (nd <= 0) continue;

			if (hasCentroid) {
				// 严格质心模式：OD zone 必须映射到质心新节点
				int originNodeId = -1;
				map<int,int>::iterator mo = zone2centroid.find(orgZone);
				if (mo != zone2centroid.end()) originNodeId = mo->second;
				else { TNM_SetLastError(string("OD映射失败：起点小区 ") + to_string_local(orgZone) + string(" 无质心连杆映射")); PQfinish(conn); return 2; }

				TNM_SORIGIN* pOrg = CreateSOrigin(originNodeId, nd);
				if (pOrg == NULL) { cerr << TNM_AcpToUtf8("cannot create static origin object!") << endl; TNM_SetLastError(string("创建起点对象失败，起点 ID ") + to_string_local(originNodeId)); PQfinish(conn); return 6; }
				pOrg->m_tdmd = 0.0;
				if (pOrg->origin) { pOrg->origin->is_centroid = true; if (centroids_blocked) pOrg->origin->SkipCentroid = true; }

				for (int j = 0; j < nd; ++j) {
					int destZone = pairs[j].first; floatType dmd = pairs[j].second;
					int destNodeId = -1; map<int,int>::iterator md = zone2centroid.find(destZone);
					if (md != zone2centroid.end()) destNodeId = md->second; else { TNM_SetLastError(string("OD映射失败：终点小区 ") + to_string_local(destZone) + string(" 无质心连杆映射")); PQfinish(conn); return 2; }
					TNM_SNODE* node = CatchNodePtr(destNodeId, false);
					if (node == NULL) { TNM_SetLastError(string("catch_node_dest: cannot find mapped node for dest zone ") + to_string_local(destZone)); PQfinish(conn); return 2; }
					pOrg->SetDest(j + 1, node, dmd); pOrg->m_tdmd += dmd;
					if (node) { node->is_centroid = true; if (centroids_blocked) node->SkipCentroid = true; }
				}
			}
			else {
				// 无质心模式：OD 的 f_id/t_id 直接视为节点 ID
				TNM_SNODE* orgNode = CatchNodePtr(orgZone, false);
				if (orgNode == NULL) { TNM_SetLastError(string("OD映射失败：起点小区 ") + to_string_local(orgZone) + string(" 在路网中无对应节点")); PQfinish(conn); return 2; }
				TNM_SORIGIN* pOrg = CreateSOrigin(orgZone, nd);
				if (pOrg == NULL) { cerr << TNM_AcpToUtf8("cannot create static origin object!") << endl; TNM_SetLastError(string("创建起点对象失败，起点 ID ") + to_string_local(orgZone)); PQfinish(conn); return 6; }
				pOrg->m_tdmd = 0.0;
				// 非质心模式不设置 is_centroid/SkipCentroid

				for (int j = 0; j < nd; ++j) {
					int destZone = pairs[j].first; floatType dmd = pairs[j].second;
					TNM_SNODE* node = CatchNodePtr(destZone, false);
					if (node == NULL) { TNM_SetLastError(string("OD映射失败：终点小区 ") + to_string_local(destZone) + string(" 在路网中无对应节点")); PQfinish(conn); return 2; }
					pOrg->SetDest(j + 1, node, dmd); pOrg->m_tdmd += dmd;
				}
			}
		}
		nz = (int)od_map.size();
	}

        // —— 收尾：更新计数并设置构建状态 ——
	UpdateLinkNum();
	UpdateNodeNum();
	if (loadod) UpdateOriginNum();
	else        numOfOrigin = nz;
	buildStatus = 1;

        PQfinish(conn);
        return 0;
}

static bool pg_table_exists(PGconn* conn, const string& qualified_name)
{
        if (conn == NULL || qualified_name.empty()) {
                return false;
        }
        string schema = TNM_DEFAULT_ROLE;
        string table = qualified_name;
        size_t dot = qualified_name.find('.');
        if (dot != string::npos) {
                schema = qualified_name.substr(0, dot);
                table = qualified_name.substr(dot + 1);
        }
        const char* values[2] = { schema.c_str(), table.c_str() };
        PGresult* res = PQexecParams(
                conn,
                "SELECT 1 FROM information_schema.tables WHERE table_schema = $1 AND table_name = $2 LIMIT 1",
                2, nullptr, values, nullptr, nullptr, 0);
        bool exists = (res != nullptr && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
        if (res != nullptr) {
                PQclear(res);
        }
        return exists;
}

// 从 PostgreSQL 读取外部观测流量并回填到网络
int LoadObservedLinkFlowFromPG(TNM_SNET* net, const string& connStr, const string& observedTableName)
{
        if (net == NULL)
        {
                TNM_SetLastError("网络指针为空，无法加载观测流量");
                return 1;
        }

        auto clean_error_text = [](const char* text) -> string
        {
                string msg = text ? string(text) : string();
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                {
                        msg.pop_back();
                }
                return msg;
        };

        PGconn* conn = PQconnectdb(connStr.c_str());
        if (PQstatus(conn) != CONNECTION_OK) {
                cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("观测流量数据库连接失败: ") << PQerrorMessage(conn) << endl;
                PQfinish(conn);
                TNM_SetLastError(string("观测流量数据库连接失败: ") + clean_error_text(PQerrorMessage(conn)));
                return 2;
        }

        auto pg_quote_ident = [](const string& name) -> string
        {
                if (name.find('"') != string::npos) return name;
                size_t dot = name.find('.');
                if (dot != string::npos)
                {
                        return string("\"") + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
                }
                return string("\"") + name + "\"";
        };

        string table = observedTableName;
        if (table.empty()) table = TNM_DefaultObservedTable(string());
        if (!pg_table_exists(conn, table)) {
                PQfinish(conn);
                TNM_SetLastError(string("未找到路段流量观测表: ") + table
                                 + string("。请先导入路段流量观测数据（link_id、flow）。"));
                return 5;
        }
        string qTable = pg_quote_ident(table);

        string sqlObserved = "SELECT link_id, SUM(flow) AS flow FROM " + qTable + " WHERE flow IS NOT NULL AND flow > 0 GROUP BY link_id";
        PGresult* r = PQexec(conn, sqlObserved.c_str());
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
                cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("读取观测流量失败: ") << PQerrorMessage(conn) << endl;
                PQclear(r); PQfinish(conn);
                TNM_SetLastError(string("读取观测流量失败: ") + clean_error_text(PQerrorMessage(conn)));
                return 3;
        }

        int rows = PQntuples(r);
        if (rows == 0) {
                cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("观测流量表为空") << endl;
                PQclear(r); PQfinish(conn);
                TNM_SetLastError(string("路段流量观测表无有效数据: ") + table
                                 + string("。请先导入路段流量观测数据（link_id、flow）。"));
                return 4;
        }

        unordered_map<int, double> obs;
        for (int i = 0; i < rows; ++i) {
                const char* sid = PQgetvalue(r, i, 0);
                const char* sflow = PQgetvalue(r, i, 1);
                if (!sid || !sflow) continue;
                int lid = (int)strtol(sid, NULL, 10);
                double flw = strtod(sflow, NULL);
                if (flw > 0.0) {
                        obs[lid] = flw;
                }
        }
        PQclear(r);

        int matched = 0;
        for (int i = 0; i < net->numOfLink; ++i) {
                TNM_SLINK* link = net->linkVector[i];
                auto it = obs.find(link->id);
                if (it != obs.end()) {
                        link->observed_volume = (floatType)it->second;
                        if (link->observed_volume > 0.0) {
                                ++matched;
                        }
                }
        }

        PQfinish(conn);
        if (matched == 0) {
                TNM_SetLastError(string("路段流量观测与路网 link_id 无匹配有效记录: ") + table
                                 + string("。请校核观测表 link_id 后重新导入。"));
                return 6;
        }
        return 0;
}

// 从 PostgreSQL 读取 roadway 分配结果并按比例更新表内 volume，同时回填到内存 observed_volume
int LoadObservedLinkFlowFromRoadway(TNM_SNET* net, const string& connStr, const string& roadwayTableName, double scale)
{
        if (net == NULL)
        {
                TNM_SetLastError("网络指针为空，无法加载 roadway");
                return 1;
        }

        auto clean_error_text = [](const char* text) -> string
        {
                string msg = text ? string(text) : string();
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                {
                        msg.pop_back();
                }
                return msg;
        };

        if (scale <= 0.0) scale = 1.0;

        PGconn* conn = PQconnectdb(connStr.c_str());
        if (PQstatus(conn) != CONNECTION_OK) {
                cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("分配结果数据库连接失败: ") << PQerrorMessage(conn) << endl;
                PQfinish(conn);
                TNM_SetLastError(string("分配结果数据库连接失败: ") + clean_error_text(PQerrorMessage(conn)));
                return 2;
        }

        auto pg_quote_ident = [](const string& name) -> string
        {
                if (name.find('"') != string::npos) return name;
                size_t dot = name.find('.');
                if (dot != string::npos)
                {
                        return string("\"") + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
                }
                return string("\"") + name + "\"";
        };

        string table = roadwayTableName;
        if (table.empty()) {
                PQfinish(conn);
                TNM_SetLastError("roadway 表名为空");
                return 3;
        }
        string qTable = pg_quote_ident(table);

        // 直接读取 roadway 表：优先用 volume_edit，若为空或为 0 则回退到 volume。
        // 仅选择正值并回填至内存 observed_volume（按 link_id 聚合）
        string sqlRead = string("SELECT link_id, ")
                         + string("GREATEST(")
                         + string("COALESCE(MAX(NULLIF(volume_edit, 0)), 0), ")
                         + string("COALESCE(MAX(NULLIF(volume, 0)), 0)")
                         + string(") AS volume ")
                         + string("FROM ") + qTable
                         + string(" WHERE COALESCE(type, 0) <> 10 GROUP BY link_id ")
                         + string(" HAVING GREATEST(")
                         + string("COALESCE(MAX(NULLIF(volume_edit, 0)), 0), ")
                         + string("COALESCE(MAX(NULLIF(volume, 0)), 0)")
                         + string(") > 0");
        PGresult* rr = PQexec(conn, sqlRead.c_str());
        if (PQresultStatus(rr) != PGRES_TUPLES_OK) {
                cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("读取 roadway 分配结果失败: ") << PQerrorMessage(conn) << endl;
                PQclear(rr); PQfinish(conn);
                TNM_SetLastError(string("读取 roadway 分配结果失败: ") + clean_error_text(PQerrorMessage(conn)));
                return 5;
        }

        int rows = PQntuples(rr);
        if (rows == 0) {
                cerr << TNM_AcpToUtf8("\t") << TNM_AcpToUtf8("roadway 表为空或无 volume_edit") << endl;
                PQclear(rr); PQfinish(conn);
                TNM_SetLastError("roadway 表为空或无 volume_edit 字段数据");
                return 6;
        }

        unordered_map<int, double> obs;
        for (int i = 0; i < rows; ++i) {
                const char* sid = PQgetvalue(rr, i, 0);
                const char* svol = PQgetvalue(rr, i, 1);
                if (!sid || !svol) continue;
                int lid = (int)strtol(sid, NULL, 10);
                double vol = strtod(svol, NULL);
                if (vol <= 0.0) continue;
                obs[lid] = vol * scale;
        }
        PQclear(rr);

        for (int i = 0; i < net->numOfLink; ++i) {
                TNM_SLINK* link = net->linkVector[i];
                if (!link) continue;
                auto it = obs.find(link->id);
                if (it != obs.end()) link->observed_volume = (floatType)it->second;
        }

        PQfinish(conn);
        return 0;
}

TNM_SLINK* TNM_SNET::CatchLinkPtr(TNM_SNODE* tail, TNM_SNODE *head)
{
	for (PTRTRACE pl = tail->forwStar.begin(); pl!=tail->forwStar.end(); pl++)
	{
			if((*pl))
			{
				if((*pl)->head == head)
					return *pl;
			}
	}
	return NULL;
}

void TNM_SNET::UpdateOriginNum()
{
	numOfOrigin = originVector.size();
	numOfOD = 0;
	for(int i = 0;i<numOfOrigin;i++)
	{
		numOfOD += originVector[i]->numOfDest;
	}
	for (int i = 0;i<numOfNode;i++)
		if(nodeVector[i]->attachedDest >0) destNodeVector.push_back(nodeVector[i]);
}

TNM_SORIGIN* TNM_SNET::CreateSOrigin(int nodeID, int nd)
{
	TNM_SNODE *node = CatchNodePtr(nodeID);
	if(node == NULL)
	{
		cout<<"\n\tnode "<<nodeID<<" is not a valid node object. "<<endl;
		return NULL;
	}
	else return CreateSOriginP(node, nd);
	
}

TNM_SORIGIN* TNM_SNET::CreateSOriginP(TNM_SNODE* node, int nd)
{
	if (nd <0)
	{
		cout<<"\n\tOrigin "<<node->id<<" contains none or negative destinations."<<endl;
		return NULL;
	}
	TNM_SORIGIN *org = new TNM_SORIGIN(node, nd);
	if (org == NULL)
	{
		cout<<"\n\tCannot allocate memory for new origin"<<endl;
		return NULL;
	}
	originVector.push_back(org);
	numOfOrigin ++;
	numOfOD+=nd;
	return org;
}

TNM_SNODE * TNM_SNET::CatchNodePtr(int id, bool safe)
{
	/*if(nodeVector[id - 1]->id == id) return nodeVector[id-1];
	else*/
	if(safe) return nodeVector[id - 1];
	else
	{
		vector<TNM_SNODE *>::iterator pv;
		pv = find_if(nodeVector.begin(), nodeVector.end(), predP(&TNM_SNODE::id_, id));
		if(pv == nodeVector.end()) return NULL;
		else                       return *pv;
	}
}

//Shortest path calculaton provided address of a given origin
void TNM_SNET::UpdateSP(TNM_SNODE *rootNode)
{
	TNM_SNODE *node;
//initialize node for shortest path calculation
	for (int j = 0;j<numOfNode;j++)
	{
		node = nodeVector[j];
		node->InitPathElem();            
		node->scanStatus = 0;
	}
//call scanList 's major method to compute shortest path
	if(centroids_blocked) rootNode->SkipCentroid=false;
	scanList->SPTreeO(rootNode);//compute shortest path;
	if(centroids_blocked) rootNode->SkipCentroid=true;
}

static inline bool TNM_IsKnownLinkInNet(const TNM_SNET* net, TNM_SLINK* p)
{
    if (p == NULL) return false;
    for (size_t i = 0; i < net->linkVector.size(); ++i)
    {
        if (net->linkVector[i] == p) return true;
    }
    return false;
}

void TNM_SNET::InitialSubNet3(int kx, bool createPath)
{
	TNM_SORIGIN *origin;
	for (int i = 0;i<numOfOrigin; i++)
	{
		origin = originVector[i];
		
		UpdateSP(origin->origin);
		
		//add path
		{
		
			for (int j=0; j<origin->numOfDest; j++)
			{
				TNM_SDEST* dest = origin->destVector[j];
				if(origin->origin == dest->dest) continue;
				//test
				//cout<<"For dest "<<dest->dest->id<<endl;
				//add iteration 0
				
				//
				double dmd = dest->assDemand;
				TNM_SPATH* path= new TNM_SPATH;
				path->path.clear();
				TNM_SNODE* snode = NULL;
				TNM_SLINK* slink = NULL;
				snode = dest->dest;
				while(snode != origin->origin)
				{
					slink = snode->pathElem->via;
					if(slink == NULL) 
					{
						cout<<"The link is null in calculating the direction for OFW algorithm"<<endl;
						cout<<"OFW null via (init subnet): origin="<<origin->origin->id<<", dest="<<dest->dest->id<<", at node="<<snode->id<<endl;
						break;
					}
					// 校验 slink 是否属于当前网络，避免野指针
					if (!TNM_IsKnownLinkInNet(this, slink))
					{
						cout<<"Invalid link pointer found when building initial path, skip."<<endl;
						break;
					}
					// Initialize flow along the shortest path for this OD (AON initialization)
                    slink->volume += dmd;
					path->path.push_back(slink);
					//test
					//cout<<slink->tail->id<<"-->"<<slink->head->id<<endl;
					//
					snode = slink->tail;
					
					
					
				}
				//
				if (!path->path.empty())
                {
                    path->flow = dest->assDemand;
                    dest->pathSet.push_back(path);
                }
                else
                {
                    TNM_SafeDeletePath(path);
                }
				//path->Print(true);
				//// system("PAUSE");
			}
		}
		//getchar();
		//cout<<"1.2"<<endl;
	}

}

void SCANLIST::SPTreeO(TNM_SNODE * rootNode)//given root, perform shortest path computation.
{

//insert the root into scanList, whatever data strucrue it uses.
    TNM_SNODE *curNode;
    InitRoot(rootNode);
    InsertANode(rootNode);
//  PrintList();
    curNode = GetNextNode();
    while (curNode != NULL)
            {
                // 运行期开关：true=无条件扩展；false=兼容旧逻辑（通过 m_isThrough/root 过滤）
                if (g_relax_sp_expand_all) {
                    curNode->SearchMinOutLink(this);
                } else {
                    if (curNode->m_isThrough || curNode == rootNode) curNode->SearchMinOutLink(this);
                }
                curNode = GetNextNode();
            }//end while 
}

void SCANLIST::SPTreeD(TNM_SNODE *rootNode)
{
    TNM_SNODE *curNode;
    //rootNode->tmpPathElem->cost = 0;
    InitRoot(rootNode);
    InsertANode(rootNode);
//  PrintList();
    curNode = GetNextNode();
    while (curNode != NULL)
    {
        // 运行期开关：true=无条件扩展；false=兼容旧逻辑（通过 m_isThrough/root 过滤）
        if (g_relax_sp_expand_all) {
            curNode->SearchMinInLink(this);
        } else {
            if (curNode->m_isThrough || curNode == rootNode) curNode->SearchMinInLink(this);
        }
        /*else
        {
            cout<<"node "<<curNode->id<<" is neither a through node, or a root node "<<endl;
        }
    */  curNode = GetNextNode();
    //  getchar();
    }//end while 
}

void SCANLIST::InitRoot(TNM_SNODE *root) 
{
	root->pathElem->cost = 0;
}

/*===========================================================================================
                              DEQUE 
  ===========================================================================================*/

SCAN_DEQUE::SCAN_DEQUE()
{
/*	status = new tinyInt[numOfNode];
	for (int i = 0;i<numOfNode;i++)
		status[i] = 0;*/

}

void SCAN_DEQUE::ClearList()
{
	nodeList.clear();
}

bool SCAN_DEQUE::InsertANode(TNM_SNODE *node)
{
	if(!node->SkipCentroid)
	{
	if(node->scanStatus == 0) // if never been used, insert it to the back of dq
	{
		nodeList.push_back(node); 
	    node->scanStatus = 1; // now being used
		return true;
	}
    else if (node->scanStatus == -1) // if it has ever been used, insert it to the front of dq
	{
		nodeList.push_front(node); 
	    node->scanStatus = 1; // now being used
		return true;
	}
	}
	return false;
}

TNM_SNODE *SCAN_DEQUE::GetNextNode()
{
   TNM_SNODE *node;
   if (!nodeList.empty()) //if the list is not empty
   {
   node = nodeList.front(); // get the current node
   nodeList.pop_front(); //delete it from the deque;
   node->scanStatus = -1; //mark it as been used but not in queue right now.
   return node;
   }
   else
	   return NULL;
}

SCAN_DEQUE::~SCAN_DEQUE()
{
	if(!nodeList.empty()) nodeList.clear();
	//cout<<"delete SCAN_DEQUE"<<endl;
}

void SCAN_DEQUE::PrintList()
{
	cout<<"Print List is under construction"<<endl;
}


/*===========================================================================================
                              QUEUE 
  ===========================================================================================*/

SCAN_QUEUE::SCAN_QUEUE()
{
/*	status = new tinyInt[numOfNode];
	for (int i = 0;i<numOfNode;i++)
		status[i] = 0;*/

}

void SCAN_QUEUE::ClearList()
{
	while(!nodeList.empty()) nodeList.pop();
}
bool SCAN_QUEUE::InsertANode(TNM_SNODE *node)
{
      if(node->scanStatus !=1)   
	  {
		  nodeList.push(node); 
		  node->scanStatus = 1;
		  return true;
	  }
	  else return false;
}

TNM_SNODE *SCAN_QUEUE::GetNextNode()
{
   TNM_SNODE *node;
   if (!nodeList.empty()) //if the list is not empty
   {
   node = nodeList.front(); // get the current node
   node->scanStatus = 0;
   nodeList.pop(); //delete it from the deque;
   return node;
   }
   else
	   return NULL;
	
}
SCAN_QUEUE::~SCAN_QUEUE()
{
	//nodeList.clear();
}
void SCAN_QUEUE::PrintList()
{
  cout<<"Print List is under costruction."<<endl;
}