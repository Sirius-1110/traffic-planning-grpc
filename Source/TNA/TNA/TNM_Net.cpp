#include "header/stdafx.h"

#ifdef _WIN32
#include "../../../Include/postgresql/libpq-fe.h"
#else
#include <libpq-fe.h>
#endif

#include <iostream>

#include <iomanip>

#include <string.h>

#include <math.h>

#include <cassert>

#include <cstdlib>

#include <stack>

#include <algorithm>

#include <set>

#include <map>

#include <vector>

namespace {
bool od_trace_pair(int o, int d)
{
	static int en = -1;
	if (en < 0) en = (std::getenv("TNA_GREEDY_OD_TRACE") != nullptr) ? 1 : 0;
	if (!en) return false;
	return (o == 20384 && d == 20393) || (o == 20393 && d == 20384);
}
} // namespace

#include <sstream>



//#include <my_predicate.h>

using namespace std;



static std::string w2u8(const wchar_t* ws)

{

	if (!ws) return std::string();

#ifdef _WIN32
	int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);

	std::string out;

	if (len > 0) {

		out.resize((size_t)len - 1);

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

}



smallInt TNM_SPATH::pathBufferSize = 0;

IDManager TNM_SPATH::idManager;



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

	

	for(PTRTRACE pv = forwStar.begin(); pv!= forwStar.end(); pv++)  

		if(*pv!=NULL) 

		{

			delete *pv;

			*pv = NULL;

		}

   for(PTRTRACE pv = backStar.begin(); pv!= backStar.end(); pv++)  

	   if(*pv!=NULL) 

	   {

		   delete *pv;

		   *pv = NULL;

	   }

	   if(!forwStar.empty()) forwStar.clear();

	   if(!backStar.empty()) backStar.clear();

  // cout<<"finish deleting links for node "<<id<<endl;

   if(buffer!=NULL)

   {

	   delete [] buffer;

	   buffer = NULL;

   }



   //cout<<"finish delete gui for node "<<id<<endl;

   delete pathElem;

   delete rPathElem;

  // cout<<"finish delete node "<<id<<endl;

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

    				list->InsertANode2(scanNode);

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

	id         =  0;

	orderID    = 0;

	type       =  BASLK;

	head       =  NULL;         /*starting node of the link*/

	tail       =  NULL;         /*ending node of the link */

	capacity   =  0.0;          /*link capacity*/

	volume     =  0.0;          /*link volume*/

	length     =  0.0;          /*link length*/

	ffs        =  0.0;          /*free flow speed*/

	fft        =  0.0;          /*free flow travel time*/

	cost       =  0;

	toll       =  0.0;

	fdCost     =  0.0;

	//m_classid  =  -1;   //no clasification. 

	markStatus =  0;

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

	if(tail==NULL) return;

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

	if(head==NULL) return;

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

		cout<<"Creating a new link: tail node pointer is invalid"<<endl;

		cout << "[INIT][FAIL] reason=tail_null link_id=" << id

             << " tail=NULL head=" << (head? head->id : -1)

             << " cap=" << capacity << " len=" << length << " ffs=" << ffs << endl;

		return false;

	}

	if(head == NULL)

	{

		cout<<"Creating a new link: head node pointer is invalid"<<endl;

		cout << "[INIT][FAIL] reason=head_null link_id=" << id

             << " tail=" << (tail? tail->id : -1) << " head=NULL"

             << " cap=" << capacity << " len=" << length << " ffs=" << ffs << endl;

		return false;

	}

	if(capacity<0)    

	{

		cout<<"Warning: negative cap found when constructing a link."<<endl;

		cout << "[INIT][FAIL] reason=negative_capacity link_id=" << id

             << " tail=" << (tail? tail->id : -1) << " head=" << (head? head->id : -1)

             << " cap=" << capacity << " len=" << length << " ffs=" << ffs << endl;

		return false;

	}

	if(length<0)    

	{

		cout<<"Warning: negative length found when constructing a link. length  = "<<length<<endl;

		cout << "[INIT][FAIL] reason=negative_length link_id=" << id

             << " tail=" << (tail? tail->id : -1) << " head=" << (head? head->id : -1)

             << " cap=" << capacity << " len=" << length << " ffs=" << ffs << endl;

		return false;

	}

	if(ffs<0)  

	{

		cout<<"Warning: negative speed found when constructing a link. speed = "<<ffs<<endl;

		cout << "[INIT][FAIL] reason=negative_ffs link_id=" << id

             << " tail=" << (tail? tail->id : -1) << " head=" << (head? head->id : -1)

             << " cap=" << capacity << " len=" << length << " ffs=" << ffs << endl;

		return false;

	}

	if(!CheckParallel()) {

        cout << "[INIT][FAIL] reason=parallel_or_duplicate link_id=" << id

             << " tail=" << (tail? tail->id : -1) << " head=" << (head? head->id : -1)

             << " cap=" << capacity << " len=" << length << " ffs=" << ffs << endl;

        return false;

    }

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

					cout<<"Parallel links found: link "<<link->id<<" and "<<id<<endl;

					return false;

				}

				if(link->id   == id)   

				{

					cout<<"Duplicate links found: link "<<link->id<<" existed "<<endl;

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

					cout<<"Parallel links found: link "<<link->id<<" and "<<id<<endl;

					return false;

				}

				if(link->id   == id)   

				{

					cout<<"Duplicate links found: link "<<link->id<<" existed "<<endl;

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

	//	if(m_classid == -1) t =  (GetCost_()  + GetDerCost_()*volume) * m_timeCostCoefficient + length * m_distCostCoefficient + mc; //sigle class case:

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

	//		case TT_NOTOLL:

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

	id = idManager.SelectANewID();

	idManager.RegisterID(id);

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

	idManager.UnRegisterID(id);

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



void TNM_SPATH::Print(bool node)

{

	int count = 0;

	if (this == NULL)

	{

		cout << "try to print a nonexist path" << endl;

		return;

	}

	cout << "\nPath ID = " << id << " Flow = " << flow << " Cost = " << cost << " size = " << path.size() << endl;

	PTRTRACE pv;

	for (pv = path.begin(); pv != path.end(); pv++)

	{

		count++;

		if (!node) cout << (*pv)->id << " -> ";

		else      cout << (*pv)->tail->id << " -> ";

		if (count % 8 == 0) cout << "\n";

	}



	if (node && !path.empty())

	{

		vector<TNM_SLINK*>::reverse_iterator pv = path.rbegin();

		cout << (*pv)->head->id;

	}

	cout << endl;

}



bool TNM_SORIGIN::SetDest(int id, TNM_SNODE *node, floatType demand)

{

	if(id>numOfDest||id<=0) 

	{

		cout<<"\n\tSetDest in Origin Object: dest index exceeds the rannge!"

			<<"\n\trequired index = "<<id<<endl;;

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

}



TNM_SDEST::~TNM_SDEST()

{

	dest->attachedDest--;

	EmptyPathSet();

	if(buffer!=NULL) 

	{

		delete [] buffer;

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

	for(PTRPATH pv = pathSet.begin(); pv!=pathSet.end(); pv++)

		delete *pv;

	//cout<<"terminatd a static dest object!"<<endl;

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

	ClearLastError();

}



TNM_SNET::~TNM_SNET()

{

	//cout<<"beging to delete tnm_snet object\n"<<endl;

	UnBuild();

	if(scanList) delete scanList;

	scanList = NULL;

}





void TNM_SNET::ClearLastError()

{

    last_error_code = 0;

    last_error_detail.clear();

}



void TNM_SNET::SetLastError(int code, const string& msg)

{

    last_error_code = code;

    last_error_detail = msg;

}



void TNM_SNET::RecordUnreachableOD(int originId, int destId)

{

    ostringstream oss;

    oss << "最短路径不可达: origin " << originId << " -> dest " << destId;

    string msg = oss.str();

    string j = string("{\"status\":0,\"stage\":\"assignment\",\"code\":11,\"message\":\"") + msg + "\"}";

    SetLastError(11, j);

}



int TNM_SNET::GetLastErrorCode() const

{

    return last_error_code;

}



const string& TNM_SNET::GetLastErrorDetail() const

{

    return last_error_detail;

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

		link->fdCost = link->GetDerCost(fToll);

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

		cout<<"\tInvalid size of link buffer array"<<endl;

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

			cout<<"\tCannot allocate memory for link buffer!"<<endl;

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

		cout<<"\tInvalid size of node buffer array"<<endl;

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

			cout<<"\tCannot allocate memory for node buffer!"<<endl;

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

		cout<<"\tInvalid size of path buffer array"<<endl;

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

					cout<<"\tCannot allocate memory for link buffer!"<<endl;

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

		cout<<"\tInvalid size of dest buffer array"<<endl;

		return 1;

     }

	else

	{

		destBufferSize = size;

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

			if(dest->buffer) delete [] dest->buffer;

			dest->buffer = new floatType[size];   

			if (dest->buffer == NULL)

			{

					cout<<"\tCannot allocate memory for dest buffer!"<<endl;

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

		cout<<"Undefined data structure for shortest path search"<<endl;

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

	 //if(CheckInitialStatus(false)==0) return 1;

	 EmptyPathSet();

	 initialStatus = 0; 

	 return 0;

 }



void TNM_SNET::EmptyPathSet()

{

	TNM_SORIGIN *origin;

	TNM_SDEST *dest;

	vector<TNM_SORIGIN*>::iterator po;

	for (po = originVector.begin();po!=originVector.end();po++)

	{

			origin =*po;

			for (int j = 0; j<origin->numOfDest;j++) {

				dest = origin->destVector[j];

				dest->EmptyPathSet();

			}

	}

}



void TNM_SNET::SetLinkCostScalar(floatType s)

{

	if(s<=0.01 || s> 99999)

	{

		cout<<"link cost scalar out of range"<<endl;

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

		if(noteBuilt) cout<<"\tNetwork "<<networkName<<" has been built.  You are not allowed to rebuild it"<<endl;

		break;

	default:

		cout<<"\tUnknown build status!"<<endl;

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

		//	cout<<"\n\tError: a "<<lval.type<<" link cannot be created. "<<endl;

			cout<<pType<<" is not a valid static link type"<<endl;

			return NULL;

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

			//cout << tStr << endl;

			nd = 0;

			if(!dvec.empty()) dvec.clear();

			if(!dmdvec.empty()) dmdvec.clear();

			while(tStr.compare("Origin") != 0 && !odFile.eof()) //not

			{

				if(TNM_FromString<int>(destID, tStr, std::dec))

				{

					//cout << "destid is " << destID << endl;

					odFile>>tStr;

					//cout << tStr<<endl;

					odFile>>tStr;

					//cout << tStr<<endl;

					TNM_FromString<floatType>(dmd, tStr, std::dec);

					//cout << "demand is " << dmd << endl;

					odFile>>tStr;

					//cout << tStr << endl;

					//system("PAUSE");

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

	if(safe) {

		// 添加边界检查

		if (id <= 0 || id > nodeVector.size()) {

			cout << "Warning: Invalid node ID " << id << ", nodeVector size: " << nodeVector.size() << endl;

			return NULL;

		}

		return nodeVector[id - 1];

	}

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



//shortest path calculation for an OD pair using the one-to-one Dijkstra algorithm

TNM_SPATH* TNM_SNET::SPath(TNM_SNODE* origin, TNM_SNODE* dest)

{

	TNM_SPATH* path;

	UpdateSPR(origin, dest);

	//cout << "1" << endl;

	path = GetSPath_R_(origin, dest);

	//cout << origin->id << "-->" << dest->id << endl;

	//cout << "2" << endl;

	if (path == NULL)

	{

		RecordUnreachableOD(origin->id, dest->id);

		cout << "Cannot find shortest path between origin " << origin->id

			<< " and dest " << dest->id << endl;

		cout << "\tPossible reasons:\n1. Network is not connected.\n2.Link cost has extremely large value.\n3.For TAPAS format, the first through node setting in _net file may be inappropriate."

			<< endl;

		return NULL;

	}

	else return path;

}



/*=====================================================================================

   Internal function.

   retrieve a path from a shortest path tree rooted at dest

  ===================================================================================*/

TNM_SPATH* TNM_SNET::GetSPath_R_(TNM_SNODE* origin, TNM_SNODE* dest)

{

	TNM_SNODE* node;

	TNM_SLINK* link;

	TNM_SPATH* path = new TNM_SPATH; //allocate memory for new path

	int count = 0;

	assert(path != 0);

	node = origin;

	while (node != dest)

	{

		//	cout<<"\tcurrent node = "<<node->id<<endl;

		link = node->pathElem->via;

		if (link == NULL) //there may exist no shortest path at all, or there must be sth wrong.

		{

			cout << "\tno shortest path found at node " << node->id << endl;

			delete path;

			return NULL;

		}

		node = link->head;

		path->path.push_back(link);

		count++;

		if (count > numOfNode)

		{

			cout << "Error: shortest path tree contains a cycle!" << endl;

			cout << "Report: origin = " << origin->id << " dest = " << dest->id << " path be generated so far " << endl;

			path->Print(true);

			delete path;

			return NULL;

		}

	}

	//vector<TNM_SLINK*>(path->path).swap(path->path);

	path->cost = origin->pathElem->cost;

	//path->Print();

	return path;

}



//shortest path calculation provided address of a given destination

void TNM_SNET::UpdateSPR(TNM_SNODE* rootNode)

{

	TNM_SNODE* node;

	for (int j = 0; j < numOfNode; j++)

	{

		node = nodeVector[j];

		node->InitPathElem();

		node->scanStatus = 0;

	}

	scanList->SPTreeD(rootNode);//compute shortest path;

}



//shortest path calculation provided address of a given destination

void TNM_SNET::UpdateSPR(TNM_SNODE* origin, TNM_SNODE* dest)

{

	TNM_SNODE* node;

	for (int j = 0; j < numOfNode; j++)

	{

		node = nodeVector[j];

		node->InitPathElem();

		node->scanStatus = 0;

	}

	scanList->SPTreeD(origin,dest);//compute shortest path;

}



void TNM_SNET::InitialSubNet3(int kx, bool createPath)

{

	TNM_SORIGIN *origin;

	for (int i = 0;i<numOfOrigin; i++)

	{

		origin = originVector[i];



		//cout << "origin id is " << origin->origin->id << endl;

		

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

					slink->volume+=dmd;

					if(slink == NULL) 

					{

						cout<<"The link is null in calculating the direction for OFW algorithm"<<endl;

						break;

					}

					path->path.push_back(slink);

					//test

					//cout<<slink->tail->id<<"-->"<<slink->head->id<<endl;

					//

					snode = slink->tail;

					

					

				}

				//

				path->flow = dest->assDemand;

				dest->pathSet.push_back(path);

				//path->Print(true);

				//system("PAUSE");

			}

		}

		//getchar();

		//cout<<"1.2"<<endl;

	}



}



bool TNM_SNET::InitialSubNet4(int kx, bool createPath)

{

	TNM_SORIGIN* origin;

	for (int i = 0; i < numOfOrigin; i++)

	{

		origin = originVector[i];



		//cout << "origin id is " << origin->origin->id << endl;



		//UpdateSP(origin->origin);



		//add path

		//{



			for (int j = 0; j < origin->numOfDest; j++)

			{

				TNM_SDEST* dest = origin->destVector[j];

				if (origin->origin == dest->dest) continue;

				if (dest->assDemand == 0.0) continue;

				double dmd = dest->assDemand;

				TNM_SPATH* path;



				path = SPath(origin->origin, dest->dest);

				if (path == NULL)

					return false;



				path->flow = dest->assDemand;

				//

				TNM_SLINK* slink;

				for (int pi = 0; pi < path->path.size(); pi++)

				{

					slink = path->path[pi];

					slink->volume += dest->assDemand;

					//slink->cost = slink->GetCost();

					//slink->fdCost = slink->GetDerCost();



					//cout << "link " << slink->id << "  :  " << slink->volume << "  " << slink->cost << "  " << slink->fdCost << endl;

				}

				//



				dest->pathSet.push_back(path);

				if (od_trace_pair(origin->origin->id, dest->dest->id))
				{
					std::cout << "[OD_TRACE] InitialSubNet4 OD(" << origin->origin->id << "->"
					          << dest->dest->id << ") demand=" << dmd << " initial_path nlinks="
					          << path->path.size() << " pathSet.size=" << dest->pathSet.size()
					          << std::endl;
				}

				//path->Print(true);

				//system("PAUSE");

			}

		//}

		//getchar();

		//cout<<"1.2"<<endl;

	}

	return true;

}





void SCANLIST::SPTreeO(TNM_SNODE * rootNode)//given root, perform shortest path computation.

{



//insert the root into scanList, whatever data strucrue it uses.

	TNM_SNODE *curNode;

	//nodeList.clear();

	InitRoot(rootNode);

	InsertANode(rootNode);

//	PrintList();

	curNode = GetNextNode();

	while (curNode != NULL)

			{

				if(curNode->m_isThrough || curNode == rootNode) curNode->SearchMinOutLink(this);

				curNode = GetNextNode();

			}//end while	

}



void SCANLIST::SPTreeD(TNM_SNODE* origin, TNM_SNODE *dest)

{

	//cout << "o is " << origin->id << "  d is " << dest->id << endl;

	TNM_SNODE *curNode;

	//rootNode->tmpPathElem->cost = 0;

	nodeList.clear();

	InitRoot(dest);

	InsertANode(dest);

	/*for (size_t i = 0; i < nodeList.size(); i++)

	{

		cout << nodeList[i]->id << " ; ";

	}

	system("PAUSE");*/

	curNode = GetNextNodeMin();

	while (curNode != NULL && curNode != origin)

	{

		if (curNode->m_isThrough || curNode == dest)

			curNode->SearchMinInLink(this);

		curNode = GetNextNodeMin();

	}//end while	

}



void SCANLIST::SPTreeD(TNM_SNODE* rootNode)

{

	TNM_SNODE* curNode;

	//rootNode->tmpPathElem->cost = 0;

	InitRoot(rootNode);

	InsertANode(rootNode);

	//	PrintList();

	curNode = GetNextNode();

	while (curNode != NULL)

	{

		//cout<<"Scan node "<<curNode->id<<endl;

		if (curNode->m_isThrough || curNode == rootNode) curNode->SearchMinInLink(this);

		/*else

		{

			cout<<"node "<<curNode->id<<" is neither a through node, or a root node "<<endl;

		}

	*/	curNode = GetNextNode();

	//	getchar();

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



bool SCAN_DEQUE::InsertANode2(TNM_SNODE* node)

{

	if (!node->SkipCentroid)

	{

		if (node->scanStatus == 0) // if never been used, insert it to the back of dq

		{

			nodeList.push_back(node);

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



TNM_SNODE* SCAN_DEQUE::GetNextNodeMin()

{

	TNM_SNODE* node;

	if (!nodeList.empty()) //if the list is not empty

	{//test

		/*for (size_t i = 0; i < nodeList.size(); i++)

		{

			cout <<nodeList[i]->id<<"--> "<< nodeList[i]->pathElem->cost << " ; ";

		}

		cout << endl;*/

		QuickSortNode(0, nodeList.size() - 1);

		//test

		/*for (size_t i = 0; i < nodeList.size(); i++)

		{

			cout << nodeList[i]->id << " ; ";

		}

		system("PAUSE");*/

		//

		node = nodeList.front(); // get the current node

		nodeList.pop_front(); //delete it from the deque;

		node->scanStatus = -1; //mark it as been used but not in queue right now.

		return node;

	}

	else

		return NULL;

}



//sort the path set according to the path's cost fro min to max

void SCAN_DEQUE::QuickSortNode( int low, int high)

{

	if (low < high)

	{

		TNM_SNODE* snode = nodeList[low];

		int i = low;

		int j = high;

		while (i < j)

		{

			while ((i < j) && (nodeList[j]->pathElem->cost >= snode->pathElem->cost))

			{

				j = j - 1;

			}

			nodeList[i] = nodeList[j];

			while ((i < j) && (nodeList[i]->pathElem->cost <= snode->pathElem->cost))

			{

				i = i + 1;

			}

			nodeList[j] = nodeList[i];

		}

		nodeList[i] = snode;



		QuickSortNode(low, i - 1);

		QuickSortNode(i + 1, high);

	}

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

	cout<<"Print List is under construction."<<endl;

}


int TNM_SNET::BuildPostgreSQL(bool loadod, TNM_LINKTYPE lpf, const string& dbConnStr, const string& networkTableName, const string& odTableName)

{

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    cout << "[DEBUG] Begin to build the network using data from PostgreSQL..." << endl;

    ClearLastError();

    auto esc_json = [&](const string& s){ string o; o.reserve(s.size()*2); for (size_t i=0;i<s.size();++i){ char c=s[i]; switch(c){ case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break; default: o.push_back(c); } } return o; };

    auto mkerr = [&](int code, const string& stage, const string& msg, const string& extra)->int{

        string j = string("{\"status\":0,\"stage\":\"") + stage + "\",\"code\":" + to_string(code) + ",\"message\":\"" + esc_json(msg) + "\"" + (extra.empty()? string("") : string(",") + extra) + "}";

        SetLastError(code, j);

        return code;

    };

    if (CheckBuildStatus(true) != 0) return mkerr(7, "precheck", "network already built or status invalid", "");

    NODE_VALCONTAINER nval;

    LINK_VALCONTAINER lval;



    long long tail, head;

    floatType cap, len, fft, t, ffs, B, P, spd;

    vector<string> words;

    int lid = 0;

    int numod, numlink = 0;

    long long baseMaxNodeId = 0; // 最大道路节点ID（排除质心连杆 type=10）

    map<int, int> zone2centroid;  // 小区ID -> 质心新节点ID 映射（来自 type=10 连杆）

    map<int, int> zone2anchor;    // 小区ID -> 锚定的道路节点ID（type=10 连杆的另一端）



    //连接数据库

    //cout << "[DEBUG]  - Connecting to PostgreSQL: " << dbConnStr << endl;

    

    // 检查连接字符串是否为空

    if (dbConnStr.empty()) {

        cout << "Error: Database connection string is empty!" << endl;

        return mkerr(1, "input", "dbConnStr is empty", "");

    }



    // 尝试创建一个简单的连接

    //PGconn* conn = NULL;

    PGconn* conn = PQconnectdb(dbConnStr.c_str());



    // 首先尝试一个简单的连接字符串测试

    //cout << "[DEBUG] : Testing basic connection..." << endl;

    

    // 使用更简单的连接方式

    const char* conninfo = dbConnStr.c_str();

    //cout << "[DEBUG] : Connection string: " << conninfo << endl;

    



    

        if (conn == NULL) {

            cout << "Error: Failed to create database connection object!" << endl;

            cout << "This usually means PostgreSQL client library is not properly linked or DLL not found." << endl;

            return mkerr(1, "connect", "Failed to create database connection object", "");

        }

        

    cout << "PQstatus(conn) = " << PQstatus(conn) << endl;

    if (PQstatus(conn) == CONNECTION_BAD) {

        cout << "Connection to database failed: " << PQerrorMessage(conn) << endl;

        string em = PQerrorMessage(conn)? string(PQerrorMessage(conn)) : string("CONNECTION_BAD");

        PQfinish(conn);

        return mkerr(1, "connect", em, "");

    }



    // 使用动态表名构建SQL查询语句（优先环境变量）

    const char* env_net = getenv("PG_TABLE_NET");

    const char* env_od  = getenv("PG_TABLE_OD");

    string chosen_net = (env_net && *env_net) ? string(env_net) : networkTableName;

    string chosen_od  = (env_od  && *env_od)  ? string(env_od)  : odTableName;

    cout << "[DEBUG] Using tables: net='" << chosen_net << "' od='" << chosen_od << "'" << endl;



    const char* env_scn = getenv("PG_SCENARIO_PREFIX");

    std::string scenarioPrefix = (env_scn && *env_scn) ? std::string(env_scn) : std::string();

    auto trim_copy2 = [](std::string s){ while(!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin()); while(!s.empty() && (s.back()==' '||s.back()=='\t')) s.pop_back(); return s; };

    auto extractSchemaFromConn2 = [&](const std::string& connStr)->std::string{

        size_t p = connStr.find("role=");

        if (p != std::string::npos) {

            p += 5;

            size_t q = p;

            while (q < connStr.size() && connStr[q] != ' ' && connStr[q] != '\'' && connStr[q] != '"') q++;

            std::string role = connStr.substr(p, q - p);

            while(!role.empty() && (role.back()=='\''||role.back()=='"')) role.pop_back();

            return trim_copy2(role);

        }

        return std::string();

    };

    struct SchemaTable2 { std::string schema; std::string table; };

    auto splitSchemaTable2 = [&](const std::string& qname) -> SchemaTable2 {

        SchemaTable2 r;

        size_t dot = qname.find('.');

        if (dot == std::string::npos) { r.schema = std::string(); r.table = qname; }

        else { r.schema = qname.substr(0, dot); r.table = qname.substr(dot + 1); }

        return r;

    };

    auto table_exists2 = [&](const std::string& qname)->bool{

        SchemaTable2 st = splitSchemaTable2(qname);

        std::string q;

        if (!st.schema.empty()) q = "SELECT 1 FROM information_schema.tables WHERE table_schema='" + st.schema + "' AND table_name='" + st.table + "' LIMIT 1";

        else q = "SELECT 1 FROM information_schema.tables WHERE table_schema=current_schema() AND table_name='" + st.table + "' LIMIT 1";

        PGresult* r = PQexec(conn, q.c_str());

        bool ok = (r && PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)>0);

        if (r) PQclear(r);

        return ok;

    };

    auto column_exists2 = [&](const std::string& qname, const std::string& col)->bool{

        SchemaTable2 st = splitSchemaTable2(qname);

        std::string q;

        if (!st.schema.empty()) q = "SELECT 1 FROM information_schema.columns WHERE table_schema='" + st.schema + "' AND table_name='" + st.table + "' AND column_name='" + col + "' LIMIT 1";

        else q = "SELECT 1 FROM information_schema.columns WHERE table_schema=current_schema() AND table_name='" + st.table + "' AND column_name='" + col + "' LIMIT 1";

        PGresult* r = PQexec(conn, q.c_str());

        bool ok = (r && PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)>0);

        if (r) PQclear(r);

        return ok;

    };

    auto normalize_prefix2 = [&](std::string p)->std::string{ p = trim_copy2(p); while(!p.empty() && p.back()=='_') p.pop_back(); return p; };

    // 为 SQL 中的表名加双引号，支持 schema.table 和纯表名（含以数字开头的情况）
    auto pg_quote_ident = [](const std::string& name) -> std::string {
        size_t dot = name.find('.');
        if (dot != std::string::npos)
            return "\"" + name.substr(0, dot) + "\".\"" + name.substr(dot + 1) + "\"";
        return "\"" + name + "\"";
    };



    bool od_direct_mode = false;

    #ifdef _WIN32
    _putenv_s("TNA_OD_MODE", "");
#else
    setenv("TNA_OD_MODE", "", 1);
#endif

    std::string normalized_prefix = normalize_prefix2(scenarioPrefix);

    int existing_centroid_links = 0;
    {
        std::string qcnt = "SELECT COUNT(*)::int FROM " + pg_quote_ident(chosen_net) + " WHERE \"type\" = 10";
        PGresult* rcnt = PQexec(conn, qcnt.c_str());
        if (rcnt && PQresultStatus(rcnt) == PGRES_TUPLES_OK && PQntuples(rcnt) > 0)
            existing_centroid_links = atoi(PQgetvalue(rcnt, 0, 0));
        if (rcnt) PQclear(rcnt);
    }

    auto fail_centroid_and_return = [&](const std::string& msg, const std::string& extra)->int {
        std::cout << "[TNA_DLL][CentroidPrebuild] ERROR: " << msg << std::endl;
        PQfinish(conn);
        return mkerr(10, "centroid_prebuild", msg, extra);
    };

    // 硬性校验：无论是否已有 type=10，road_way 必须有 geometry 列且每条路段 geometry 非 NULL
    {
        std::string wayTable = chosen_net;
        if (!column_exists2(wayTable, "geometry"))
            return fail_centroid_and_return("质心连杆失败：路网表缺少 geometry 列", "");
        int geom_null_cnt = 0;
        std::string qnull = "SELECT COUNT(*)::int FROM " + pg_quote_ident(wayTable) + " WHERE geometry IS NULL";
        PGresult* rnull = PQexec(conn, qnull.c_str());
        if (rnull && PQresultStatus(rnull) == PGRES_TUPLES_OK && PQntuples(rnull) > 0)
            geom_null_cnt = atoi(PQgetvalue(rnull, 0, 0));
        if (rnull) PQclear(rnull);
        if (geom_null_cnt > 0) {
            std::string extra = "\"geom_null_rows\":" + std::to_string(geom_null_cnt);
            return fail_centroid_and_return(
                "质心连杆失败：路网表存在 geometry 为 NULL 的路段（共 " + std::to_string(geom_null_cnt)
                    + " 条），请补全几何或重建质心连杆",
                extra);
        }
    }

    if (!normalized_prefix.empty()) {

        SchemaTable2 stn = splitSchemaTable2(chosen_net);

        std::string schema = stn.schema;

        if (schema.empty()) schema = extractSchemaFromConn2(dbConnStr);

        std::string communityBase = normalized_prefix + "_road_community";

        std::string pointBase = normalized_prefix + "_road_point";

        std::string communityTable = schema.empty() ? communityBase : (schema + "." + communityBase);

        std::string pointTable = schema.empty() ? pointBase : (schema + "." + pointBase);

        std::cout << "[TNA_DLL][CentroidPrebuild] centroid source: ST_Centroid(" << communityTable << ")\n";
        std::string zones_cte =
            "zones AS (\n"
            "  SELECT ra.area_id::bigint AS area_id, ST_Centroid(ra.\"geometry\") AS gc\n"
            "  FROM " + pg_quote_ident(communityTable) + " ra\n"
            "  WHERE ra.\"geometry\" IS NOT NULL\n"
            "),\n";

        std::string wayTable = chosen_net;

    const char* centroid_mode_env = getenv("TNA_CENTROID_CONNECTOR_MODE");
    bool multi_osm_mode = (centroid_mode_env && std::string(centroid_mode_env) == "multi_osm");
    int max_connectors_per_zone = 5;
    const char* max_conn_env = getenv("TNA_CENTROID_MAX_CONNECTORS");
    if (max_conn_env && *max_conn_env) {
        max_connectors_per_zone = atoi(max_conn_env);
        if (max_connectors_per_zone < 1) max_connectors_per_zone = 1;
        if (max_connectors_per_zone > 20) max_connectors_per_zone = 20;
    }

    bool skip_centroid_rebuild = false;
    const char* preserve_centroid_env = getenv("TNA_PRESERVE_CENTROID_LINKS");
    bool preserve_centroid_links = (preserve_centroid_env && std::string(preserve_centroid_env) == "1");
    const char* skip_prebuild_env = getenv("TNA_SKIP_CENTROID_PREBUILD");
    if (skip_prebuild_env && std::string(skip_prebuild_env) == "1") {
        if (existing_centroid_links <= 0) {
            return fail_centroid_and_return(
                "构网跳过质心连杆：路网中无 type=10，请先调用 build_centroid_connectors",
                "");
        }
        skip_centroid_rebuild = true;
        std::cout << "[TNA_DLL][CentroidPrebuild] skip_centroid_prebuild=1: reuse type=10 links="
                  << existing_centroid_links << std::endl;
#ifdef _WIN32
        _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
        setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
    } else if (preserve_centroid_links && existing_centroid_links > 0) {
        skip_centroid_rebuild = true;
        std::cout << "[TNA_DLL][CentroidPrebuild] preserve: keep OD-estimation type=10 links="
                  << existing_centroid_links << std::endl;
#ifdef _WIN32
        _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
        setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
    } else if (existing_centroid_links > 0 && !multi_osm_mode) {
        int max_per_zone = 0;
        {
            std::string qmax =
                "SELECT COALESCE(MAX(cnt), 0)::int FROM ("
                "SELECT COUNT(*)::int AS cnt FROM " + pg_quote_ident(wayTable) +
                " WHERE \"type\" = 10 GROUP BY centroid_matched_node) s";
            PGresult* rmax = PQexec(conn, qmax.c_str());
            if (rmax && PQresultStatus(rmax) == PGRES_TUPLES_OK && PQntuples(rmax) > 0)
                max_per_zone = atoi(PQgetvalue(rmax, 0, 0));
            if (rmax) PQclear(rmax);
        }
        if (max_per_zone <= 2) {
            skip_centroid_rebuild = true;
            std::cout << "[TNA_DLL][CentroidPrebuild] skip: existing type=10 links=" << existing_centroid_links << std::endl;
#ifdef _WIN32
            _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
            setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
        } else {
            std::cout << "[TNA_DLL][CentroidPrebuild] rebuild: multi-connector residue detected"
                      << " max_per_zone=" << max_per_zone
                      << " total_type10=" << existing_centroid_links << std::endl;
        }
    }

    if (!skip_centroid_rebuild) {
            if (multi_osm_mode && existing_centroid_links > 0) {
                std::cout << "[TNA_DLL][CentroidPrebuildMulti] rebuild: existing type=10 links="
                          << existing_centroid_links << " (multi_osm)" << std::endl;
            } else {
                std::cout << "[TNA_DLL][CentroidPrebuild] enabled. scenario_prefix='" << normalized_prefix << "'" << std::endl;
            }

            if (!table_exists2(communityTable))
                return fail_centroid_and_return("质心连杆失败：缺少交通小区表 " + communityTable, "");
            if (!table_exists2(pointTable))
                return fail_centroid_and_return("质心连杆失败：缺少路网节点表 " + pointTable, "");
            if (!column_exists2(wayTable, "type") || !column_exists2(wayTable, "centroid_matched_node"))
                return fail_centroid_and_return("质心连杆失败：路网表缺少 type 或 centroid_matched_node 字段", "");
            if (!column_exists2(communityTable, "geometry"))
                return fail_centroid_and_return("质心连杆失败：交通小区表缺少 geometry 列", "");
            if (!column_exists2(pointTable, "geometry"))
                return fail_centroid_and_return("质心连杆失败：路网节点表缺少 geometry 列", "");
            if (!column_exists2(wayTable, "geometry"))
                return fail_centroid_and_return("质心连杆失败：路网表缺少 geometry 列，无法写入质心连杆", "");

            std::string delWay = "DELETE FROM " + pg_quote_ident(wayTable) + " WHERE \"type\" = 10;";
            PGresult* rdel = PQexec(conn, delWay.c_str());
            bool del_ok = (rdel && PQresultStatus(rdel) == PGRES_COMMAND_OK);
            if (rdel) PQclear(rdel);
            if (!del_ok) {
                std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
                return fail_centroid_and_return("质心连杆失败：清理路网表中旧质心连杆失败", "\"pg_error\":\"" + esc_json(em) + "\"");
            }

            std::string ins;
            if (multi_osm_mode) {
                std::string osm_key_expr = column_exists2(wayTable, "link_osmid")
                    ? "COALESCE(NULLIF(w.\"link_osmid\"::text, ''), w.\"link_id\"::text)"
                    : "w.\"link_id\"::text";
                std::string max_conn_str = std::to_string(max_connectors_per_zone);
                ins =
                    "WITH bm AS (SELECT GREATEST("
                    "  COALESCE(MAX(init_node),0),"
                    "  COALESCE(MAX(term_node),0),"
                    "  COALESCE((SELECT MAX(node_id) FROM " + pg_quote_ident(pointTable) + "),0)"
                    ") AS base_max FROM " + pg_quote_ident(wayTable) + " WHERE \"type\" <> 10),\n"
                    "lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM " + pg_quote_ident(wayTable) + "),\n"
                    + zones_cte +
                    "link_candidates AS (\n"
                    "  SELECT z.area_id, w.link_id, w.init_node, w.term_node, w.geometry AS link_geom,\n"
                    "         w.\"type\"::int AS road_type,\n"
                    "         " + osm_key_expr + " AS osm_key,\n"
                    "         ST_Distance(ST_Transform(z.gc, 3857), ST_Transform(w.geometry, 3857)) AS dist_m,\n"
                    "         z.gc,\n"
                    "         CASE WHEN ST_Distance(ST_Transform(z.gc,3857), ST_Transform(ST_StartPoint(w.geometry),3857))\n"
                    "                   <= ST_Distance(ST_Transform(z.gc,3857), ST_Transform(ST_EndPoint(w.geometry),3857))\n"
                    "              THEN w.init_node ELSE w.term_node END AS anchor_node\n"
                    "  FROM zones z\n"
                    "  CROSS JOIN bm\n"
                    "  JOIN " + pg_quote_ident(wayTable) + " w ON w.\"type\" IN (1,2,3) AND w.geometry IS NOT NULL\n"
                    "),\n"
                    "link_dedup AS (\n"
                    "  SELECT DISTINCT ON (area_id, osm_key)\n"
                    "         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node, road_type\n"
                    "  FROM link_candidates\n"
                    "  ORDER BY area_id, osm_key, dist_m\n"
                    "),\n"
                    "anchor_dedup AS (\n"
                    "  SELECT DISTINCT ON (area_id, anchor_node)\n"
                    "         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node, road_type\n"
                    "  FROM link_dedup\n"
                    "  ORDER BY area_id, anchor_node, dist_m\n"
                    "),\n"
                    "mandatory_by_type AS (\n"
                    "  SELECT DISTINCT ON (area_id, road_type)\n"
                    "         area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node, road_type\n"
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
                    "  SELECT area_id, link_id, init_node, term_node, link_geom, dist_m, gc, osm_key, anchor_node\n"
                    "  FROM mandatory_by_type\n"
                    "  UNION ALL\n"
                    "  SELECT e.area_id, e.link_id, e.init_node, e.term_node, e.link_geom, e.dist_m, e.gc, e.osm_key, e.anchor_node\n"
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
                    "INSERT INTO " + pg_quote_ident(wayTable) + " (link_id, init_node, term_node, capacity, b, power, toll, \"type\", fft, speedlimit, length, geometry, centroid_matched_node)\n"
                    "SELECT (lm.link_max + row_number() OVER ())::bigint AS link_id, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node\n"
                    "FROM both_dirs, lm;";
            } else {
            ins =
                "WITH bm AS (SELECT GREATEST("
                "  COALESCE(MAX(init_node),0),"
                "  COALESCE(MAX(term_node),0),"
                "  COALESCE((SELECT MAX(node_id) FROM " + pg_quote_ident(pointTable) + "),0)"
                ") AS base_max FROM " + pg_quote_ident(wayTable) + " WHERE \"type\" <> 10),\n"
                "lm AS (SELECT COALESCE(MAX(link_id),0) AS link_max FROM " + pg_quote_ident(wayTable) + "),\n"
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
                "    SELECT node_id, geometry FROM " + pg_quote_ident(pointTable) + " ORDER BY z.gc <-> geometry LIMIT 1\n"
                "  ) AS n\n"
                "),\n"
                "both_dirs AS (\n"
                "  SELECT * FROM base\n"
                "  UNION ALL\n"
                "  SELECT term_node AS init_node, init_node AS term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node FROM base\n"
                ")\n"
                "INSERT INTO " + pg_quote_ident(wayTable) + " (link_id, init_node, term_node, capacity, b, power, toll, \"type\", fft, speedlimit, length, geometry, centroid_matched_node)\n"
                "SELECT (lm.link_max + row_number() OVER ())::bigint AS link_id, init_node, term_node, capacity, b, power, toll, type, fft, speedlimit, length, geometry, centroid_matched_node\n"
                "FROM both_dirs, lm;";
            }

            PGresult* rins = PQexec(conn, ins.c_str());
            bool ins_ok = (rins && PQresultStatus(rins) == PGRES_COMMAND_OK);
            std::string em = PQerrorMessage(conn) ? std::string(PQerrorMessage(conn)) : std::string();
            if (rins) PQclear(rins);
            if (!ins_ok)
                return fail_centroid_and_return("质心连杆失败：写入路网表失败", "\"pg_error\":\"" + esc_json(em) + "\"");

            existing_centroid_links = 0;
            PGresult* rcnt2 = PQexec(conn, ("SELECT COUNT(*)::int FROM " + pg_quote_ident(wayTable) + " WHERE \"type\" = 10").c_str());
            if (rcnt2 && PQresultStatus(rcnt2) == PGRES_TUPLES_OK && PQntuples(rcnt2) > 0)
                existing_centroid_links = atoi(PQgetvalue(rcnt2, 0, 0));
            if (rcnt2) PQclear(rcnt2);
            if (existing_centroid_links <= 0)
                return fail_centroid_and_return("质心连杆失败：生成后 type=10 连杆数量为 0，请检查小区面与节点几何是否有效", "");

#ifdef _WIN32
            _putenv_s("TNA_OD_MODE", multi_osm_mode ? "centroid_connector_multi_osm_prebuilt" : "centroid_connector_prebuilt");
#else
            setenv("TNA_OD_MODE", multi_osm_mode ? "centroid_connector_multi_osm_prebuilt" : "centroid_connector_prebuilt", 1);
#endif
            std::cout << "[TNA_DLL][CentroidPrebuild] ok: mode="
                      << (multi_osm_mode ? "multi_osm" : "single")
                      << " type=10 links=" << existing_centroid_links << std::endl;
        }

    } else if (existing_centroid_links <= 0) {

        return fail_centroid_and_return("质心连杆失败：未提供方案前缀 PG_SCENARIO_PREFIX，且路网中无既有质心连杆", "");

    } else {

#ifdef _WIN32
        _putenv_s("TNA_OD_MODE", "centroid_connector_existing");
#else
        setenv("TNA_OD_MODE", "centroid_connector_existing", 1);
#endif
    }



    // 显式列查询（仅消费既有字段）

    string sql_network =

        string("SELECT \"init_node\" AS head, \"term_node\" AS tail, ") +

        "\"capacity\", \"length\" AS length, \"fft\", \"b\", \"power\" AS power, \"speedlimit\", \"toll\", \"link_id\", \"type\", \"centroid_matched_node\" FROM " + pg_quote_ident(chosen_net);



    // OD 表显式列并排序，确保同一 org 连续，列顺序（3列）：f_id, t_id, demand

    string sql_od =

        string("SELECT \"f_id\" AS org, \"t_id\" AS dest, \"demand\" FROM ") + pg_quote_ident(chosen_od) + " ORDER BY \"f_id\", \"t_id\"";

    

    std::cout << w2u8(L"[DEBUG] 执行SQL网络查询：") << sql_network << std::endl;

    PGresult* res1 = PQexec(conn, sql_network.c_str());

    if (PQresultStatus(res1) != PGRES_TUPLES_OK) {

        std::cout << w2u8(L"[DEBUG] SQL错误（网络）：") << PQerrorMessage(conn) << std::endl;

        if (res1) PQclear(res1);

        PQfinish(conn);

        string extra = string("\"details\":{\"network_table\":\"") + esc_json(chosen_net) + "\"}";

        string em = PQerrorMessage(conn)? string(PQerrorMessage(conn)) : string("PGRES not tuples ok");

        return mkerr(1, "query_network", em, extra);

    }

    std::cout << w2u8(L"[DEBUG] SQL网络查询执行成功。") << PQntuples(res1) << w2u8(L" 行返回。") << std::endl;



    int idx_head = PQfnumber(res1, "head");

    int idx_tail = PQfnumber(res1, "tail");

    int idx_capacity = PQfnumber(res1, "capacity");

    int idx_length = PQfnumber(res1, "length");

    int idx_fft = PQfnumber(res1, "fft");

    int idx_b = PQfnumber(res1, "b");

    int idx_power = PQfnumber(res1, "power");

    int idx_speedlimit = PQfnumber(res1, "speedlimit");

    int idx_toll = PQfnumber(res1, "toll");

    int idx_link_id = PQfnumber(res1, "link_id");

    int idx_type = PQfnumber(res1, "type");

    int idx_cmn  = PQfnumber(res1, "centroid_matched_node");

    if (idx_tail<0 || idx_head<0 || idx_capacity<0 || idx_length<0 || idx_fft<0 || idx_b<0 || idx_power<0 || idx_speedlimit<0 || idx_toll<0 || idx_link_id<0 || idx_type<0 || idx_cmn<0) {

        std::cout << w2u8(L"[DEBUG] SQL错误：意外的列名或别名。") << std::endl;

        PQclear(res1);

        PQfinish(conn);

        string extra = string("\"details\":{\"network_table\":\"") + esc_json(chosen_net) + "\"}";

        return mkerr(1, "schema_network", "unexpected column names or aliases", extra);

    }



    std::cout << w2u8(L"[DEBUG] 执行SQL OD查询：") << sql_od << std::endl;

    PGresult* res2 = PQexec(conn, sql_od.c_str());

    if (PQresultStatus(res2) != PGRES_TUPLES_OK) {

        std::cout << w2u8(L"[DEBUG] SQL错误（OD）：") << PQerrorMessage(conn) << std::endl;

        PQclear(res1);

        PQclear(res2);

        PQfinish(conn);

        string extra = string("\"details\":{\"od_table\":\"") + esc_json(chosen_od) + "\"}";

        string em = PQerrorMessage(conn)? string(PQerrorMessage(conn)) : string("PGRES not tuples ok");

        return mkerr(1, "query_od", em, extra);

    }

    std::cout << w2u8(L"[DEBUG] SQL OD查询执行成功。") << PQntuples(res2) << w2u8(L" 行返回。") << std::endl;



    {

        if (od_direct_mode) {

            // keep this precheck only for direct road-node OD mode

        }

        string q_missing =

            string("WITH net_nodes AS (\n")

            + "  SELECT \"init_node\"::bigint AS nid FROM " + pg_quote_ident(chosen_net) + " WHERE \"init_node\" IS NOT NULL\n"

            + "  UNION\n"

            + "  SELECT \"term_node\"::bigint AS nid FROM " + pg_quote_ident(chosen_net) + " WHERE \"term_node\" IS NOT NULL\n"

            + "), od_nodes AS (\n"

            + "  SELECT \"f_id\"::bigint AS nid FROM " + pg_quote_ident(chosen_od) + " WHERE \"demand\" > 0 AND \"f_id\" IS NOT NULL\n"

            + "  UNION\n"

            + "  SELECT \"t_id\"::bigint AS nid FROM " + pg_quote_ident(chosen_od) + " WHERE \"demand\" > 0 AND \"t_id\" IS NOT NULL\n"

            + "), miss AS (\n"

            + "  SELECT nid FROM od_nodes EXCEPT SELECT nid FROM net_nodes\n"

            + ")\n"

            + "SELECT (SELECT COUNT(*) FROM miss) AS missing_cnt, (SELECT COALESCE(array_to_string(array_agg(nid::text),','),'') FROM (SELECT nid FROM miss LIMIT 10) s) AS sample_ids;";

        if (od_direct_mode) {

            PGresult* rmiss = PQexec(conn, q_missing.c_str());

            long long missing_cnt = 0;

            string sample_ids;

            if (rmiss && PQresultStatus(rmiss) == PGRES_TUPLES_OK && PQntuples(rmiss) > 0) {

                char* v0 = PQgetvalue(rmiss, 0, 0);

                char* v1 = PQgetvalue(rmiss, 0, 1);

                missing_cnt = v0 ? atoll(v0) : 0;

                sample_ids = v1 ? string(v1) : string();

            }

            if (rmiss) PQclear(rmiss);

            if (missing_cnt > 0) {

                PQclear(res2);

                PQclear(res1);

                PQfinish(conn);

                string extra = string("\"details\":{\"network_table\":\"") + esc_json(chosen_net)

                            + string("\",\"od_table\":\"") + esc_json(chosen_od)

                            + string("\",\"missing_cnt\":") + to_string(missing_cnt)

                            + string(",\"sample_missing_ids\":\"") + esc_json(sample_ids) + "\"}";

                return mkerr(8, "precheck_od_net", "OD需求数据和路网不匹配，请检查数据", extra);

            }

        }

    }



    // 获取路段数量信息

    int numLinks = PQntuples(res1);

    std::cout << w2u8(L"[DEBUG] 路段数量：") << numLinks << std::endl;



    // 构建节点集合

    // 首先从路段数据中获取所有唯一的节点ID

    set<int> nodeIds;

    set<int> roadNodeIds; // 仅来自非 type=10 的常规道路



    for (int i = 0; i < numLinks; i++) {

        char* tailStr = PQgetvalue(res1, i, idx_tail);

        char* headStr = PQgetvalue(res1, i, idx_head);

        char* typeStr = PQgetvalue(res1, i, idx_type);

        char* cmnStr  = PQgetvalue(res1, i, idx_cmn);



        if (tailStr && headStr) {

            tail = atoll(tailStr);

            head = atoll(headStr);

            nodeIds.insert((int)tail);

            nodeIds.insert((int)head);

            int ltype_db = typeStr ? atoi(typeStr) : 0;

            if (ltype_db != 10) {

                if (tail > baseMaxNodeId) baseMaxNodeId = tail;

                if (head > baseMaxNodeId) baseMaxNodeId = head;

                roadNodeIds.insert((int)tail);

                roadNodeIds.insert((int)head);



            } else {

                // 质心连杆（type=10）：

                // - centroid_matched_node: 原质心ID（zone_id）

                // - 质心新节点ID：取 max(head, tail)

                int zone_id = (cmnStr && *cmnStr) ? atoi(cmnStr) : -1;

                int centroid_new_node = (int)((tail > head)? tail : head);

                if (zone_id >= 0) {

                    zone2centroid[zone_id] = centroid_new_node;

                    int anchorNodeId = (int)((tail > head)? head : tail);

                    zone2anchor[zone_id] = anchorNodeId;

                }

            }



        } else {

            std::cout << w2u8(L"警告：第 ") << i << w2u8(L" 行数据无效。") << std::endl;

        }

    }



    int numNodes = nodeIds.size();

    std::cout << w2u8(L"[DEBUG] 节点数量：") << numNodes << std::endl;

    std::cout << w2u8(L"[DEBUG] 基础最大道路节点ID（type!=10）：") << baseMaxNodeId << std::endl;

    std::cout << w2u8(L"[DEBUG] 质心映射对数（zone -> centroid_node(new）：") << zone2centroid.size() << std::endl;

    if (!zone2centroid.empty()) {

        std::cout << w2u8(L"[DEBUG] zone2centroid 列表：") << std::endl;

        for (map<int,int>::const_iterator it = zone2centroid.begin(); it != zone2centroid.end(); ++it) {

            std::cout << w2u8(L"  zone ") << it->first

                 << w2u8(L" -> centroid_node(new) ") << it->second << std::endl;

        }

    } else {

        std::cout << w2u8(L"[DEBUG] 没有找到 type=10 连接器；如果可能，将回退到直接使用节点 ID 作为 OD 区域。") << std::endl;

    }



    // 构建节点

    for (int nodeId : nodeIds) {

        nval.type = BASND;

        nval.id = nodeId;

        nval.dummy = false;

        nval.xCord = 0; // 可以从数据库获取坐标

        nval.yCord = 0;

        if (CreateNewNode(nval) == NULL) {

            return mkerr(3, "create_node", string("CreateNewNode failed for node ") + to_string(nodeId), "");

        }

    }



    // 构建路段定义

    for (int i = 0; i < numLinks; i++) {

        // 安全地获取所有字段值

        char* tailStr = PQgetvalue(res1, i, idx_tail);

        char* headStr = PQgetvalue(res1, i, idx_head);

        char* capStr = PQgetvalue(res1, i, idx_capacity);

        char* lenStr = PQgetvalue(res1, i, idx_length);

        char* fftStr = PQgetvalue(res1, i, idx_fft);

        char* bStr = PQgetvalue(res1, i, idx_b);

        char* pStr = PQgetvalue(res1, i, idx_power);

        char* spdStr = PQgetvalue(res1, i, idx_speedlimit);

        char* tollStr = PQgetvalue(res1, i, idx_toll);

        char* linkIdStr = PQgetvalue(res1, i, idx_link_id);

        char* typeStr = PQgetvalue(res1, i, idx_type);

        

        // 检查所有字段是否有效

        if (!tailStr || !headStr || !capStr || !bStr || !pStr || !tollStr || 

            !typeStr || !fftStr || !spdStr || !lenStr || !linkIdStr) {

            std::cout << w2u8(L"警告：第 ") << i << w2u8(L" 行数据无效，跳过...") << std::endl;

            continue;

        }

        

        tail = atoll(tailStr);        // 字段0: init_node

        head = atoll(headStr);        // 字段1: term_node

        cap = atof(capStr);         // 字段2: capacity

        B = atof(bStr);           // 字段3: b

        P = atof(pStr);           // 字段4: power

        floatType toll = atof(tollStr); // 字段5: toll

        int linkType = atoi(typeStr);   // 字段6: type

        fft = atof(fftStr);         // 字段7: fft

        spd = atof(spdStr);         // 字段8: speedlimit

        len = atof(lenStr);         // 字段9: length

        long long linkId = atoll(linkIdStr); // 字段10: link_id

        if (tail != head) {

            lval.type = lpf; // 最小化修复：不使用数据库中的道路类型覆盖 TNM_LINKTYPE

            lval.id = (int)linkId; // 若内部结构为 int，窄化转换

            lval.dummy = false;

            lval.tail = CatchNodePtr((int)head, false);

            lval.head = CatchNodePtr((int)tail, false);



            if (fft > 0) {

                lval.length = len;

                lval.ffs = lval.length * 60 / fft / linkCostScalar;

            }

            else {

                lval.length = len;

                lval.ffs = 25.0;

            }



            lval.par.clear();

            lval.par.push_back(B);

            lval.par.push_back(P);

            lval.capacity = cap;



            TNM_SLINK* link = CreateNewLink(lval);

            if (link == NULL) {

                string reason;

                if (lval.tail == NULL) {

                    reason = "tail_null";

                } else if (lval.head == NULL) {

                    reason = "head_null";

                } else if (lval.capacity < 0) {

                    reason = "negative_capacity";

                } else if (lval.length < 0) {

                    reason = "negative_length";

                } else if (lval.ffs < 0) {

                    reason = "negative_ffs";

                } else {

                    bool parallel = false, duplicate = false;

                    for (PTRTRACE pv = lval.tail->forwStar.begin(); pv != lval.tail->forwStar.end(); ++pv) {

                        TNM_SLINK* ex = *pv;

                        if (ex != NULL) {

                            if (ex->head == lval.head) { parallel = true; break; }

                            if (ex->id == lval.id) { duplicate = true; break; }

                        }

                    }

                    if (parallel) reason = "parallel_or_duplicate";

                    else if (duplicate) reason = "duplicate_id";

                    else reason = "unknown";

                }

                return mkerr(4, "create_link", string("CreateNewLink failed for link_id=") + std::to_string(linkId)

                    + " tail=" + std::to_string(tail) + " head=" + std::to_string(head)

                    + " reason=" + reason, "");

            }

            link->cost = 0.0; // 可以从数据库获取收费信息

            link->toll = toll; // 使用数据库中的toll值

            link->SetTollType(TT_NOTOLL);

            link->InitializeCostCoef(timeCostCoefficient, distCostCoefficient);

        }

    }



    // 获取OD需求数据

    if (loadod) {

        string odQuery = "SELECT \"f_id\" AS org, \"t_id\" AS dest, \"demand\" FROM " + pg_quote_ident(chosen_od) + " ORDER BY \"f_id\", \"t_id\"";

        // cout << "[DEBUG] odQuery: " << odQuery.c_str() << endl;

        PGresult* res2 = PQexec(conn, odQuery.c_str());

        if (PQresultStatus(res2) != PGRES_TUPLES_OK) {

            std::cout << w2u8(L"执行OD需求查询失败：") << PQerrorMessage(conn) << std::endl;

            string em = PQerrorMessage(conn)? string(PQerrorMessage(conn)) : string("PGRES not tuples ok");

            string extra = string("\"details\":{\"od_table\":\"") + esc_json(chosen_od) + "\"}";

            PQclear(res2);

            PQclear(res1);

            PQfinish(conn);

            return mkerr(5, "query_od", em, extra);

        }



        int numOD = PQntuples(res2);

        std::cout << w2u8(L"[DEBUG] OD对数量：") << numOD << std::endl;



        if (numOD <= 0) {

            string extra = string("\"details\":{\"od_table\":\"") + esc_json(chosen_od) + "\"}";

            PQclear(res2);

            PQclear(res1);

            PQfinish(conn);

            return mkerr(9, "precheck_od_empty", "OD需求表为空（0行），已终止计算", extra);

        }



        map<int, vector<pair<int, floatType>>> odDemands;

        floatType totalDemand = 0.0;

        for (int i = 0; i < numOD; i++) {

            char* originStr = PQgetvalue(res2, i, 0);

            char* destStr = PQgetvalue(res2, i, 1);

            char* demandStr = PQgetvalue(res2, i, 2);

            if (!originStr || !destStr || !demandStr) {

                std::cout << w2u8(L"警告：第 ") << i << w2u8(L" 行OD数据无效，跳过...") << std::endl;

                continue;

            }



            int originId = atoi(originStr);

            int destId = atoi(destStr);

            floatType demand = atof(demandStr);

            if (demand <= 0) {

                continue;

            }



            totalDemand += demand;

            odDemands[originId].push_back(make_pair(destId, demand));

        }



        if (odDemands.empty() || totalDemand <= 0.0) {

            string extra = string("\"details\":{\"od_table\":\"") + esc_json(chosen_od)

                        + string("\",\"total_demand\":") + to_string((double)totalDemand) + "}";

            PQclear(res2);

            return mkerr(9, "precheck_od_zero_demand", "OD需求表总需求为0（或无正需求），已终止计算", extra);

        }



        		for (map<int, vector<pair<int, floatType>>>::iterator it = odDemands.begin(); it != odDemands.end(); ++it) {

			int originId = it->first;

			int numDest = (int)it->second.size();



			int originNodeId = -1;

			if (!od_direct_mode) {

				map<int,int>::iterator mo = zone2centroid.find(originId);

				if (mo != zone2centroid.end()) {

					originNodeId = mo->second;

				}

			}

			if (originNodeId < 0) {

				TNM_SNODE* fallbackOrg = CatchNodePtr(originId, false);

				if (fallbackOrg != NULL && roadNodeIds.count(originId) > 0) {

					originNodeId = originId;

				} else {

					std::cout << w2u8(L"错误：没有质心映射的origin zone ") << originId << w2u8(L"，且没有同ID的节点") << std::endl;

					PQclear(res2);

					PQclear(res1);

					PQfinish(conn);

					return mkerr(6, "map_origin", string("no centroid mapping and no node for zone ") + to_string(originId), "");

				}

			}



			TNM_SORIGIN* pOrg = CreateSOrigin(originNodeId, numDest);

			if (pOrg == NULL) {

				std::cout << w2u8(L"错误：创建origin失败，节点ID：") << originNodeId << std::endl;

				PQclear(res2);

				PQclear(res1);

				PQfinish(conn);

				return mkerr(6, "create_origin", string("failed for node ") + to_string(originNodeId), "");

			}



			pOrg->m_tdmd = 0.0;

			pOrg->origin->is_centroid = true;

			if (centroids_blocked) pOrg->origin->SkipCentroid = true;



			for (int j = 0; j < numDest; j++) {

				int destId = it->second[j].first;

				floatType demand = it->second[j].second;



				int destNodeId = -1;

				if (!od_direct_mode) {

					map<int,int>::iterator md = zone2centroid.find(destId);

					if (md != zone2centroid.end()) {

						destNodeId = md->second;

					}

				}

				if (destNodeId < 0) {

					TNM_SNODE* fallbackDest = CatchNodePtr(destId, false);

					if (fallbackDest != NULL && roadNodeIds.count(destId) > 0) {

						destNodeId = destId;

					} else {

						std::cout << w2u8(L"错误：没有质心映射的dest zone ") << destId << w2u8(L"，且没有同ID的节点") << std::endl;

						PQclear(res2);

						PQclear(res1);

						PQfinish(conn);

						return mkerr(7, "map_dest", string("no centroid mapping and no node for zone ") + to_string(destId), "");

					}

				}



				TNM_SNODE* node = CatchNodePtr(destNodeId, false);

				if (node == NULL) {

					cout << "Error: Failed to get node pointer for destination " << destNodeId << endl;

					PQclear(res2);

					PQclear(res1);

					PQfinish(conn);

					return mkerr(7, "catch_node_dest", string("destination node not found ") + to_string(destNodeId), "");

				}



				pOrg->SetDest(j + 1, node, demand);

				pOrg->m_tdmd += demand;

				node->is_centroid = true;

				if (centroids_blocked) node->SkipCentroid = true;

			}

		}



		PQclear(res2);

	}



    PQclear(res1);

    PQfinish(conn);



    UpdateLinkNum();

    UpdateNodeNum();

    if (loadod) UpdateOriginNum();

    else numOfOrigin = 0;

    buildStatus = 1;



    return 0;

}