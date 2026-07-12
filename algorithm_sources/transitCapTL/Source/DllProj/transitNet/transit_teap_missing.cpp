#include "PTNet.h"

using namespace std;

namespace
{
	void DeleteHyperPath(TNM_HyperPath* path)
	{
		if(path == NULL) return;
		vector<GLINK*> glinks = path->GetGlinks();
		for(size_t i = 0; i < glinks.size(); ++i)
		{
			delete glinks[i];
		}
		delete path;
	}

	void ClearOrgPathSet(PTOrg* org)
	{
		if(org == NULL) return;
		for(size_t i = 0; i < org->pathSet.size(); ++i)
		{
			DeleteHyperPath(org->pathSet[i]);
		}
		org->pathSet.clear();
		org->minIx = 0;
		org->currentTotalCost = 0.0;
		org->currentRelativeGap = 1.1;
		org->maxPathGap = 0.0;
	}
}

void PTNode::CleanStgLinksOnHyperPath()
{
	PTLink* link = StgElem ? StgElem->vialink : NULL;
	while(link)
	{
		PTLink* next = link->stglinkptr;
		link->stglinkptr = NULL;
		link = next;
	}
	if(StgElem)
	{
		StgElem->vialink = NULL;
	}
	m_attProb.clear();
}

int PTNET::PTAllOrNothing(PTDestination* dest)
{
	if(dest == NULL) return 1;
	if(InitializeHyperpathLS(dest->destination) != 0) return 1;
	for(int j = 0; j < dest->numOfOrg; j++)
	{
		PTOrg* org = dest->orgVector[j];
		TNM_HyperPath* path = new TNM_HyperPath();
		if(path->InitializeHP(org->org, dest->destination))
		{
			double dmd = org->assDemand;
			netTTwaitcost += path->WaitCost * dmd;
			vector<GLINK*> glinks = path->GetGlinks();
			for(size_t i = 0; i < glinks.size(); ++i)
			{
				glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
			}
			DeleteHyperPath(path);
		}
		else
		{
			cout<<"OD-pair <"<<org->org->GetStopPtr()->m_id<<","<<dest->destination->GetStopPtr()->m_id<<"> can not initialize hyperpath tree"<<endl;
			DeleteHyperPath(path);
			return 1;
		}
	}
	return 0;
}

int PTNET::PTAllOrNothing()
{
	netTTwaitcost = 0.0;
	for(int i = 0; i < numOfLink; i++)
	{
		linkVector[i]->volume = 0.0;
	}
	for(int i = 0; i < numOfPTDest; i++)
	{
		if(PTAllOrNothing(PTDestVector[i]) != 0) return 1;
	}
	return 0;
}

bool PTNET::InitialHyperpathNetFlow()
{
	netTTwaitcost = 0.0;
	for(int i = 0; i < numOfLink; i++)
	{
		linkVector[i]->volume = 0.0;
	}
	for(int i = 0; i < numOfPTDest; i++)
	{
		PTDestination* dest = PTDestVector[i];
		for(int j = 0; j < dest->numOfOrg; j++)
		{
			ClearOrgPathSet(dest->orgVector[j]);
		}
	}
	for(int i = 0; i < numOfPTDest; i++)
	{
		PTDestination* dest = PTDestVector[i];
		if(InitializeHyperpathLS(dest->destination) != 0) return false;
		for(int j = 0; j < dest->numOfOrg; j++)
		{
			PTOrg* org = dest->orgVector[j];
			TNM_HyperPath* path = new TNM_HyperPath();
			if(!path->InitializeHP(org->org, dest->destination))
			{
				cout<<"OD-pair <"<<org->org->GetStopPtr()->m_id<<","<<dest->destination->GetStopPtr()->m_id<<"> can not initialize hyperpath tree"<<endl;
				DeleteHyperPath(path);
				return false;
			}
			path->flow = org->assDemand;
			org->pathSet.push_back(path);
			netTTwaitcost += path->WaitCost * path->flow;
			vector<GLINK*> glinks = path->GetGlinks();
			for(size_t k = 0; k < glinks.size(); ++k)
			{
				glinks[k]->m_linkPtr->volume += glinks[k]->m_data * path->flow;
			}
		}
	}
	ComputeConvGap();
	return true;
}

void PTNET::ColumnGeneration(PTDestination* dest, PTOrg* org)
{
	if(dest == NULL || org == NULL) return;
	TNM_HyperPath* cpath = new TNM_HyperPath();
	if(cpath->InitializeHP(org->org, dest->destination))
	{
		bool pin = false;
		for(vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it != org->pathSet.end(); ++it)
		{
			if((*it)->name == cpath->name)
			{
				pin = true;
				break;
			}
		}
		if(!pin)
		{
			org->pathSet.push_back(cpath);
		}
		else
		{
			DeleteHyperPath(cpath);
		}
	}
	else
	{
		DeleteHyperPath(cpath);
	}
}
