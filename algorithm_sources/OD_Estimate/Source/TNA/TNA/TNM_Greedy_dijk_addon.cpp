/**
 * TAP_Greedy_dijk + 点对点最短路（从 Greedy 树移植，供 OD_ESTIMATION 内部分配与 motor 一致）
 */
#include "header/stdafx.h"
#include <cmath>
#include <sstream>
#include <cassert>
#include <ctime>
#include <cstdlib>
#include <iostream>

using namespace std;

namespace {
bool OdAssignTraceEnabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("TNA_OD_ASSIGN_TRACE") != nullptr) ? 1 : 0;
	return cached != 0;
}
void OdAssignTrace(const string& line)
{
	if (!OdAssignTraceEnabled()) return;
	cout << "[OD_ASSIGN_TRACE] " << line << endl;
}
} // namespace

floatType TNM_TAP::RelativeGap2(bool scale)
{
	TNM_SLINK* link;
	TNM_SORIGIN* org;
	TNM_SDEST* dest;
	floatType gap, tt = 0.0;

	for (int i = 0; i < network->numOfLink; i++)
	{
		link = network->linkVector[i];
		tt += link->volume * link->cost;
	}
	gap = tt;
	for (int i = 0; i < network->numOfOrigin; i++)
	{
		org = network->originVector[i];
		for (int j = 0; j < org->numOfDest; j++)
		{
			dest = org->destVector[j];
			if (dest->ifZeroDemand) continue;
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
	return fabs(gap);
}

TAP_Greedy_dijk::TAP_Greedy_dijk() {}

TAP_Greedy_dijk::~TAP_Greedy_dijk() {}

void TAP_Greedy_dijk::Initialize()
{
	network->AllocateNodeBuffer(2);
	network->AllocateLinkBuffer(3);

	cout << "[DEBUG] num of origin is " << network->numOfOrigin << endl;
	cout << "[DEBUG] num of nodes is " << network->numOfNode << endl;
	cout << "[DEBUG] num of OD is " << network->numOfOD << endl;

	network->UpdateLinkCost();
	network->InitialSubNet4();
	if (!TNM_GetLastError().empty())
	{
		termFlag = ErrorTerm;
		return;
	}
	network->UpdateLinkCost();
	ComputeOFV();

	cout << "[DEBUG] The initial objective is : " << OFV << endl;

	for (int oi = 0; oi < network->numOfOrigin; oi++)
	{
		TNM_SORIGIN* pOrg = network->originVector[oi];
		for (int di = 0; di < pOrg->numOfDest; di++)
		{
			TNM_SDEST* sdest = pOrg->destVector[di];
			if (pOrg->origin == sdest->dest) continue;
			if (sdest->pathSet.empty()) continue;
			TNM_SPATH* spath = sdest->pathSet.front();
			nPath++;
			spath->id = nPath;
		}
	}
	cout << "[DEBUG] The num of links is " << network->numOfLink << endl;
	cout << "[DEBUG] end of the ini " << endl;
}

void TAP_Greedy_dijk::MainLoop()
{
	clock_t t_loop = clock();
	{
		ostringstream oss;
		oss << "MainLoop ENTER curIter=" << curIter
		    << " maxMainIter=" << maxMainIter << " maxInner=" << m_maxInnerIter;
		OdAssignTrace(oss.str());
	}

	TotalFlowChange = 0.0;
	numOfPathChange = 0;
	totalShiftFlow = 0.0;
	maxPathGap = 0;

	clock_t t_col = clock();
	TNM_SORIGIN* pOrg;
	for (int i = 0; i < network->numOfOrigin; i++)
	{
		pOrg = network->originVector[i];
		for (int j = 0; j < pOrg->numOfDest; j++)
		{
			TNM_SDEST* dest = pOrg->destVector[j];
			if (dest->ifZeroDemand) continue;
			if (pOrg->origin == dest->dest) continue;
			dest->shiftFlow = 1.0;
			ColumnGeneration(pOrg, dest);
			if (ReachError())
				return;
			columnG = true;
			UpdatePathFlowGreedy(pOrg, dest);
		}
	}

	innerShiftFlow = 1.0;
	int il;
	int numofPath;
	maxPathGap = 0.0;
	numOfD = 0;
	int numofD2 = 0;
	int maxInIter = (m_maxInnerIter > 0) ? m_maxInnerIter : 500;
	if (maxInIter < 5) maxInIter = 5;

	{
		ostringstream oss;
		oss << "MainLoop after ColumnGeneration phase elapsed_sec="
		    << (1.0 * (clock() - t_col) / CLOCKS_PER_SEC);
		OdAssignTrace(oss.str());
	}

	clock_t t_gap0 = clock();
	double currentGap = RelativeGap2();
	if (ReachError())
		return;
	double innerThreshold = currentGap / 2.0;
	{
		ostringstream oss;
		oss << "MainLoop RelativeGap2(initial) gap=" << currentGap
		    << " elapsed_sec=" << (1.0 * (clock() - t_gap0) / CLOCKS_PER_SEC);
		OdAssignTrace(oss.str());
	}

	clock_t t_inner = clock();
	for (il = 0; il < maxInIter; il++)
	{
		innerShiftFlow = 0.0;
		numofPath = 0;
		numofD2 = 0;
		for (int i = 0; i < network->numOfOrigin; i++)
		{
			pOrg = network->originVector[i];
			for (int j = 0; j < pOrg->numOfDest; j++)
			{
				TNM_SDEST* dest = pOrg->destVector[j];
				if (dest->ifZeroDemand) continue;
				if (pOrg->origin == dest->dest) continue;
				if (maxInIter >= 100 && il % (maxInIter / 100) == 0)
					dest->shiftFlow = 1.0;
				if (dest->pathSet.size() > 1 && il == 0)
					dest->shiftFlow = 1.0;
				if (dest->shiftFlow > innerThreshold)
				{
					columnG = false;
					UpdatePathFlowGreedy(pOrg, dest);
					numofD2++;
				}
				numofPath += (int)dest->pathSet.size();
			}
		}
		if (innerShiftFlow < 1e-10)
			break;
	}
	{
		ostringstream oss;
		oss << "MainLoop inner loop done il=" << il << "/" << maxInIter
		    << " elapsed_sec=" << (1.0 * (clock() - t_inner) / CLOCKS_PER_SEC);
		OdAssignTrace(oss.str());
	}

	if (!dePathSet.empty())
	{
		for (size_t pi = 0; pi < dePathSet.size(); ++pi)
			delete dePathSet[pi];
		dePathSet.clear();
	}

	clock_t t_gap1 = clock();
	convIndicator = RelativeGap2();
	if (ReachError())
		return;
	{
		ostringstream oss;
		oss << "MainLoop RelativeGap2(final) gap=" << convIndicator
		    << " elapsed_sec=" << (1.0 * (clock() - t_gap1) / CLOCKS_PER_SEC);
		OdAssignTrace(oss.str());
	}
	aveFlowChange = (numOfPathChange > 0) ? TotalFlowChange / numOfPathChange : 0.0;
	ComputeOFV();

	{
		ostringstream oss;
		oss << "MainLoop EXIT curIter=" << curIter << " gap=" << convIndicator
		    << " OFV=" << OFV << " loop_elapsed_sec=" << (1.0 * (clock() - t_loop) / CLOCKS_PER_SEC)
		    << " solve_elapsed_sec=" << (1.0 * (clock() - m_startRunTime) / CLOCKS_PER_SEC);
		OdAssignTrace(oss.str());
	}

	static bool s_dbg = (getenv("TNA_GREEDY_DEBUG") != nullptr);
	if (s_dbg)
	{
		cout << "[DEBUG] iter=" << curIter << " gap=" << convIndicator
		     << " elapsed=" << 1.0 * (clock() - m_startRunTime) / CLOCKS_PER_SEC << endl;
	}
}

void TAP_Greedy_dijk::ColumnGeneration(TNM_SORIGIN* pOrg, TNM_SDEST* dest)
{
	yPath->path.clear();
	yPath->flow = 0;
	yPath->cost = 0;
	TNM_SPATH* sp = network->SPath(pOrg->origin, dest->dest);
	if (!sp)
	{
		termFlag = ErrorTerm;
		return;
	}
	yPath = sp;
}

// --- TNM_SNET one-to-one shortest path (from Greedy TNM_Net.cpp) ---

TNM_SPATH* TNM_SNET::SPath(TNM_SNODE* origin, TNM_SNODE* dest)
{
	UpdateSPR(origin, dest);
	TNM_SPATH* path = GetSPath_R_(origin, dest);
	if (path == NULL)
	{
		ostringstream oss;
		oss << "最短路径不可达: origin " << origin->id << " -> dest " << dest->id;
		TNM_SetLastError(oss.str());
		cout << "Cannot find shortest path between origin " << origin->id
		     << " and dest " << dest->id << endl;
	}
	return path;
}

TNM_SPATH* TNM_SNET::GetSPath_R_(TNM_SNODE* origin, TNM_SNODE* dest)
{
	TNM_SNODE* node;
	TNM_SLINK* link;
	TNM_SPATH* path = new TNM_SPATH;
	int count = 0;
	assert(path != 0);
	node = origin;
	while (node != dest)
	{
		link = node->pathElem->via;
		if (link == NULL)
		{
			delete path;
			return NULL;
		}
		node = link->head;
		path->path.push_back(link);
		count++;
		if (count > numOfNode)
		{
			delete path;
			return NULL;
		}
	}
	path->cost = origin->pathElem->cost;
	return path;
}

void TNM_SNET::UpdateSPR(TNM_SNODE* rootNode)
{
	for (int j = 0; j < numOfNode; j++)
	{
		TNM_SNODE* node = nodeVector[j];
		node->InitPathElem();
		node->scanStatus = 0;
	}
	scanList->SPTreeD(rootNode);
}

void TNM_SNET::UpdateSPR(TNM_SNODE* origin, TNM_SNODE* dest)
{
	for (int j = 0; j < numOfNode; j++)
	{
		TNM_SNODE* node = nodeVector[j];
		node->InitPathElem();
		node->scanStatus = 0;
	}
	scanList->SPTreeD(origin, dest);
}

void TNM_SNET::InitialSubNet4(int kx, bool createPath)
{
	(void)kx;
	(void)createPath;
	for (int i = 0; i < numOfOrigin; i++)
	{
		TNM_SORIGIN* origin = originVector[i];
		for (int j = 0; j < origin->numOfDest; j++)
		{
			TNM_SDEST* dest = origin->destVector[j];
			if (origin->origin == dest->dest) continue;
			double dmd = dest->assDemand;
			TNM_SPATH* path = SPath(origin->origin, dest->dest);
			if (!path)
				return;
			path->flow = dmd;
			for (size_t pi = 0; pi < path->path.size(); pi++)
			{
				TNM_SLINK* slink = path->path[pi];
				slink->volume += dmd;
			}
			dest->pathSet.push_back(path);
		}
	}
}

void SCANLIST::SPTreeD(TNM_SNODE* origin, TNM_SNODE* dest)
{
	TNM_SNODE* curNode;
	nodeList.clear();
	InitRoot(dest);
	InsertANode(dest);
	curNode = GetNextNodeMin();
	while (curNode != NULL && curNode != origin)
	{
		if (curNode->m_isThrough || curNode == dest)
			curNode->SearchMinInLink(this);
		curNode = GetNextNodeMin();
	}
}

TNM_SNODE* SCAN_DEQUE::GetNextNodeMin()
{
	if (nodeList.empty())
		return NULL;
	QuickSortNode(0, (int)nodeList.size() - 1);
	TNM_SNODE* node = nodeList.front();
	nodeList.pop_front();
	node->scanStatus = -1;
	return node;
}

void SCAN_DEQUE::QuickSortNode(int low, int high)
{
	if (low >= high) return;
	TNM_SNODE* snode = nodeList[low];
	int i = low;
	int j = high;
	while (i < j)
	{
		while (i < j && nodeList[j]->pathElem->cost >= snode->pathElem->cost)
			j--;
		nodeList[i] = nodeList[j];
		while (i < j && nodeList[i]->pathElem->cost <= snode->pathElem->cost)
			i++;
		nodeList[j] = nodeList[i];
	}
	nodeList[i] = snode;
	QuickSortNode(low, i - 1);
	QuickSortNode(i + 1, high);
}
