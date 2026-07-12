#include "PTNet.h"
//#include "TNM_utility.h"
//#include "..\..\..\include\tnm\TNM_utility.h"


bool comp_link_cost(const PTLink *a,const PTLink *b)
{
	return a->tmpuse < b->tmpuse;
};

bool comp_link_id(const PTLink *a,const PTLink *b)
{
	return  a->id <b->id;
};

bool comp_gnode_level(const GNODE *a,const GNODE *b)
{
	return  a->m_tpLevel > b->m_tpLevel;
};



int	PTNET::PCTAE_Solver(PCTAE_algorithm alg)
{
	PCTAE_SetAlgorithm(alg);

	if (m_symLinks)//m_symLinks默认为false
	{	
		SetLinksAttribute(true);//symmtric functional form
	}
	else 
	{	
		SetLinksAttribute(false);//asymmtric functional form（默认为不对称link）
	}


	if (alg != PCTAE_algorithm::PCTAE_P_AON)
	{
		cout << "This standalone build only supports PCTAE_P_AON." << endl;
		return 1;
	}

	return SolvePathTEAP_AON();
}


int PTNET::SolveFWTEAP()
{
	AllocateLinkBuffer(1);
	AllocateNetBuffer(1);
	UpatePTNetworkLinkCost();
	PTAllOrNothing();
    UpatePTNetworkLinkCost();
	m_startRunTime = clock();

	if(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime) * 1.0 / (CLOCKS_PER_SEC*60.0))))
	{
		do
		{
			clock_t sclock = clock();
			SaveLinkWaitVariables();
			
			PTAllOrNothing();
			if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_GFW_TP || PCTAE_ALG == PCTAE_algorithm::PCTAE_A_GFW)	BisecSearch();
			NewSolution(1);
			UpatePTNetworkLinkCost();
			IterMainlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;
		
			ComputeConvGap();
			curIter ++;		
			RecordTEAPCurrentIter();
			cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<endl;
		}while (!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))));
	}
	cout<<"CPU TIME: "<<1.0*(clock() - m_startRunTime)<<endl;
	return 0;
}



int PTNET::SolveMSATEAP()
{
	AllocateLinkBuffer(1);//初始化link的临时变量（1个）
	AllocateNetBuffer(1);//初始化network的临时变量（1个）
	UpatePTNetworkLinkCost();//更新网络中所有弧的费用和一阶导
	PTAllOrNothing();//全有全无分配
    UpatePTNetworkLinkCost();//更新网络中所有弧的费用和一阶导
	m_startRunTime = clock();

	if(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))))
	{
		do
		{
			clock_t sclock = clock();
			SaveLinkWaitVariables();
			
			PTAllOrNothing();//全有全无分配
			stepSize = 1.0 / (curIter + 1);//MSA步长
			NewSolution(1);
			UpatePTNetworkLinkCost();
			IterMainlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;
		
			ComputeConvGap();
			curIter ++;		
			RecordTEAPCurrentIter();
			cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<endl;
		}while (!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))));
	}
	cout<<"CPU TIME: "<<1.0*(clock() - m_startRunTime)<<endl;
	return 0;
}


//int	PTNET::SolveFWTEAP()
//{
//	AllocateLinkBuffer(1);
//	AllocateNetBuffer(1);
//	UpatePTNetworkLinkCost();
//	PTAllOrNothing();
//	UpatePTNetworkLinkCost();
//	m_startRunTime = clock();
//	//PrintNetLinks();
//	//ComputeConvGap();
//	//cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<endl;
//
//	if(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))))
//	{
//		do
//		{
//			clock_t sclock = clock();
//			SaveLinkWaitVariables();
//			PTAllOrNothing();
//			if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_GFW)	BisecSearch();
//			//cout<<"                netTTwaitcost :"<<netTTwaitcost<<endl;
//			NewSolution(1);
//		
//			UpatePTNetworkLinkCost();
//			IterMainlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;
//			ComputeConvGap();
//			//cout<<"                iter: "<<curIter<<endl;
//			curIter ++;
//			RecordTEAPCurrentIter();
//			cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<endl;
//		}while (!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))));
//	}
//	cout<<"CPU TIME: "<<1.0*(clock() - m_startRunTime)<<endl;
//	//PrintNetLinks();
//	return 0;
//}
/*
StgLinks* PTNET::GenerateNonBoardingStg(PTLink* link)
{
	StgLinks* stg  = new StgLinks();
	GLINK* glink = new GLINK(link) ;

	stg->StgLinkVia = glink;		
	glink->m_prob = 1.0;
	stg->sname = std::to_string(link->id) + "-";
	stg->cost = link->cost + link->head->m_cost;
	return stg;
}
*/
/*
StgLinks* PTNET::GenerateBoardingStg(vector<PTLink*> links)
{
	StgLinks* stg  = new StgLinks();
	if (links.size()>1)
		sort(links.begin(),links.end(),comp_link_cost);
	floatType MinExpTotalTT;
	floatType ttfreq;
	vector<PTLink*> AttractiveSet;
	
	PTRTRACE pv = links.begin();
	do 
    {
		AttractiveSet.push_back(*pv);
		ttfreq = 0.0;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			ttfreq += 1.0/ (*ai)->m_hwmean;
		}
		MinExpTotalTT = timescaler/ttfreq;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			MinExpTotalTT += ((1.0/ (*ai)->m_hwmean)/ttfreq) * ((*ai)->tmpuse);
		}
		pv++;
	}while(pv!=links.end() && (*pv)->tmpuse < MinExpTotalTT );

	PTRTRACE iv = AttractiveSet.begin(); 
	if (AttractiveSet.size()>1) sort(AttractiveSet.begin(),AttractiveSet.end(),comp_link_id);
	GLINK* lastlink = NULL;
	do
	{
		GLINK* glink = new GLINK(*iv) ;
		if (!lastlink)	stg->StgLinkVia = glink;
		else lastlink->revStgLink = glink;

		glink->m_prob = (1.0/(*iv)->m_hwmean)/ttfreq;
		stg->sname += std::to_string((*iv)->id) + "-";

		lastlink = glink;
		iv++;

	}while(iv!=AttractiveSet.end());

	stg->waitT = timescaler/ttfreq ;
	stg->cost = MinExpTotalTT;
	return stg;
}
*/

/*
StgLinks*	PTNET::GenerateBoardingStg(vector<PTLink*> links)
{
	if (links.size()>1)
		sort(links.begin(),links.end(),comp_link_cost);
	floatType MinExpTotalTT;
	floatType ttfreq;
	vector<PTLink*> AttractiveSet;
	PTRTRACE pv = links.begin();
	do 
    {
		AttractiveSet.push_back(*pv);
		ttfreq = 0.0;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			ttfreq += 1.0/ (*ai)->m_hwmean;
		}
		MinExpTotalTT = timescaler/ttfreq;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			MinExpTotalTT += ((1.0/ (*ai)->m_hwmean)/ttfreq) * ((*ai)->tmpuse);
		}
		pv++;
	}while(pv!=links.end() && (*pv)->tmpuse < MinExpTotalTT );

	PTRTRACE iv = AttractiveSet.begin(); 
	if (AttractiveSet.size()>1) sort(AttractiveSet.begin(),AttractiveSet.end(),comp_link_id);

	vector<floatType> probs;
	vector<int> ids;
	string name = "";
	for(PTRTRACE iv = AttractiveSet.begin();iv != AttractiveSet.end();iv++)
	{
		probs.push_back((1.0/(*iv)->m_hwmean)/ttfreq);
		ids.push_back((*iv)->id - 1);//insert link order
		name += std::to_string((*iv)->id) + "-";
	}
	return new StgLinks(name,timescaler/ttfreq,ids,probs,MinExpTotalTT);

}
*/

StgLinks*	PTNET::GenerateNonBoardingStg(PTLink* link)
{
	StgLinks* newstg = new StgLinks;		
	newstg->waitT = 0;
	vector<floatType> probs;
	probs.push_back(1.0);
	newstg->SetLinkProbVec(probs);		
	newstg->sname += std::to_string(link->fID) + "-";
	newstg->StgLinkPosVec.push_back(link->fID);	
	return newstg;

}


void PTNET::GenerateBoardingStg(vector<PTLink*> links, vector<PTLink*> &AttractiveSet,floatType &ec/*expected cost*/,floatType &ew/*expected waiting delays*/)
{
	if (links.size()>1)
		sort(links.begin(),links.end(),comp_link_cost);//依据cost大小进行排序
	floatType ttfreq;
	//cout<<endl<<"tailnode:"<<links[0]->tail->id<<endl;
	PTRTRACE pv = links.begin();
	do 
    {
		AttractiveSet.push_back(*pv);
		ttfreq = 0.0;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			ttfreq += 1.0/ (*ai)->m_hwmean;
		}
		ew = 1.0 / ttfreq;
		ec = ew;
		double p =0;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			ec += ((1.0/ (*ai)->m_hwmean)/ttfreq) * ((*ai)->tmpuse);
			(*ai)->m_probdata = (1.0/(*ai)->m_hwmean)/ttfreq;
			
		}
		pv++;
	}while(pv != links.end() && (*pv)->tmpuse < ec);

	if (AttractiveSet.size()>1) 
		sort(AttractiveSet.begin(),AttractiveSet.end(),comp_link_id);


}

void PTNET::GenerateBoardingStg_eff(vector<PTLink*> links, vector<PTLink*> &AttractiveSet,floatType &ec/*expected cost*/,floatType &ew/*expected waiting delays*/)
{
	if (links.size()>1)
		sort(links.begin(),links.end(),comp_link_cost);//使用的是tmpuse排的大小
	floatType ttfreq;
	//cout<<endl<<"tailnode:"<<links[0]->tail->id<<endl;
	PTRTRACE pv = links.begin();
	do 
    {
		AttractiveSet.push_back(*pv);
		ttfreq = 0.0;
		for (PTRTRACE ai = AttractiveSet.begin(); ai!= AttractiveSet.end(); ai++)
		{
			ttfreq += (*ai)->efffreq;//使用的是有效发车频率
		}
		ew = 1.0 / ttfreq;
		ec = ew;
		double p =0;
		for (PTRTRACE ai = AttractiveSet.begin(); ai!= AttractiveSet.end(); ai++)
		{
			ec += ((*ai)->efffreq / ttfreq) * ((*ai)->tmpuse);
			(*ai)->m_probdata = (*ai)->efffreq / ttfreq;
			//if((*ai)->id == 14)
			//{cout<<""<<endl;}
		}
		pv++;
	}while(pv!=links.end() && (*pv)->tmpuse < ec );

	if (AttractiveSet.size()>1) 
		sort(AttractiveSet.begin(),AttractiveSet.end(),comp_link_id);


}


/*
StgLinks* PTNET::GenerateBoardingStg(multimap<double, int,less<double>> linkids)
{
	multimap<double, int, less<double> >::iterator pv= linkids.begin();  
	vector<PTLink*> AttractiveSet;
	StgLinks* stg  = new StgLinks();
	floatType MinExpTotalTT;
	floatType ttfreq;
	do 
    {
		//cout<<pv->second->id<<","<<pv->first<<endl;
		PTLink* link = linkVector[pv->second - 1];
		
		AttractiveSet.push_back(link);
		ttfreq = 0.0;
		
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			ttfreq += 1.0/ (*ai)->m_hwmean;
		}
		MinExpTotalTT = timescaler/ttfreq;
		for (PTRTRACE ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			MinExpTotalTT += ((1.0/ (*ai)->m_hwmean)/ttfreq) * (*ai)->tmpuse;
		}
		//cout<<"MinExpTotalTT:"<<MinExpTotalTT<<",ttfreq:"<<ttfreq<<",ttfreq:"<<ttfreq<<endl;
        pv++;
    }while(pv!=linkids.end() && pv->first < MinExpTotalTT);

	PTRTRACE iv = AttractiveSet.begin(); 
	if (AttractiveSet.size()>1) sort(AttractiveSet.begin(),AttractiveSet.end(),comp_link_id);
	GLINK* lastlink = NULL;
	double p = 0;
	do
	{
		GLINK* glink = new GLINK(*iv) ;
		if (!lastlink)	stg->StgLinkVia = glink;
		else lastlink->revStgLink = glink;

		glink->m_prob = (1.0/(*iv)->m_hwmean)/ttfreq;
		p += glink->m_prob;
		stg->sname += std::to_string((*iv)->id) + "-";

		lastlink = glink;
		iv++;

	}while(iv!=AttractiveSet.end());


	stg->waitT = timescaler/ttfreq;
	//cout<<timescaler<<endl;
	stg->cost = MinExpTotalTT;
	stg->approach  = p;
	return stg;

}

StgLinks* PTNET::GenerateBoardingStg(multimap<double, PTLink*,less<double>> plines)
{
	
	multimap<double, PTLink*, less<double> >::iterator pv= plines.begin();  
	//cout<<pv->second->id<<endl;
	multimap<int, PTLink*,less<int>> AttractiveSet;
	StgLinks* stg  = new StgLinks();
	floatType MinExpTotalTT;
	floatType ttfreq;
	do 
    {
		//cout<<pv->second->id<<","<<pv->first<<endl;
		AttractiveSet.insert(pair<int, PTLink*>(pv->second->id, pv->second));
		pv->second->tmpuse = pv->first;
		ttfreq = 0.0;
		
		for (multimap<int, PTLink*,less<int>>::iterator it = AttractiveSet.begin(); it!=  AttractiveSet.end();it++)
		{
			ttfreq += 1.0/ it->second->m_hwmean;
		}
		MinExpTotalTT = timescaler/ttfreq;
		for (multimap<int, PTLink*,less<int>>::iterator it = AttractiveSet.begin(); it!=  AttractiveSet.end();it++)
		{
			MinExpTotalTT +=  ((1.0/ it->second->m_hwmean)/ttfreq) * it->second->tmpuse;
		}
		//cout<<"MinExpTotalTT:"<<MinExpTotalTT<<",ttfreq:"<<ttfreq<<",ttfreq:"<<ttfreq<<endl;
        pv++;
    }while(pv!=plines.end() && pv->first < MinExpTotalTT);

	multimap<int, PTLink*,less<int>>::iterator iv = AttractiveSet.begin();
	GLINK* lastlink = NULL;
	double p = 0;
	do
	{
		GLINK* glink = new GLINK(iv->second) ;
		if (!lastlink)	stg->StgLinkVia = glink;
		else lastlink->revStgLink = glink;

		glink->m_prob = (1.0/iv->second->m_hwmean)/ttfreq;
		p += glink->m_prob;
		stg->sname += std::to_string(iv->first) + "-";

		lastlink = glink;
		iv++;

	}while(iv!=AttractiveSet.end());

	stg->waitT = timescaler/ttfreq;
	stg->cost = MinExpTotalTT;
	stg->approach = p;
	return stg;
}
*/
void PTNET::SaveLinkWaitVariables(int col)//col=1
{
	for (int i = 0;i<numOfLink;i++)
	{
		linkVector[i]->buffer[col - 1] = linkVector[i]->volume;//记录每个link的当前流量
		//linkVector[i]->oldvolumeset = linkVector[i]->volumeset;
	}
		
	if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_A_MSA_eff_TL)
	{
		buffer[0] = max_w;
	}
	else
	{
		buffer[0] = netTTwaitcost;
	}
	
}

void	PTNET::NewSolution(int col)
{
	PTLink* plink;
	double tmp;
	//convIndicator = 0.0;
	if (linkBufferSize<col)
	{
		cout<<"\tTNM_A-NewSolution: you need a buffer array of size "<<col<<" on links. "
			<<"\tAbnormal termination!"<<endl;
		exit(0);
	}
	for (int i = 0;i<numOfLink;i++)
	{
		plink = linkVector[i];
		
		//cout<<"link:"<<plink->id<<",old volume:"<<plink->buffer[col - 1]<<",new volume:"<<plink->volume<<",stepSize:"<<stepSize<<endl;

		plink->volume  = plink->buffer[col - 1] + (plink->volume - plink->buffer[col - 1]) * stepSize;//更新link的volume

		
		//if (link->buffer[col-1] != link->volume)
		//{
		//	cout<<"oldvol = "<<link->buffer[col-1]<<" link volume is updated to "<<link->volume<<endl;
		//}
	}
	//cout<<"oldnetwait = "<<buffer[0]<<" , auxi netwait =  "<<netTTwaitcost<<endl;
	if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff)
	{
		max_w = buffer[0] + (max_w - buffer[0])* stepSize;//???
	}
	else
	{
		netTTwaitcost = buffer[0] + (netTTwaitcost - buffer[0])* stepSize; //update network wait cost 
	}
	
}

floatType PTNET::ComputePTDz()
{
	floatType dz = 0, 
		diff, tmpVol,rtemVol,rdiff;
	PTLink* plink;
	for (int i = 0; i < numOfLink; i++) 
	{
		plink = linkVector[i];
		if (plink->rLink)
		{
			diff    = plink->volume - plink->buffer[0];
			rdiff	= plink->rLink->volume - plink->rLink->buffer[0];
			tmpVol  = plink->volume;
			rtemVol = plink->rLink->volume;
			plink->volume = plink->buffer[0] + diff*stepSize;
			plink->rLink->volume = plink->rLink->buffer[0] + rdiff*stepSize;
			dz     += diff * plink->GetPTLinkCost();
			plink->volume = tmpVol;
			plink->rLink->volume = rtemVol;

		}
		else
		{
			diff    = plink->volume - plink->buffer[0];
			tmpVol  = plink->volume;
			plink->volume = plink->buffer[0] + diff*stepSize;
			dz  += diff * plink->GetPTLinkCost();
			plink->volume = tmpVol;	
		}
	}
	if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff)
	{dz += max_w - buffer[0];}
	else
	{dz += netTTwaitcost - buffer[0];}
	

	return dz;

}

int PTNET::BisecSearch(floatType maxStep)
{
	floatType a = 0, dz = 1.0, b;
	int iter = 0; 
	b = maxStep;
	stepSize = b / 2.0;
	int maxLineSearchIter = 100;
	floatType lineSearchAccuracy = 1e-8;
	//cout<<"netTTwaitcost:"<<netTTwaitcost<<", buffer[0]:"<<buffer[0]<<endl;
 //cout<<"Start bisec search, maximum step size = "<<b<<endl;
	//while (((b-a)>=lineSearchAccuracy * maxStep ||dz > 0)&&iter<maxLineSearchIter) {
	while(dz > 0 || (iter < maxLineSearchIter && (b-a)>=lineSearchAccuracy * maxStep)) 
	//while( (iter < maxLineSearchIter && (b-a)>=lineSearchAccuracy * maxStep)) 
	{
		stepSize = (a + b)/2.0;
		iter = iter + 1;
		//stepSize = 0.0;
		dz = ComputePTDz();
		//cout<<"lineiter:"<<iter<<",dz:"<<dz<<endl;
		if (dz<0.0)  a = stepSize;
		else b=stepSize;

		if (iter >= maxLineSearchIter ) 
		{
			stepSize = 1.0;
			break;
		}
		//cout<<"dz = "<<dz<<" iter = "<<iter<<" a="<<a<<" b="<<b<<" b-a = "<<b-a<<" maxstep = "
		//<<maxStep<<" step size = "<<stepSize<<endl;
		//cout<<endl;
		//if(iter > 20) getchar();
	}  
	//getchar();
	if(fabs(stepSize - maxStep) <= b-a) 
	{
		stepSize = maxStep;
		//cout<<"step size is taken as the maximum"<<endl;
	}
	//cout<<"after bisec, dz = "<<dz<<" iter = "<<iter<<" b-a="<<b-a<<" step size = "<<stepSize<<endl;
	//cout<<"after bisec, dz = "<<dz<<" iter = "<<iter<<" b-a="<<b-a<<" step size = "<<stepSize<<endl;
	////getchar();
	//stepSize =1;
	
	if (dz<0)
	{
		//cout<<"                    stepsize:"<<stepSize<<endl;
		return 0;
	}
		
	else
	{
		
		return 1;
	}
		
}

int PTNET::InitializeHyperpathLC(PTNode* dest)
{
	PTNode *node;
    PTLink *link;
	std::deque<PTNode*> Q;
    for(int i = 0;i<numOfNode;i++)//chushihua
    {
        node = nodeVector[i];
		node->StgElem->cost = POS_INF_FLOAT;//stgelem->stglink初始化；stgelem->cost初始化
		node->StgElem->vialink = NULL;
        node->scanStatus = 0;
		node->m_wait = 0.0;
		node->m_attProb.clear();
    }
	for(int i = 0;i<numOfLink;i++)	linkVector[i]->stglinkptr = NULL;//ptlink->stglink

	//构造dest的树
	dest->StgElem->cost = 0;
    dest->scanStatus = 1;//1没有被检查
	Q.push_back(dest); //initialize Q

	int niter = 0;
    deque<PTNode*>::iterator firstElem;
	while(!Q.empty())
	{
		niter++;
        firstElem = Q.begin();
        node = *firstElem;
        node->scanStatus = 0;//0已被检查
		Q.erase(firstElem);
		for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)//backStar -- incoming links
		{
			link = *pv;
            PTNode* tailNode = link->tail;
			
			double t = node->StgElem->cost + link->cost;
			
			if(tailNode->StgElem->cost > t  )
			{
				if(link->GetTransitLinkType() == PTLink::ABOARD)//对上车弧所对应的站点求解共线问题，获得link的prop
				{
					//construct common-line problem
					int nn = tailNode->forwStar.size();
					vector<PTLink*> candidatelinks;
					for(int i = 0;i<nn;i++)
                    {
						PTLink* vlink = tailNode->forwStar[i];
						if(vlink->GetTransitLinkType() == PTLink::ABOARD && vlink->head->StgElem->cost < POS_INF_FLOAT)//要求头节点被搜索过	
						{
							vlink->tmpuse = vlink->head->StgElem->cost + vlink->cost;
							candidatelinks.push_back(vlink);				
						}
					}

					if (candidatelinks.size()>0)
					{
						vector<PTLink*> attlinks;
						floatType ct,wt;

						GenerateBoardingStg(candidatelinks,attlinks,ct,wt);//根据freq计算站点的策略集合，以及策略的等待时间，boarding_link的使用概率

						if(ct < tailNode->StgElem->cost)
						{
							if(tailNode->scanStatus == 0)  //tailnode is not in Q
							{
								Q.push_back(tailNode);
								tailNode->scanStatus = 1;
							}
							
							tailNode->m_wait = wt;
							tailNode->StgElem->cost = ct;
							tailNode->CleanStgLinksOnHyperPath();

							// set stg link pointer and link_prob
							PTRTRACE pa = attlinks.begin();
							PTLink* curlink = *pa;
							tailNode->m_attProb.push_back(curlink->m_probdata);
					
							tailNode->StgElem->vialink = curlink;
							pa++;
							while (pa != attlinks.end())
							{
								curlink->stglinkptr = (*pa);													
								curlink = (*pa);
								tailNode->m_attProb.push_back(curlink->m_probdata);
								pa++;
							}

							//cout<<endl;
						}
					}
				}
				else
				{
					
					if(tailNode->scanStatus == 0)  //tailnode is not in Q
					{                              
						Q.push_back(tailNode);
						tailNode->scanStatus = 1;
					}
					
					
					if (tailNode->GetTransitNodeType()==PTNode::TRANSFER) 
						tailNode->CleanStgLinksOnHyperPath();//针对换乘节点而言
					else
						tailNode->m_attProb.clear();//link的使用概率
					tailNode->StgElem->cost = t;
					tailNode->StgElem->vialink = link;		
					tailNode->m_attProb.push_back(1.0);
					//if(tailNode->m_attProb.size()>1) 
					//{
					//	cout<<tailNode->GetTransitNodeType()<<endl;
					//	system("PAUSE");
					//}
					tailNode->m_wait = 0.0;
				}
			}				
		}
	}
	//cout<<"cishulc: "<<niter<<endl;
	return niter;

	return 0;
}

int	PTNET::InitializeHyperpathLS_1(PTNode* org1,PTNode* dest)
{
	PTNode *node;
    PTLink *link;

    multimap<double, PTNode*, less<double> > Q;
    for(int i = 0;i<numOfNode;i++)//chushihua
    {
        node = nodeVector[i];
		node->StgElem->cost = POS_INF_FLOAT;
		node->StgElem->vialink = NULL;
        node->scanStatus = 0;
		node->m_wait = 0.0;
		node->m_attProb.clear();
    }

	for(int i = 0;i<numOfLink;i++)	
	{
		linkVector[i]->stglinkptr = NULL; 
	} 

	
	dest->StgElem->cost = 0;
    dest->scanStatus = 1;
    Q.insert(std::pair<double, PTNode*>(0.0, dest)); //initialize Q

	int niter = 0;
    multimap<double, PTNode*, less<double> >::iterator firstElem;

	firstElem = Q.begin();
	while(firstElem->second->id != org1->id && !Q.empty() )//
    {
        niter++;
		//cout<<Q.size()<<endl;
        firstElem = Q.begin();
        node = firstElem->second;
        node->scanStatus = 0;
		Q.erase(firstElem);
		for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
		{
			link = *pv;
            PTNode* tailNode = link->tail;
		
			double t = node->StgElem->cost + link->cost;
			if(tailNode->StgElem->cost > t  )
			{
				if(link->GetTransitLinkType() == PTLink::ABOARD)
				{
					//construct common-line problem
					int nn = tailNode->forwStar.size();
					vector<PTLink*> candidatelinks;
					for(int i = 0;i<nn;i++)
                    {
						PTLink* vlink = tailNode->forwStar[i];
						if(vlink->GetTransitLinkType() == PTLink::ABOARD && vlink->head->StgElem->cost < POS_INF_FLOAT)	
						{
							vlink->tmpuse = vlink->head->StgElem->cost + vlink->cost;
							candidatelinks.push_back(vlink);				
						}
					}
					if (candidatelinks.size()>0)
					{
						vector<PTLink*> attlinks;
						floatType ct,wt;

						GenerateBoardingStg(candidatelinks,attlinks,ct,wt);
						if(ct < tailNode->StgElem->cost)
						{
							if(tailNode->scanStatus == 0)  //tailnode is not in Q
							{
								Q.insert(std::pair<double, PTNode*>(ct, tailNode));
								tailNode->scanStatus = 1;
							}
							else
							{
								multimap<double, PTNode*, less<double> >::iterator lower =Q.lower_bound(tailNode->StgElem->cost - 1e-10),
									upper = Q.upper_bound(tailNode->StgElem->cost + 1e-10), piter;
								piter = lower;
								while(piter!=upper && piter->second!=tailNode)
								{
									piter++;
								}
								if(piter == Q.end()) 
								{
									cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
									cout<<"\tCurrently, Q has the following nodes"<<endl;
                                   
										for(lower = Q.begin(); lower!=Q.end();lower++)
											cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

									return 1;
								}
								if(piter->second!=tailNode)
								{
									cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
									return 1;
								}
								else
								{
									Q.erase(piter);
									Q.insert(std::pair<double, PTNode*>(ct, tailNode));
								}
                                
							}

							tailNode->m_wait = wt;
							tailNode->StgElem->cost = ct;
							tailNode->CleanStgLinksOnHyperPath();

							// set stg link pointer
							PTRTRACE pa = attlinks.begin();
							PTLink* curlink = *pa;
							tailNode->m_attProb.push_back(curlink->m_probdata);
					
							tailNode->StgElem->vialink = curlink;
							pa++;
							while (pa!=attlinks.end())
							{
								curlink->stglinkptr = (*pa);													
								curlink = (*pa);
								tailNode->m_attProb.push_back(curlink->m_probdata);
								pa++;
							}

							//cout<<endl;
						}
					}
				}
				else
				{
					if(tailNode->scanStatus == 0)  //tailnode is not in Q
					{                              
						Q.insert(std::pair<double, PTNode*>(t, tailNode));
						tailNode->scanStatus = 1;
					}
					else
					{
						multimap<double, PTNode*, less<double> >::iterator lower =Q.lower_bound(tailNode->StgElem->cost - 1e-10),
							upper = Q.upper_bound(tailNode->StgElem->cost + 1e-10), piter;

						piter = lower;
						while(piter!=upper && piter->second!=tailNode)
						{
							piter++;
						}
						if(piter == Q.end()) 
						{
							cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
							cout<<"\tCurrently, Q has the following nodes"<<endl;
                                   
								for(lower = Q.begin(); lower!=Q.end();lower++)
									cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

							return 1;
						}
						if(piter->second!=tailNode)
						{
							cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
							return 1;
						}
						else
						{
							Q.erase(piter);
							Q.insert(std::pair<double, PTNode*>(t, tailNode));                                    
						}
					}

					if (tailNode->GetTransitNodeType()==PTNode::TRANSFER) 
						tailNode->CleanStgLinksOnHyperPath();
					else
						tailNode->m_attProb.clear();
					tailNode->StgElem->cost = t;
					tailNode->StgElem->vialink = link;
					tailNode->m_attProb.push_back(1.0);
					//if(tailNode->m_attProb.size()>1) 
					//{
					//	cout<<tailNode->GetTransitNodeType()<<endl;
					//	system("PAUSE");
					//}
					tailNode->m_wait = 0.0;
				}				
			}
			
		}
	}
	if (firstElem->second->id == org1->id)
	{
		cout<<"over"<<endl;
	}
	else
	{
		cout<<"false"<<endl;
	}
	return 0;
}

int	PTNET::InitializeHyperpathLS(PTNode* dest)
{
	PTNode *node;
    PTLink *link;
    multimap<double, PTNode*, less<double> > Q;//检查序列，带排序（从小到大）
    for(int i = 0;i<numOfNode;i++)//初始化节点的状态（最短树上：节点的标号（费用），使用的路径，被检查的状态以及换乘节点上车弧的集合）
    {
        node = nodeVector[i];
		node->StgElem->cost = POS_INF_FLOAT;
		node->StgElem->vialink = NULL;
        node->scanStatus = 0;
		node->m_wait = 0.0;
		node->m_attProb.clear();
    }


	for(int i = 0;i<numOfLink;i++)	
	{
		linkVector[i]->stglinkptr = NULL; 
	}


	dest->StgElem->cost = 0;
    dest->scanStatus = 1;
    Q.insert(std::pair<double, PTNode*>(0.0, dest)); //initialize Q

	int niter = 0;
    multimap<double, PTNode*, less<double> >::iterator firstElem;
	while(!Q.empty())
    {
        niter++;
		//cout<<"第 "<<niter<<" 次"<<endl;
        firstElem = Q.begin();//取出首节点
        node = firstElem->second;
		/*if(node->m_stop->m_name=="8" && node->GetTransitNodeType()==PTNode::TRANSFER)
			{
				cout<<"node 8 : "<<node->StgElem->cost<<endl;
			}*/
        node->scanStatus = 0;
		Q.erase(firstElem);//移除首节点

		//分析进入分析节点的弧
		for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
		{
			link = *pv;
            PTNode* tailNode = link->tail;
		
			double t = node->StgElem->cost + link->cost;
			
			if(tailNode->StgElem->cost > t )//判断是否需要更新尾节点的标号
			{
				if(link->GetTransitLinkType() == PTLink::ABOARD)//如果是进入弧为上车弧（尾节点是换乘节点）
				{
					//construct common-line problem（求解共线问题）
					int nn = tailNode->forwStar.size();
					//构建上车弧集合
					vector<PTLink*> candidatelinks;
					for(int i = 0;i<nn;i++)
                    {
						PTLink* vlink = tailNode->forwStar[i];
						if(vlink->GetTransitLinkType() == PTLink::ABOARD && vlink->head->StgElem->cost < POS_INF_FLOAT)	//头节点需要被检查过，不然无法排序
						{
							vlink->tmpuse = vlink->head->StgElem->cost + vlink->cost;
							candidatelinks.push_back(vlink);				
						}
					}
					if (candidatelinks.size()>0)
					{
						vector<PTLink*> attlinks;
						floatType ct,wt;

						GenerateBoardingStg(candidatelinks,attlinks,ct,wt);//求解共线问题
						//判断是否需要更新尾节点
						if(ct < tailNode->StgElem->cost)
						{
							if(tailNode->scanStatus == 0)  //tailnode is not in Q
							{
								Q.insert(std::pair<double, PTNode*>(ct, tailNode));//更新检查序列
								tailNode->scanStatus = 1;
							}
							else
							{
								multimap<double, PTNode*, less<double> >::iterator lower =Q.lower_bound(tailNode->StgElem->cost - 1e-10),
									upper = Q.upper_bound(tailNode->StgElem->cost + 1e-10), piter;
								piter = lower;
								while(piter!=upper && piter->second!=tailNode)
								{
									piter++;
								}
								if(piter == Q.end()) 
								{
									cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
									cout<<"\tCurrently, Q has the following nodes"<<endl;
                                   
										for(lower = Q.begin(); lower!=Q.end();lower++)
											cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

									return 1;
								}
								if(piter->second!=tailNode)
								{
									cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
									return 1;
								}
								else
								{
									Q.erase(piter);
									Q.insert(std::pair<double, PTNode*>(ct, tailNode));
								}
                                
							}
							//cout<<"size:"<<attlinks.size()<<endl;
							tailNode->m_wait = wt;
							tailNode->StgElem->cost = ct;
							tailNode->CleanStgLinksOnHyperPath();

							// set stg link pointer（构造换乘节点处的上车弧集合）
							PTRTRACE pa = attlinks.begin();
							PTLink* curlink = *pa;
							tailNode->m_attProb.push_back(curlink->m_probdata);
					
							tailNode->StgElem->vialink = curlink;
							pa++;
							while (pa != attlinks.end())
							{
								curlink->stglinkptr = (*pa);													
								curlink = (*pa);
								tailNode->m_attProb.push_back(curlink->m_probdata);
								pa++;
							}

							//cout<<endl;
						}
					}
				}
				else
				{
					
					if(tailNode->scanStatus == 0)  //tailnode is not in Q
					{                              
						Q.insert(std::pair<double, PTNode*>(t, tailNode));
						tailNode->scanStatus = 1;
					}
					else
					{
						multimap<double, PTNode*, less<double> >::iterator lower = Q.lower_bound(tailNode->StgElem->cost - 1e-10),
							upper = Q.upper_bound(tailNode->StgElem->cost + 1e-10), piter;

						piter = lower;
						while(piter!=upper && piter->second!=tailNode)
						{
							piter++;
						}
						if(piter == Q.end()) 
						{
							cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
							cout<<"\tCurrently, Q has the following nodes"<<endl;
                                   
								for(lower = Q.begin(); lower!=Q.end();lower++)
									cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

							return 1;
						}
						if(piter->second!=tailNode)
						{
							cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
							return 1;
						}
						else
						{
							Q.erase(piter);
							Q.insert(std::pair<double, PTNode*>(t, tailNode));                                    
						}
					}

					if (tailNode->GetTransitNodeType()==PTNode::TRANSFER) //当前弧是步行弧，因此尾节点是换乘节点
						tailNode->CleanStgLinksOnHyperPath();
					else
						tailNode->m_attProb.clear();
					tailNode->StgElem->cost = t;
					tailNode->StgElem->vialink = link;
					tailNode->m_attProb.push_back(1.0);
					//if(tailNode->m_attProb.size()>1) 
					//{
					//	cout<<tailNode->GetTransitNodeType()<<endl;
					//	system("PAUSE");
					//}
					tailNode->m_wait = 0.0;
				}				
			}
		}
	}
	//cout<<"lscishu:"<<niter<<endl;
	//return niter;

	return 0;
}

int	PTNET::InitializeHyperpathLS_TP(PTNode* dest)
{
	PTNode *node;
    PTLink *link;
	std::multimap<double,PTNode*,less<double>> Q_now;
	std::multimap<double,PTNode*,less<double>> Q_next;
	//std::multimap<double,PTNode*,less<double>> Q_temp;

	for(int i = 0;i<numOfNode;i++)//chushihua
    {
        node = nodeVector[i];
		//node->transfer = 0;
		node->StgElem->cost = POS_INF_FLOAT;
		node->StgElem->vialink = NULL;
		node->StgElem_next->cost=POS_INF_FLOAT;
		node->StgElem_next->vialink=NULL;
		node->StgElem->scanstatus = 0;
		node->walkt=0;
		node->transfer=0;
		node->StgElem_next->scanstatus = 0;
		node->StgElem->walkcost=0.0;//
		node->m_wait = 0.0;
		node->m_attProb.clear();
    }


	for(int i = 0;i<numOfLink;i++)	
	{
		linkVector[i]->stglinkptr = NULL; 
	}

	dest->StgElem->cost = 0;
	//dest->scanStatus = 1;
	dest->StgElem->scanstatus = 1;
	dest->StgElem_next->cost=0;
	//cout<<dest->id<<endl;
	Q_now.insert(std::pair<double,PTNode*>(0.0,dest)); //initialize Q

	int niter = 0;
	int h = 0;
    multimap<double,PTNode*,less<double>>::iterator firstElem;
	while(h<=transfer_times)
	{
		if(h!=0)
		{
			Q_now.swap(Q_next);
			for (multimap<double,PTNode*,less<double>>::iterator iter=Q_now.begin();iter !=Q_now.end();iter++)
			{
				node=iter->second;
				node->StgElem->scanstatus=1;
				node->StgElem_next->scanstatus=0;
				//if(node->StgElem_next->cost>node->StgElem->cost)
				//node->CleanStgLinksOnHyperPath();//???????????????????
				node->m_attProb.clear();
				node->StgElem->tempcost=node->StgElem->cost;
				node->StgElem->cost=node->StgElem_next->cost;
				node->StgElem->tempvialink=node->StgElem->vialink;
				node->StgElem->vialink=node->StgElem_next->vialink;
				node->m_attProb.push_back(1.0);
				node->m_wait = 0.0;
				node->transfer++;
			}
//================================================================================================//
			PTDestination* ptdest;
			for(int i=0;i<numOfPTDest;i++)
			{
				ptdest=PTDestVector[i];
				if(ptdest->destination==dest)
				{
					break;
				}
			}
			for(int i=0;i<ptdest->numOfOrg;i++)
			{
				TNM_HyperPath* path=new TNM_HyperPath();
				/*cout<<"h: "<<h<<endl;
				if(ptdest->destination->id==17&&ptdest->orgVector[i]->org->id==12)
				{
					cout<<endl;
				}*/
				if(path->InitializeHP(ptdest->orgVector[i]->org,dest));
				{
					/*cout<<"-----------"<<endl;
					cout<<ptdest->orgVector[i]->org->transfer<<endl;*/
					if(ptdest->orgVector[i]->org->transfer>h)
					{
						vector<PTNode*> tempnodes;
						PTOrg* temporg;
						vector<GLINK*> templinks;
						temporg=ptdest->orgVector[i];
						templinks=path->GetGlinks();
						for(int i=0;i<templinks.size();i++)
						{
							if(templinks[i]->m_linkPtr->head->StgElem->scanstatus==1)
							{
								tempnodes.push_back(templinks[i]->m_linkPtr->head);

								multimap<double, PTNode*, less<double> >::iterator lower =Q_now.lower_bound(templinks[i]->m_linkPtr->head->StgElem->cost - 1e-10),
										upper = Q_now.upper_bound(templinks[i]->m_linkPtr->head->StgElem->cost + 1e-10), piter;
								piter = lower;
								while(piter!=upper && piter->second!=templinks[i]->m_linkPtr->head)
								{
									piter++;
								}
								if(piter == Q_now.end()) 
								{
									cout<<"Warning: cannot find tailNode, it's cost is "<<templinks[i]->m_linkPtr->head->StgElem->cost<<endl;
									cout<<"\tCurrently, Q_now has the following nodes"<<endl;
                                   
										for(lower = Q_now.begin(); lower!=Q_now.end();lower++)
											cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

									return 1;
								}
								if(piter->second!=templinks[i]->m_linkPtr->head)
								{
									cout<<"\tError: failed to find tailNode "<<templinks[i]->m_linkPtr->head->id<<" while it is in the queue."<<endl;
									return 1;
								}
								else
								{
									piter->second->StgElem->scanstatus=0;
									Q_now.erase(piter);
									//Q_now.insert(std::pair<double, PTNode*>(ct, tailNode));

								}
							}
						}
						if(templinks[0]->m_linkPtr->tail->StgElem->scanstatus==1)
						{
							tempnodes.push_back(templinks[0]->m_linkPtr->tail);

							multimap<double, PTNode*, less<double> >::iterator lower =Q_now.lower_bound(templinks[i]->m_linkPtr->tail->StgElem->cost - 1e-10),
										upper = Q_now.upper_bound(templinks[i]->m_linkPtr->tail->StgElem->cost + 1e-10), piter;
								piter = lower;
								while(piter!=upper && piter->second!=templinks[i]->m_linkPtr->tail)
								{
									piter++;
								}
								if(piter == Q_now.end()) 
								{
									cout<<"Warning: cannot find tailNode, it's cost is "<<templinks[i]->m_linkPtr->tail->StgElem->cost<<endl;
									cout<<"\tCurrently, Q_now has the following nodes"<<endl;
                                   
										for(lower = Q_now.begin(); lower!=Q_now.end();lower++)
											cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

									return 1;
								}
								if(piter->second!=templinks[i]->m_linkPtr->tail)
								{
									cout<<"\tError: failed to find tailNode "<<templinks[i]->m_linkPtr->tail->id<<" while it is in the queue."<<endl;
									return 1;
								}
								else
								{
									piter->second->StgElem->scanstatus=0;
									Q_now.erase(piter);

								}
						}

						floatType tempcost=POS_INF_FLOAT;
						PTNode* tempnode;
						for(int i=0;i<tempnodes.size();i++)
						{
							tempnodes[i]->StgElem->cost=tempnodes[i]->StgElem->tempcost;
							tempnodes[i]->StgElem->vialink=tempnodes[i]->StgElem->tempvialink;
						}
						for(int i=0;i<tempnodes.size();i++)//??????
						{
							tempnodes[i]->StgElem->cost=tempnodes[i]->StgElem_next->cost;
							tempnodes[i]->StgElem->vialink=tempnodes[i]->StgElem_next->vialink;
							TNM_HyperPath* path1=new TNM_HyperPath();
							path1->InitializeHP1(temporg->org,dest);//???????
							if(path1->tempcost<tempcost)
							{
								tempcost=path1->tempcost;
								tempnode=tempnodes[i];
							}
							tempnodes[i]->StgElem->cost=tempnodes[i]->StgElem->tempcost;
							tempnodes[i]->StgElem->vialink=tempnodes[i]->StgElem->tempvialink;
						}
						if(tempnodes.size()>0)
						{
							tempnode->StgElem->scanstatus=1;
							tempnode->StgElem->cost=tempnode->StgElem_next->cost;
							tempnode->StgElem->vialink=tempnode->StgElem_next->vialink;
							Q_now.insert(std::pair<double,PTNode*>(tempnode->StgElem->cost,tempnode));
						}
					}
				}
			}
//==========================================================================//
		}
		while(!Q_now.empty())
		{
			niter++;
			firstElem = Q_now.begin();
			node = firstElem->second;
			node->StgElem->scanstatus = 0;
			Q_now.erase(firstElem);
			
			for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
			{
				link = *pv;
				PTNode* tailNode = link->tail;
		
				double t = node->StgElem->cost + link->cost;
				
				if(link->GetTransitLinkType() == PTLink::ABOARD)
				{
					if(tailNode->StgElem->cost > t )//&& t < tttraveltime//&& t<tailNode->StgElem_next->cost
					{
						
					
						//construct common-line problem
						int nn = tailNode->forwStar.size();
						vector<PTLink*> candidatelinks;
						for(int i = 0;i<nn;i++)
						{
							PTLink* vlink = tailNode->forwStar[i];
							if(vlink->GetTransitLinkType() == PTLink::ABOARD && vlink->head->StgElem->cost < POS_INF_FLOAT)	
							{
								vlink->tmpuse = vlink->head->StgElem->cost + vlink->cost;
								candidatelinks.push_back(vlink);				
							}
						}
						if (candidatelinks.size()>0)
						{
							vector<PTLink*> attlinks;
							floatType ct,wt;

							GenerateBoardingStg(candidatelinks,attlinks,ct,wt);
							if(ct < tailNode->StgElem->cost)
							{
								if(tailNode->StgElem->scanstatus == 0)  //tailnode is not in Q_now
								{                              
									Q_now.insert(std::pair<double,PTNode*>(ct,tailNode));
									tailNode->StgElem->scanstatus = 1;
							
								}								

								else
								{
									multimap<double, PTNode*, less<double> >::iterator lower =Q_now.lower_bound(tailNode->StgElem->cost - 1e-10),
										upper = Q_now.upper_bound(tailNode->StgElem->cost + 1e-10), piter;
									piter = lower;
									while(piter!=upper && piter->second!=tailNode)
									{
										piter++;
									}
									if(piter == Q_now.end()) 
									{
										cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
										cout<<"\tCurrently, Q_now has the following nodes"<<endl;
                                   
											for(lower = Q_now.begin(); lower!=Q_now.end();lower++)
												cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

										return 1;
									}
									if(piter->second!=tailNode)
									{
										cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
										return 1;
									}
									else
									{
										Q_now.erase(piter);
										Q_now.insert(std::pair<double, PTNode*>(ct, tailNode));

									}
                                
								}

								tailNode->m_wait = wt;
								tailNode->walkt=node->walkt;
								tailNode->StgElem->cost = ct;
								tailNode->transfer = node->transfer;
								//cout<<tailNode->id<<"ct:"<<ct<<endl;
								tailNode->CleanStgLinksOnHyperPath();
								//cout<<"size: "<<attlinks.size()<<endl;
								// set stg link pointer
								PTRTRACE pa = attlinks.begin();
								//cout<<tailNode->id<<":"<<attlinks.size()<<endl;
								PTLink* curlink = *pa;
								tailNode->m_attProb.push_back(curlink->m_probdata);
					
								tailNode->StgElem->vialink = curlink;
								//tailNode->StgElem_next->vialink=curlink;
								pa++;
								while (pa!=attlinks.end())
								{
									curlink->stglinkptr = (*pa);													
									curlink = (*pa);
									tailNode->m_attProb.push_back(curlink->m_probdata);
									pa++;
								}

								//cout<<endl;
							}
						}
						
					
					}
				}
				else if(link->GetTransitLinkType() == PTLink::WALK)
				{
					if(tailNode->StgElem_next->cost > t && tailNode->StgElem->cost > t && node->walkt+link->cost<=ttwalktime )//&& tailNode->StgElem->cost > t//&& t<tttraveltime
					{
						if(tailNode->StgElem_next->scanstatus == 0)  //tailnode is not in Q_next
						{                              
							Q_next.insert(std::pair<double,PTNode*>(t,tailNode));
							tailNode->StgElem_next->scanstatus = 1;
						}
						else
						{
							multimap<double, PTNode*, less<double> >::iterator lower =Q_next.lower_bound(tailNode->StgElem_next->cost - 1e-10),
								upper = Q_next.upper_bound(tailNode->StgElem_next->cost + 1e-10), piter;

							piter = lower;
							while(piter!=upper && piter->second!=tailNode)
							{
								piter++;
							}
							if(piter == Q_next.end()) 
							{
								cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
								cout<<"\tCurrently, Q_next has the following nodes"<<endl;
                                //cout<<"____________________"<<endl;   
								for(lower = Q_next.begin(); lower!=Q_next.end();lower++)
									cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem_next->cost<<endl;

								return 1;
							}
							if(piter->second!=tailNode)
							{
								cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
								return 1;
							}
							else
							{
								Q_next.erase(piter);
								Q_next.insert(std::pair<double, PTNode*>(t, tailNode));
								
							}
						  }

						tailNode->StgElem_next->cost = t;
						tailNode->StgElem_next->scanstatus=1;
						tailNode->walkt=node->walkt+link->cost;
						/*if (tailNode->GetTransitNodeType()==PTNode::TRANSFER) 
							tailNode->CleanStgLinksOnHyperPath();
					    else
							tailNode->m_attProb.clear();*/
						tailNode->transfer = node->transfer;
						//cout<<tailNode->id<<" next:"<<t<<endl;
						tailNode->StgElem_next->vialink = link;
						//tailNode->StgElem->vialink = link;
						//tailNode->m_attProb.push_back(1.0);
						//tailNode->m_wait = 0.0;

					}
				}
					
				else
					if(tailNode->StgElem->cost > t )//&& t<tttraveltime// && t<tailNode->StgElem_next->cost 
					{
						if(tailNode->StgElem->scanstatus == 0)  //tailnode is not in Q_now
						{                              
							Q_now.insert(std::pair<double,PTNode*>(t,tailNode));
							tailNode->StgElem->scanstatus = 1;
						}
						else
						{
							multimap<double, PTNode*, less<double> >::iterator lower =Q_now.lower_bound(tailNode->StgElem->cost - 1e-10),
								upper = Q_now.upper_bound(tailNode->StgElem->cost + 1e-10), piter;

							piter = lower;
							while(piter!=upper && piter->second!=tailNode)
							{
								piter++;
							}
							if(piter == Q_now.end()) 
							{
								cout<<"Warning: cannot find tailNode, it's cost is "<<tailNode->StgElem->cost<<endl;
								cout<<"\tCurrently, Q_now has the following nodes"<<endl;
                                //cout<<"=========================="<<endl;   
									for(lower = Q_now.begin(); lower!=Q_now.end();lower++)
										cout<<lower->second->id<<", "<<lower->first<<","<<lower->second->StgElem->cost<<endl;

								return 1;
							}
							if(piter->second!=tailNode)
							{
								cout<<"\tError: failed to find tailNode "<<tailNode->id<<" while it is in the queue."<<endl;
								return 1;
							}
							else
							{
								Q_now.erase(piter);
								Q_now.insert(std::pair<double, PTNode*>(t, tailNode));                                    
							}
						}
						if (tailNode->GetTransitNodeType()==PTNode::TRANSFER) 
							tailNode->CleanStgLinksOnHyperPath();
					    else
							tailNode->m_attProb.clear();

						tailNode->StgElem->cost = t;
						tailNode->walkt=node->walkt;
						tailNode->transfer = node->transfer;
						tailNode->StgElem->vialink = link;
						//tailNode->StgElem_next->vialink=link;
						tailNode->m_attProb.push_back(1.0);
						tailNode->m_wait = 0.0;

					
					}
				
				
			}
		}
		h++;
		if (Q_next.empty())
		{
			return 0;
		}


	}

	return 0;
}

int	PTNET::InitializeHyperpathLS_TP2(PTNode* dest)
{
	//cout<<"dest id: "<<dest->id<<endl;
	/*if(dest->id==11)
	{
		cout<<endl;
	}*/
	PTNode *node;
	PTLink *link;
	multimap<double, TPHYPERPATHELEM*,less<double>> Q_now,Q_next;
	//cout<<"!!!!!!!!!"<<endl;
	ReleaseMLElemMap();
	//cout<<"@@@@@@@@"<<endl;
	for(int i = 0;i<numOfNode;i++)
    {
        node = nodeVector[i];
		node->m_labels = new LabelsMap; 
    }
	bestLastSol->cost = POS_INF_FLOAT;//
	TPHYPERPATHELEM* pElem = new TPHYPERPATHELEM;
	pElem->transfers  = 0;
	pElem->scanStatus = 1;
	pElem->cost       = 0.0;
	pElem->node       = dest;
	pElem->isInserted = true;
	pElem->walkcost=0.0;
	string ts         = to_string(pElem->transfers);//destnaiton transfers;state//+"-"+to_string(pElem->walkcost)
	dest->m_labels->insert(pair<string,TPHYPERPATHELEM*>(ts,pElem));
	Q_now.insert(std::pair<double,TPHYPERPATHELEM*>(0.0,pElem)); 
	multimap<double, TPHYPERPATHELEM*,less<double>>::iterator firstElem;
	TPHYPERPATHELEM *hElem,*tElem = NULL;
	LabelsMapIter iter;
	int niter = 0;
	int n = 0;
	//int trans = transfer_times;
	count=0;
	int chckedLabels =0;
	PTNode* orig;
	for(int transIter = 0;transIter <= transfer_times;transIter++)
	{
		niter++;
		//cout<<niter<<endl;
		while(!Q_now.empty())
		{
			firstElem = Q_now.begin();
			hElem = firstElem->second;
			hElem->scanStatus=0;
			node = hElem->node;
			int curTrans = hElem->transfers; //the modal tansfer number from current node to destination. 
			floatType curwalktime=hElem->walkcost;
			Q_now.erase(firstElem);

			chckedLabels++;
			for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
			{
				link =*pv;			
				n++;
				tempcountc++;
				if(pElem->isInserted == true)
				{
					pElem = new TPHYPERPATHELEM;
				}
				else
				{
					pElem->ResetLabel();
				}

				pElem->transfers = curTrans;
				pElem->walkcost = curwalktime;
				PTNode* tailNode = link->tail;
				//cout<<"tailnode id: "<<tailNode->id<<endl;
				/*if(tailNode->id==19)
				{
					cout<<endl;
				}*/
				double t = hElem->cost + link->cost; 
				if(link->GetTransitLinkType() == PTLink::ABOARD)
				{
					if(!tailNode->GetMinLabeForFindSHP(curTrans,curwalktime)|| t < tailNode->GetMinLabeForFindSHP(curTrans,curwalktime)->cost)
					{
						map<int,TPHYPERPATHELEM*> candidatelinks;
						hElem->tempuse = t;
						candidatelinks.insert(std::pair<int,TPHYPERPATHELEM*>(link->id,hElem)); 
						int nn = tailNode->forwStar.size();

						for(int i = 0;i<nn;i++)
						{
							PTLink* vlink = tailNode->forwStar[i];
							PTNode* hnode = vlink->head;
							TPHYPERPATHELEM* hlabel = hnode->GetMinAcyclicLabel();
							if(hlabel && vlink != link && vlink->GetTransitLinkType() == PTLink::ABOARD && hlabel->transfers <= curTrans )// && hlabel->walkcost<=curwalktime
							{
								hlabel->tempuse = hlabel->cost + vlink->cost;
								pair<map<int,TPHYPERPATHELEM*>::iterator,bool> ret = candidatelinks.insert(std::pair<int,TPHYPERPATHELEM*>(vlink->id,hlabel));
								if(!ret.second)
								{
									cout<<"The label not be inserted the candidate set！ Node id:"<<hlabel->node->id<<" transfers:"<<hlabel->transfers <<"!!!"<<endl;
								}
							}
						}
						if (candidatelinks.size()>0)
						{
							if(GetAttSetAndStateMLMMEAP(tailNode,candidatelinks,pElem,hElem))
							{
								continue;
							}
							//cout<<pElem->node->id<<endl;
							//cout<<"++++"<<endl;
							/*if(pElem->node->id==10&& dest->id==6)
							{cout<<"!"<<endl;}*/
							//if(DominanceCheck(tailNode,tElem,pElem,pElem->transfers,link,pElem->walkcost))
							//{	
							//	pElem->node = tailNode;
							//	if(pElem->node->id==65)//4658
							//	{
							//		cout<<endl;
							//	}
							//}
							if(DominanceCheck(tailNode,tElem,pElem,curTrans,link,curwalktime))
							{
								if(!tElem)//pElem not in tail node LabelsMap
								{
									pElem->scanStatus = 1;
									Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(pElem->cost,pElem));
									//cout<<"111"<<endl;
									tailNode->InsertLabel(pElem);//
									//cout<<"insert pElem: ";
									//pElem->PrintStgLabels();
									//cout<<"-----"<<endl;
									pElem->isInserted = true;
								}
								else 
								{	
									if(tElem->scanStatus == 0)  //tailnode is not in Q
									{										
										if(tElem->transfers >= curTrans)
										{
											UpdateLabel(tElem,pElem);
											tElem->scanStatus = 1;
											Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(tElem->cost,tElem));								
										}
										else  
										{
											pElem->scanStatus = 1;
											Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(pElem->cost,pElem));
											//cout<<"+"<<endl;
											tailNode->InsertLabel(pElem);
											//cout<<"insert pElem: ";
											//pElem->PrintStgLabels();
											//cout<<"-"<<endl;
											pElem->isInserted = true;
										}
									}
									else
									{
											
										if(tElem->transfers == curTrans)
										{
											ReInsertLabelML(tElem,pElem,Q_now);
											UpdateLabel(tElem,pElem);
										}
										else if(tElem->transfers > curTrans)
										{
											EraseLabelML(tElem,Q_next);
											UpdateLabel(tElem,pElem);
											Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(tElem->cost,tElem));
										}
										else
										{
											cout<< "The tELem scanStatus is 1, but it's transfers < curTrans"<<endl;
											system("pause"); 
										}
									}
								}
							}
						}
					}
				}
				else
				{
					pElem->cost  = t;
					//pElem->transfers=hElem->transfers;
					//if(hElem->vialink)
					//{
					//	if(link->GetTransitLinkType() == PTLink::WALK && hElem->vialink->GetTransitLinkType() == PTLink::WALK)//Avoid circle
					//	{
					//		continue;
					//	}
					//}
					{
						if(link->GetTransitLinkType()==PTLink::WALK)
						{
							pElem->transfers++;
							pElem->walkcost+=link->cost;
						}
						if(link->GetTransitLinkType()==PTLink::TRANSFER)
						{
							pElem->transfers++;
						}
						//cout<<"other"<<endl;
						//if(DominanceCheck(tailNode,tElem,pElem,pElem->transfers,link,pElem->walkcost))
						//{	
						//	pElem->node = tailNode;
						//	if(pElem->node->id==4658)//4658
						//	{
						//		cout<<endl;
						//	}
						//}	
						if(DominanceCheck(tailNode,tElem,pElem,pElem->transfers,link,pElem->walkcost))
						{	
							pElem->node = tailNode;
							pElem->vialink = link;
							pElem->stgLabels.push_back(hElem);
							pElem->m_attProb.push_back(1.0);
							pElem->m_wait = 0.0;
							UpdateLabelLSML(tailNode,link,tElem,pElem,Q_now,Q_next);
						}	
						//cout<<"over"<<endl;
					}
					
				}
				/*TPHYPERPATHELEM* label=tailNode->GetMinLabelOnTransfers(transfer_times,ttwalktime);
				if(label)
				{
					cout<<label->cost<<endl;
				}
				else
				{
					cout<<endl;
				}*/
			}
		}
		//cout<<"cishu: "<<transIter<<" size: "<<Q_next.size()<<endl;
		if(!Q_next.empty())
		{
			ClearQNow(Q_now);
			Q_now = Q_next;
			Q_next.clear();
		}
		else
			break;
	}
	//cout<<"----------------------"<<endl;
	//cout<<Q_next.size()<<endl;
	tempuse = chckedLabels;
	/*for(int i = 0;i<numOfNode;i++)
	{
		PTNode *node = nodeVector[i];
		cout<<node->id<<endl;
		if(node->id ==26)
		{
			cout<<endl;
		}
	}*/
	//cout<<"over"<<endl;
	return 0;
}

int PTNET::Swap_stl(multimap<double,PTNode*,less<double>> Q_now,multimap<double,PTNode*,less<double>> Q_next)
{
	Q_now.swap(Q_next);
	for (multimap<double,PTNode*,less<double>>::iterator it=Q_now.begin();it !=Q_now.end();it++)
	{
		PTNode*node;
		node =it->second;
		if (node->GetTransitNodeType() == PTNode::TRANSFER)
		{
			node->CleanStgLinksOnHyperPath();
				
		}
		else
		{
			node->m_attProb.clear();
		}
		node->StgElem->cost=node->StgElem_next->cost;
		node->StgElem->vialink=node->StgElem_next->vialink;
		node->m_attProb.push_back(1.0);
		node->m_wait = 0.0;
		node->StgElem->scanstatus = 1;
		node->StgElem_next->scanstatus = 0;
		node->transfer++;
	}
	return 0;
}

int  PTNET::Topological(PTNode* dest)
{
	PTNode *node;
    PTLink *link;
	std::deque<PTNode*> Q_now;
	std::deque<PTNode*> Q_next;

	for(int i = 0;i<numOfNode;i++)//chushihua
    {
        node = nodeVector[i];
		node->StgElem->cost = POS_INF_FLOAT;
		node->StgElem_next->cost = POS_INF_FLOAT;
		node->StgElem->vialink = NULL;
		node->StgElem_next->vialink = NULL;
		node->m_wait = 0.0;
		node->m_attProb.clear();
    }


	for(int i = 0;i<numOfLink;i++)	
	{
		linkVector[i]->stglinkptr = NULL; 
	}

	dest->StgElem->cost = 0;
	dest->StgElem_next->cost=0;
	Q_now.push_back(dest); //initialize Q

	int niter = 0;
	int k = 2;//transfer次数
	int h = 0;
    deque<PTNode*>::iterator firstElem;
	while(h<=k)
	{
		while(!Q_now.empty())
		{
			niter++;
			firstElem = Q_now.begin();
			node = *firstElem;
			Q_now.erase(firstElem);
			//cout<<"+++++++++++++++++++++++++"<<endl;
			//cout<<"node id:"<<node->id<<endl;
			for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
			{
				link = *pv;
				PTNode* tailNode = link->tail;
				//cout<<"tailNode id:"<<tailNode->id<<endl;		
				double t = node->StgElem->cost + link->cost;

				/*if (node->GetTransitNodeType()==PTNode::TRANSFER && tailNode->GetTransitNodeType()==PTNode::TRANSFER)
				{
					cout<<"yes"<<endl;
				}*/
				
				if (link->GetTransitLinkType() == PTLink::WALK)
				{
					if (tailNode->StgElem_next->cost > t )
					{
						deque<PTNode*>::iterator ret1;
						ret1=std::find(Q_next.begin(),Q_next.end(),tailNode);
						if(ret1==Q_next.end())  //tailnode is not in Q_next
						{        
							Q_next.push_back(tailNode);
							//cout<<"tailNode id:"<<tailNode->id<<endl;
						}
						tailNode->StgElem_next->cost = t;
						tailNode->StgElem_next->vialink = link;	
					}
					
				}
				else
				{
					if (tailNode->StgElem->cost > t  )
					{	
						deque<PTNode*>::iterator ret1;
						ret1=std::find(Q_now.begin(),Q_now.end(),tailNode);
						if(ret1==Q_now.end())  //tailnode is not in Q_now
						{        
							Q_now.push_back(tailNode);
						}
						tailNode->StgElem->cost = t;
						tailNode->StgElem->vialink = link;    
					}
					
				}
						
						//if(tailNode->m_attProb.size()>1) 
						//{
						//	cout<<tailNode->GetTransitNodeType()<<endl;
						//	system("PAUSE");
						//}
						

				
				
			}
			if (node == nodeVector[0])
			{
				cout<<h<<"次换乘的结果:"<<nodeVector[0]->StgElem->cost<<endl;
			}
			
		}
		
		//cout<<"______________________"<<endl;
		for (deque<PTNode*>::iterator it=Q_next.begin();it !=Q_next.end();it++)
		{
			node =*it;
			node->StgElem->cost=node->StgElem_next->cost;
			node->StgElem->vialink=node->StgElem_next->vialink;
		}
		Q_now.swap(Q_next);
		//cout<<"Q_now:"<<Q_now.size()<<endl;
		h++;
	}
	return 0;
}

int  PTNET::Topological_2(PTNode* dest)
{
	PTNode *node;
    PTLink *link;
	std::deque<PTNode*> Q_now;
	std::deque<PTNode*> Q_next;

	for(int i = 0;i<numOfNode;i++)//chushihua
    {
        node = nodeVector[i];
		node->StgElem->cost = POS_INF_FLOAT;
		node->StgElem_next->cost = POS_INF_FLOAT;
		node->StgElem->vialink = NULL;
		node->StgElem_next->vialink = NULL;
		node->m_wait = 0.0;
		node->m_attProb.clear();
    }


	for(int i = 0;i<numOfLink;i++)	
	{
		linkVector[i]->stglinkptr = NULL; 
	}

	dest->StgElem->cost = 0;
	dest->StgElem_next->cost=0;
	Q_now.push_back(dest); //initialize Q

	int niter = 0;
	int k = 2;//transfer次数
	int h = 0;
    deque<PTNode*>::iterator firstElem;
	while(h<=k)
	{
		while(!Q_now.empty())
		{
			niter++;
			firstElem = Q_now.begin();
			node = *firstElem;
			Q_now.erase(firstElem);
			//cout<<"+++++++++++++++++++++++++"<<endl;
			//cout<<"node id:"<<node->id<<endl;
			for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
			{
				link = *pv;
				PTNode* tailNode = link->tail;
				//cout<<"tailNode id:"<<tailNode->id<<endl;		
				double t = node->StgElem->cost + link->cost;

				/*if (node->GetTransitNodeType()==PTNode::TRANSFER && tailNode->GetTransitNodeType()==PTNode::TRANSFER)
				{
					cout<<"yes"<<endl;
				}*/
				
				if (link->GetTransitLinkType() == PTLink::ALIGHT && link->head != dest)
				{
					if (tailNode->StgElem_next->cost > t )
					{
						deque<PTNode*>::iterator ret1;
						ret1=std::find(Q_next.begin(),Q_next.end(),tailNode);
						if(ret1==Q_next.end())  //tailnode is not in Q_next
						{        
							Q_next.push_back(tailNode);
							//cout<<"tailNode id:"<<tailNode->id<<endl;
						}
						tailNode->StgElem_next->cost = t;
						tailNode->StgElem_next->vialink = link;	
					}
					
				}
				else
				{
					if (tailNode->StgElem->cost > t  )
					{	
						deque<PTNode*>::iterator ret1;
						ret1=std::find(Q_now.begin(),Q_now.end(),tailNode);
						if(ret1==Q_now.end())  //tailnode is not in Q_now
						{        
							Q_now.push_back(tailNode);
						}
						tailNode->StgElem->cost = t;
						tailNode->StgElem->vialink = link;    
					}
					
				}
						
						//if(tailNode->m_attProb.size()>1) 
						//{
						//	cout<<tailNode->GetTransitNodeType()<<endl;
						//	system("PAUSE");
						//}
						

				
				
			}
			if (node == nodeVector[0])
			{
				cout<<h<<"次换乘的结果:"<<nodeVector[0]->StgElem->cost<<endl;
			}
			
		}
		
		//cout<<"______________________"<<endl;
		for (deque<PTNode*>::iterator it=Q_next.begin();it !=Q_next.end();it++)
		{
			node =*it;
			node->StgElem->cost=node->StgElem_next->cost;
			node->StgElem->vialink=node->StgElem_next->vialink;
		}
		Q_now.swap(Q_next);
		//cout<<"Q_now:"<<Q_now.size()<<endl;
		h++;
	}
	return 0;
}

//bool TNM_HyperPath::InitializeHP_TP(PTNode *org, PTNode *dest)
//{
//	//cout<<"dest id:"<<dest->id<<"   ->   ";
//	
//	//cout<<"org:"<<org->id<<",cost:"<<org->StgElem->cost<<endl;
//	if(dest->StgElem->vialink != NULL || org->StgElem->vialink == NULL)//destination node must carry no outgoing hyperpath
//    {
//		/*if (org->StgElem->vialink == NULL)
//		{
//			cout<<org ->id<<"org       wrong"<<endl;
//		}
//		if (org->StgElem->vialink != NULL)
//		{
//			cout<<"dest wrong:  "<<dest->StgElem->vialink->tail<<"->"<<dest->StgElem->vialink->head<<endl;
//		}*/
//		//cout<<"\tHyper path tree was not properly set for od:"<<org->id<<","<<dest->id<<endl;
//        return false;
//	}
//	else
//	{
//		//cout<<"yes"<<endl;
//		std::queue<PTNode*> Q;  
//		Q.push(org);//push_back()
//		org->StgElem->scanstatus = 1;
//		while(!Q.empty())
//        {
//			PTNode* node = Q.front();
//			PTLink* link = node->StgElem->vialink;
//			while (link)
//			{
//				link->head->tmpNumOfIn++;
//				if (link->head->StgElem->scanstatus == 0)
//				{
//					link->head->StgElem->scanstatus = 1;
//					Q.push(link->head); 
//				}
//				link = link->stglinkptr;//?疯狂套娃
//			}
//			Q.pop();//pop_front()
//		}
//		//rescanning the hyperpath again to build the topological node order. 
//		Q.push(org);
//		org->m_tmpdata = 1.0;
//		AddGlinks(org);//goujianhp
//		//cout<<org->id<<"          waitcost:"<<org->m_wait<<"    data:"<<org->m_tmpdata<<endl;
//		int count = 0;
//		while(!Q.empty())
//        {
//			PTNode *node = Q.front();            
//            count++;
//			
//			node->StgElem->scanstatus = 0; //reset scan status;
//			//cout<<node->id<<"->";
//			PTLink* link = node->StgElem->vialink;
//			int ix = 0;
//			while (link)
//			{
//				link->head->tmpNumOfIn--;
//				link->head->m_tmpdata += node->m_tmpdata * node->m_attProb[ix];
//				//cout<<"link id:"<<link->id<<",tail node prob:"<<node->m_tmpdata<<", link app:"<<node->m_attProb[ix]<<endl;
//				if (link->head->tmpNumOfIn == 0)
//				{
//					AddGlinks(link->head);
//					//cout<<link->head->id<<"          waitcost:"<<link->head->m_wait<<"    data:"<<link->head->m_tmpdata<<endl;
//					Q.push(link->head);		
//				}
//				link = link->stglinkptr;
//				ix ++;
//			}
//			Q.pop();
//			//if (node==dest) cout<<node->m_tmpdata<<endl;
//			node->m_tmpdata = 0.0;
//		}
//		//cout<<endl;
//		//cout<<endl;
//	 }
//
//	if(m_links[m_links.size()-1]->m_linkPtr->head!=dest)
//	//if(m_nodes[m_nodes.size()-1]->m_ptnodePtr != dest)
//    {
//		//55cout<<m_links[m_links.size()-1]->m_linkPtr->GetTransitLinkTypeName()<<endl;
//		//cout<<"last dest id:"<<m_links[m_links.size()-1]->m_linkPtr->head->GetStopPtr()->m_id<<endl;
//		//cout<<"\tNo valid path between origin and destinaion. "<<endl;//直接删除org
//		
//		return false;
//	}
//	
//	return true;
//}

bool TNM_HyperPath::InitializeHP(PTNode *org, PTNode *dest)
{
	//cout<<"dest id:"<<dest->id<<"   ->   ";
	//cout<<"org:"<<org->id<<" cost:"<<org->StgElem->cost<<endl;
	org->walkt = 0;
	org->transfer = 0;

	if(dest->StgElem->vialink != NULL || org->StgElem->vialink == NULL)//destination node must carry no outgoing hyperpath
    {
		cout<<"\tHyper path tree was not properly set for od:"<<org->id<<","<<dest->id<<endl;
        return false;
    }
	else
	{
		std::queue<PTNode*> Q;  
		Q.push(org);//push_back()
		org->scanStatus = 1;
		while(!Q.empty())
        {
			PTNode* node = Q.front();
			PTLink* link = node->StgElem->vialink;

			//if(link)
			//{
			//	cout<<node->id<<" "<<link->id<<endl;
			//}
			//cout<<"----------------"<<endl;

	/*		cout<<link->id<<","<<link->head->id<<endl;
			cout<<link->id<<","<<link->head->id<<","<<link->head->tmpNumOfIn<<endl;
			cout<<"------------------------------"<<endl;*/

			while (link)
			{
				//cout<<link->id<<" "<<link->head->id<<endl;


				link->head->tmpNumOfIn++;//搜索一遍，获得每个节点的进入数量（方便更新、统计节点的使用概率）

				if (link->head->scanStatus == 0)
				{
					link->head->scanStatus = 1;
					Q.push(link->head); 
				}
				/*if (link->head->tmpNumOfIn>=2)
				{
					cout<<node->id<<"->"<<link->head->id<<"have hp  :"<<link->head->tmpNumOfIn<<endl;
				}*/
				link = link->stglinkptr;//仅针对站点的策略集（上车弧）而言
			}
			//cout<<"----------------"<<endl;
			Q.pop();//pop_front()  删除队列中的第一个元素
		}
		//rescanning the hyperpath again to build the topological node order. 
		Q.push(org);
		org->m_tmpdata = 1.0;
		AddGlinks(org);//按拓扑顺序更新hyperpath中的node和link（包含概率和等待时间）
		//cout<<org->id<<"          waitcost:"<<org->m_wait<<"    data:"<<org->m_tmpdata<<endl;
		//cout<<"org:"<<org->id<<" , "<<org->StgElem->cost<<endl;
		int count = 0;
		tempcost = 0;
		while(!Q.empty())
        {
			PTNode *node = Q.front();            
            count++;
			//cout<<node->id<<"->";//<<"("<<node->StgElem->cost<<","<<node->id<<")"

			node->scanStatus = 0; //reset scan status;

			PTLink* link = node->StgElem->vialink;
			int ix = 0;
			while (link)
			{
				tempcost+=link->cost;
				link->head->tmpNumOfIn--;
				link->head->m_tmpdata += node->m_tmpdata * node->m_attProb[ix];//更新头节点的概率（hyperpath中的节点）
				//cout<<"link id:"<<link->id<<",tail node prob:"<<node->m_tmpdata<<", link app:"<<node->m_attProb[ix]<<endl;
				if (link->head->tmpNumOfIn == 0)
				{
					AddGlinks(link->head);//更新hyperpath中的node和link
					//cout<<link->head->id<<"          waitcost:"<<link->head->m_wait<<"    data:"<<link->head->m_tmpdata<<endl;
					Q.push(link->head);		
				}
				//if(link->GetTransitLinkType()==PTLink::WALK)
				//{
				//	count++;
				//	org->walkt+=link->cost;
				//	org->transfer++;
				//}
				link = link->stglinkptr;
				ix ++;
			}
			Q.pop();
			//if (node==dest) cout<<node->m_tmpdata<<endl;
			node->m_tmpdata = 0.0;//检索完的节点，可以清空概率
		}
		//cout<<endl;
		//cout<<"walk: "<<org->walkt<<", transfer: "<<org->transfer<<endl;
		//org->transfer=count;
	 }//
	walktime = org->walkt;//更新OD对的步行时间

	if(m_links[m_links.size()-1]->m_linkPtr->head!=dest)
	//if(m_nodes[m_nodes.size()-1]->m_ptnodePtr != dest)
    {
		cout<<"\tNo valid path between origin and destinaion. "<<endl;//
		return false;
	}
	

	return true;
	
}

bool TNM_HyperPathTP::InitializeHP_TP(PTNode *org, PTNode *dest,int k,floatType walklimit)
{
	TPHYPERPATHELEM* OrgLabel = org->GetMinLabelOnTransfers(k,walklimit);
	//TPHYPERPATHELEM* OrgLabel = org->GetMinLabel();
	//org->walkt=OrgLabel->walkcost;
	//int count=0;
	org->transfer=0;
	org->walkt=0;
	if(dest->m_labels->size() < 1|| !OrgLabel)//destination node must carry no outgoing hyperpath
    {
		//cout<<"\tHyper path tree was not properly set for od:"<<org->id<<"->"<<dest->id<<" with number of transfers:" <<endl;
        return false;
    }
	else
	{
		std::queue<TPHYPERPATHELEM*> Q; 
		typedef vector<TPHYPERPATHELEM*>::iterator LITER;
		Q.push(OrgLabel);
		OrgLabel->scanStatus = 1;
		while(!Q.empty()) 
        {
			TPHYPERPATHELEM* tlabel = Q.front();
			int ix = 0;
			//cout<<tlabel->
			for (LITER ai = tlabel->stgLabels.begin();ai!= tlabel->stgLabels.end();ai++)
			{	
				if((*ai)->node)
				{
					(*ai)->node->tmpNumOfIn++;
					if ((*ai)->scanStatus == 0)
					{
						(*ai)->scanStatus = 1;
						Q.push((*ai)); 
					}
				}
				else
				{
					cout<<"wrong"<<endl;
					system("pause");
				}
				ix ++;
			}
			Q.pop();
		} 
		//cout<<"org:"<<org->id<<" , "<<OrgLabel->cost<<" walkcost: "<<OrgLabel->walkcost<<" ,transfer : "<<OrgLabel->transfers<<endl;
		//rescanning the hyperpath again to build the topological node order. 
		Q.push(OrgLabel);
		org->m_tmpdata = 1.0;
		AddGlinksTP(OrgLabel);
		//int count = 0;
		while(!Q.empty())
        {
			TPHYPERPATHELEM* tlabel = Q.front();    
			//cout<<tlabel->node->id<<"->";
            //count++;
			tlabel->scanStatus = 0; //reset scan status;
			int ix = 0;
			for (LITER ai = tlabel->stgLabels.begin();ai!= tlabel->stgLabels.end();ai++)
			{
				(*ai)->node->tmpNumOfIn--;
				(*ai)->node->m_tmpdata += tlabel->node->m_tmpdata * tlabel->m_attProb[ix];
				if ((*ai)->node->tmpNumOfIn == 0)
				{
					AddGlinksTP((*ai));
					Q.push((*ai));	
				}
				ix ++;
				
			}
			Q.pop();
			tlabel->node->m_tmpdata = 0.0;
		}
		org->transfer=OrgLabel->transfers;
		org->walkt=OrgLabel->walkcost;
		org->StgElem->cost=OrgLabel->cost;
		//cout<<endl;
		//cout<<" transfer: "<<org->transfer<<" walk: "<<org->walkt<<endl;
		if(m_links[m_links.size()-1]->m_linkPtr->head!=dest)
		{
			return false;
		}
		transfers = OrgLabel->transfers;
		walktime = OrgLabel->walkcost;
		moneySpent = OrgLabel->moneySpent;
		trvelTime  = OrgLabel->trvelTime;
		trvelDist  = OrgLabel->trvelDist;
	}
	return true;
}

TNM_HyperPathTP::TNM_HyperPathTP():TNM_HyperPath()
{
	transfers = -1;
	state = -1;
	walktime=0.0;
	moneySpent = 0.0;
	trvelTime  = 0.0;
	trvelDist  = 0.0;
}

void  TNM_HyperPathTP::AddGlinksTP(TPHYPERPATHELEM* tlabel)
{
	WaitCost += tlabel->node->m_tmpdata * tlabel->m_wait;
	typedef vector<TPHYPERPATHELEM*>::iterator LITER;
	int i = 0;
	PTLink* link;
	for (LITER ai = tlabel->stgLabels.begin();ai!= tlabel->stgLabels.end();ai++)
	{
		link = CatchLinkPtrTP(tlabel->node,(*ai)->node);
		
		if(link)
		{
			GLINK* newglink = new GLINK(link);
			name += std::to_string(link->id) + "-";
			newglink->m_data = tlabel->node->m_tmpdata * tlabel->m_attProb[i];		
			newglink->m_prob = tlabel->m_attProb[i];
			if(!tlabel->via_m_cost.empty())
			{
				newglink->gt_cost = link->cost + tlabel->via_m_cost[i];
				newglink->m_mcost = tlabel->via_m_cost[i];
			}
			else
			{
				newglink->gt_cost = link->cost;
			}
			//newglink->gt_cost = link->cost;
			m_links.push_back(newglink);	
			i++;
		}
		else
		{
			/*cout<<"The link is NULL! tail node:"<<tlabel->node->name<<" ,head node:"<<(*ai)->node->name<<endl;
			cout<<"The tlabel vialink:"<<tlabel->vialink->tail->name<<"->"<<tlabel->vialink->head->name<<endl;*/
		}
	}
}

PTLink*	TNM_HyperPathTP::CatchLinkPtrTP(PTNode* tail, PTNode *head)
{
	for (vector<PTLink*>::iterator pl = tail->forwStar.begin(); pl!=tail->forwStar.end(); pl++)
	{
		if((*pl))
		{
			if((*pl)->head == head)
				return (PTLink*)*pl;
		}
	}
	return NULL;
	
};

bool TNM_HyperPath::InitializeHP1(PTNode *org, PTNode *dest)
{
	//cout<<"dest id:"<<dest->id<<"   ->   ";
	//cout<<"org:"<<org->id<<" cost:"<<org->StgElem->cost<<",";

	if(dest->StgElem->vialink != NULL || org->StgElem->vialink == NULL)//destination node must carry no outgoing hyperpath
    {
		//cout<<"\tHyper path tree was not properly set for od:"<<org->id<<","<<dest->id<<endl;
        return false;
    }
	else
	{
		std::queue<PTNode*> Q;  
		Q.push(org);//push_back()
		org->scanStatus = 1;
		while(!Q.empty())
        {
			PTNode* node = Q.front();
			PTLink* link = node->StgElem->vialink;
			while (link)
			{
				link->head->tmpNumOfIn++;
				if (link->head->scanStatus == 0)
				{
					link->head->scanStatus= 1;
					Q.push(link->head); 
				}
				/*if (link->head->tmpNumOfIn>=2)
				{
					cout<<node->id<<"->"<<link->head->id<<"have hp  :"<<link->head->tmpNumOfIn<<endl;
				}*/
				link = link->stglinkptr;//套娃
			}
			Q.pop();//pop_front()
		}
		//rescanning the hyperpath again to build the topological node order. 
		Q.push(org);
		org->m_tmpdata = 1.0;
		AddGlinks(org);//goujianhp
		//cout<<org->id<<"          waitcost:"<<org->m_wait<<"    data:"<<org->m_tmpdata<<endl;
		int count = 0;
		tempcost=0;
		while(!Q.empty())
        {
			PTNode *node = Q.front();            
            //count++;
			//cout<<node->m_stop->m_name<<"->";//<<"("<<node->StgElem->cost<<","<<node->id<<")"

			node->scanStatus = 0; //reset scan status;

			PTLink* link = node->StgElem->vialink;
			int ix = 0;
			while (link)
			{
				tempcost+=link->cost;
				link->head->tmpNumOfIn--;
				link->head->m_tmpdata += node->m_tmpdata * node->m_attProb[ix];
				//cout<<"link id:"<<link->id<<",tail node prob:"<<node->m_tmpdata<<", link app:"<<node->m_attProb[ix]<<endl;
				if (link->head->tmpNumOfIn == 0)
				{
					AddGlinks(link->head);
					//cout<<link->head->id<<"          waitcost:"<<link->head->m_wait<<"    data:"<<link->head->m_tmpdata<<endl;
					Q.push(link->head);		
				}
				if(link->GetTransitLinkType()==PTLink::WALK)
				{count++;}
				link = link->stglinkptr;
				ix ++;
			}
			Q.pop();
			//if (node==dest) cout<<node->m_tmpdata<<endl;
			node->m_tmpdata = 0.0;
		}
		//cout<<"dest id:"<<dest->id<<"   ->   ";
		//cout<<"org:"<<org->id<<" cost:"<<org->StgElem->cost<<",";
		//cout<<","<<count<<endl;
		org->transfer=count;
	 }//

	if(m_links[m_links.size()-1]->m_linkPtr->head!=dest)
	//if(m_nodes[m_nodes.size()-1]->m_ptnodePtr != dest)
    {
		//cout<<"\tNo valid path between origin and destinaion. "<<endl;//
		return false;
	}
	

	return true;
	
}

void PTNode::CleanStgLinksOnHyperPath()
{
	//cout<<"links on the node:"<<m_attProb.size()<<endl;
	//cout<<"clear links for node:"<<id<<endl;
	m_attProb.clear();
	PTLink* link = StgElem->vialink;
	StgElem->vialink = NULL;
	while(link)
	{
		PTLink* plink = link->stglinkptr;//由当前link指向策略集中的其他link
		
		link->stglinkptr = NULL;
		link = plink;
	}
}

int PTNET::PTAllOrNothing(PTDestination* dest)
{
	PTOrg* org;
	{InitializeHyperpathLS(dest->destination);}
	for(int i=0;i<numOfNode;i++)
	{nodeVector[i]->scanStatus=0;}
	//InitializeHyperpathLS_TP(dest->destination);//求最短路,vialink
	//cout<<"--------------------------------------"<<endl;
	{
		for (int j = 0; j<dest->numOfOrg; j++)
		{
			TNM_HyperPath* path = new TNM_HyperPath();
			PTOrg* org= dest->orgVector[j];
			//cout<<dest->destination->id<<" : "<<org->org->id<<endl;
			if (path->InitializeHP(org->org,dest->destination))
			{
				org->state=true;
				//path->print();
				//org->pathSet.push_back(path);
				double dmd = org->assDemand;//total demand.
				netTTwaitcost += path->WaitCost * dmd;

				//cout<<"od:"<<org->org->id<<"->"<<dest->destination->id<<",waitcost:"<<path->WaitCost<<",dmd:"<<dmd<<endl;
				vector<GLINK*> glinks=path->GetGlinks();
				//floatType temp = 0;
			
				for(int i=0;i<glinks.size();++i)
				{
					glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
					//cout<<glinks[i]->m_data<<endl;
					
				}
				//cout<<"W: "<<temp<<endl;
				//max_w += temp;
			}
			else
			{
				//cout<<org->org->GetStopPtr()->m_id<<"->"<<dest->destination->GetStopPtr()->m_id<<" :"<<org->org->StgElem->cost<<endl;
				//cout<<"OD-pair <"<<org->org->GetStopPtr()->m_id<<","<<dest->destination->GetStopPtr()->m_id<<"> can not initialize hyperpath tree"<<endl;
				//return 1;
				org->state=false;
			}

			/*cout<<dest->destination->id<<" : "<<org->org->id<<endl;
			path->UpdateWaitcost();
			path->UpdateHyperpathCost();
			path->print();*/
			/*cout<<dest->destination->id<<"->"<<org->org->id<<" state: "<<org->state<<" cost : "<<org->org->StgElem->cost <<endl;*/
		}
	}
	
	return 0;
}



//int PTNET::PTAllOrNothing(PTDestination* dest)
//{
//	PTOrg* org;
//	InitializeHyperpathLS(dest->destination);//求最短路,vialink
//	//cout<<"dest id :"<<dest->destination->GetStopPtr()->m_id<<endl;
//	for (int j = 0; j<dest->numOfOrg; j++)
//	{
//		TNM_HyperPath* path = new TNM_HyperPath();
//		PTOrg* org= dest->orgVector[j];
//		//cout<<org->org->GetStopPtr()->m_id<<":"<<org->org->StgElem->cost<<endl;
//		if (path->InitializeHP(org->org,dest->destination))
//		{
//			double dmd = org->assDemand;//total demand.
//			netTTwaitcost += path->WaitCost * dmd;
//			//cout<<"od:"<<org->org->id<<"->"<<dest->destination->id<<",waitcost:"<<path->WaitCost<<",dmd:"<<dmd<<endl;
//			vector<GLINK*> glinks=path->GetGlinks();
//			
//			for(int i=0;i<glinks.size();++i)
//			{
//				glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
//			}
//		}
//		else
//		{
//			cout<<"OD-pair <"<<org->org->GetStopPtr()->m_id<<","<<dest->destination->GetStopPtr()->m_id<<"> can not initialize hyperpath tree"<<endl;
//			return 1;
//		}
//	}
//	return 0;
//}

//int PTNET::PTAllOrNothing_TP()
//{
//	netTTwaitcost = 0.0;
//	PTLink *link;
//	//清空volume
//	for(int i = 0;i<numOfLink;i++)
//	{
//		link = linkVector[i];
//		link->volume = 0.0;
//	}
//
//	for (int i = 0 ;i<numOfPTDest;i++) //for each destination;
//	{
//		//cout<<"dest:"<<PTDestVector[i]->destination->id<<endl;
//
//		if (PTAllOrNothing_TP(PTDestVector[i])!=0) return 1; //if not correctly, return 1;
//	
//	}
//	//cout<<"cost:"<<nodeVector[0]->StgElem->cost<<endl;
//	//cout<<"                all or nothing over!              "<<endl;
//	return 0;
//}

int PTNET::PTAllOrNothing()
{
	netTTwaitcost = 0.0;
	PTLink *link;
	for (int i = 0 ;i<numOfPTDest;i++) //for each destination;
	{
		//cout<<"dest:"<<PTDestVector[i]->destination->id<<endl;
		if (PTAllOrNothing(PTDestVector[i])!=0) return 1; //if not correctly, return 1;
	
	}
	//cout<<"                      over                        "<<endl;
	return 0;
}


void	PTNET::FlowReAssignment()
{
	PTLink *link;
	for(int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		link->volume = 0.0;
	}
	for (int i = 0 ;i<numOfPTDest;i++)
	{
		PTDestination* dest= PTDestVector[i];
		for (int j=0; j<dest->numOfOrg; j++)
		{
			PTOrg* org= dest->orgVector[j];
			for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
			{
				TNM_HyperPath* path = *it;
				vector<GLINK*> glinks=path->GetGlinks();		
				for(int i=0;i<glinks.size();++i)
				{
					glinks[i]->m_linkPtr->volume += glinks[i]->m_data * path->flow;
				}
			}
		
		}
	
	}
	UpatePTNetworkLinkCost();
}


bool	PTNET::InitialHyperpathNetFlow()
{
	PTDestination* dest;

	for (int i = 0;i<numOfPTDest; i++)
	{
		dest = PTDestVector[i];

		InitializeHyperpathLS(dest->destination);//shortest path

		{
			for (int j=0; j<dest->numOfOrg; j++)
			{		
				TNM_HyperPath* path = new TNM_HyperPath();
				PTOrg* org= dest->orgVector[j];

				//cout<<dest->destination->id<<" , "<<org->org->id<<endl;
				//cout<<"---------------------------------"<<endl;

				if (path->InitializeHP(org->org,dest->destination))
				{
					org->state=true;
					double dmd = org->assDemand;
					vector<GLINK*> glinks=path->GetGlinks();		
					for(int i=0;i<glinks.size();i++)
					{
						glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
	/*					glinks[i]->m_linkPtr->UpdatePTLinkCost();
						glinks[i]->m_linkPtr->UpdatePTDerLinkCost();*/

						/*不考虑拥挤效应*/
						glinks[i]->m_linkPtr->UpdatePTLinkCost_const();
						glinks[i]->m_linkPtr->UpdatePTDerLinkCost_const();


						if (glinks[i]->m_linkPtr->rLink)
						{
		/*					glinks[i]->m_linkPtr->rLink->UpdatePTLinkCost();
							glinks[i]->m_linkPtr->rLink->UpdatePTDerLinkCost();		*/	

							/*不考虑拥挤效应*/
							glinks[i]->m_linkPtr->UpdatePTLinkCost_const();
							glinks[i]->m_linkPtr->UpdatePTDerLinkCost_const();
						}
					}
					path->flow = dmd;
					org->pathSet.push_back(path);
				}
				else
				{
					org->state=false;
				}
			}
		}

	}
	//cout<<"over"<<endl;
	return true;
}

bool	PTNET::InitialHyperpathNetFlow_TP()
{
	PTDestination* dest;

	for (int i = 0;i<numOfPTDest; i++)
	{
		dest = PTDestVector[i];
		//cout<<dest->destination->id<<endl;
		
		InitializeHyperpathLS_TP2(dest->destination);
		//InitializeHyperpathLS(dest->destination);//shortest path
		
		for (int j = 0; j<dest->numOfOrg; j++)
		{
			TNM_HyperPathTP* path = new TNM_HyperPathTP();
			PTOrg* org= dest->orgVector[j];
			if (path->InitializeHP_TP(org->org,dest->destination,transfer_times,ttwalktime))
			{
				org->state=true;
				double dmd = org->assDemand;//total demand.
				//netTTwaitcost += path->WaitCost * dmd;
				//cout<<"od:"<<org->org->id<<"->"<<dest->destination->id<<",waitcost:"<<path->WaitCost<<",dmd:"<<dmd<<endl;
				vector<GLINK*> glinks=path->GetGlinks();
			
				for(int i=0;i<glinks.size();i++)
				{
					glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
					glinks[i]->m_linkPtr->UpdatePTLinkCost();
					glinks[i]->m_linkPtr->UpdatePTDerLinkCost();
					if (glinks[i]->m_linkPtr->rLink)
					{
						glinks[i]->m_linkPtr->rLink->UpdatePTLinkCost();
						glinks[i]->m_linkPtr->rLink->UpdatePTDerLinkCost();			
					}
				}
				path->flow = dmd;
				org->pathSet.push_back(path);
			}
			else
			{
				//cout<<org->org->GetStopPtr()->m_id<<"->"<<dest->destination->GetStopPtr()->m_id<<" :"<<org->org->StgElem->cost<<endl;
				//cout<<"OD-pair <"<<org->org->GetStopPtr()->m_id<<","<<dest->destination->GetStopPtr()->m_id<<"> can not initialize hyperpath tree"<<endl;
				//return 1;
				org->state=false;
			}
			//cout<<dest->destination->id<<"->"<<org->org->id<<" state: "<<org->state<<endl;
		}
	}
	cout<<"over"<<endl;
	return true;
}


void PTNET::ColumnGeneration(PTDestination* dest,PTOrg* org)
{
	TNM_HyperPath* cpath = new TNM_HyperPath();		
	if (cpath->InitializeHP(org->org, org->dest))//寻找当前OD对间的最短路
	{
		bool pin = false;

		//检查是否存在相同的path
		for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it != org->pathSet.end(); it++)
		{
			if ((*it)->name == cpath->name)
			{
				pin = true;
				//cout<<"OD:"<<org->org->id<<"-"<<org->dest->id<<" same path!"<<endl<<endl;
				break;
			}
		}
		
		//如果不存在相同路径，添加最短路（列生成法）
		if (!pin) 
		{
			org->pathSet.push_back(cpath);
			cpath->flow = 0.0;
			cpath->Preflow = 0.0;
			//cout<<"OD:"<<org->org->id<<"-"<<org->dest->id<<" don't have the same path!"<<endl<<endl;
			//cpath->s3 = 1;
			//cout<<"ok"<<endl;
			//cpath->printv2();
		}
	}
	//cpath->print();
}


int	PTNET::SolvePathTEAP_AON()
{
	clock_t startTime = clock();
	UpatePTNetworkLinkCost();
	InitialHyperpathNetFlow();

	for (int i = 0; i < numOfPTDest; i++)
	{
		PTDestination* dest = PTDestVector[i];
		for (int j = 0; j < dest->numOfOrg; j++)
		{
			PTOrg* org = dest->orgVector[j];
			org->UpdatePathSetCost();
		}
	}
	double cpuTimeSeconds =
		1.0 * (clock() - startTime) / CLOCKS_PER_SEC;
	lastAonCpuTimeSeconds = cpuTimeSeconds;
	if (writeCsvResults)
	{
		ReportAONCsvResults(cpuTimeSeconds);
	}

	return 0;
}


int	PTNET::SolvePathTEAP()
{
	//cout<<"yessss"<<endl;

	m_startRunTime = clock();
	AllocateLinkBuffer(2);// store prob for longest and shortest hyperpath pass the link （GP转移中会使用）  //  2
	UpatePTNetworkLinkCost();
	InitialHyperpathNetFlow();
	//cout<<"++++++++++++++++++++++++++++++"<<endl;
	UpatePTNetworkLinkCost();

	for (int i = 0; i < numOfPTDest; i++)
	{
		PTDestination* dest = PTDestVector[i];
		for (int j=0; j < dest->numOfOrg; j++)
		{
			PTOrg* org = dest->orgVector[j];
			org->UpdatePathSetCost();
		}
	}


	//cout<<"1"<<endl;
	//cout<<"============================"<<endl;
	if(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))))
	{
		cout<<"okkkkk"<<endl;
		do
		{
			clock_t sclock = clock();
			PTDestination* dest;

			for (int i = 0;i<numOfPTDest; i++)
			{
				dest = PTDestVector[i];
				if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP )
				{
					if (!InitializeHyperpathLS(dest->destination))
					{
						for (int j=0; j<dest->numOfOrg; j++)
						{													
							PTOrg* org= dest->orgVector[j];
							/*for(int k = 0;k<org->pathSet.size();k++)
							{
								cout<<org->pathSet[k]->cost<<endl;
							}
							cout<<"-------------------------------------"<<endl;*/
							if(org->state)
							{

								//cout<<"2"<<endl;
								//cout<<"============================"<<endl;


								ColumnGeneration(dest,org) ;//在OD对间添加最短超路径
								//cout<<dest->destination->id<<" , "<<org->org->id<<endl;
								//cout<<org->org->StgElem->cost<<endl;
								
								org->UpdatePathSetCost();

								/*cout<<dest->destination->id<<" , "<<org->org->id<<endl;
								for(int k = 0;k<org->pathSet.size();k++)
								{
									cout<<"第"<<k<<"条："<<org->pathSet[k]->cost<<endl;
								}
								cout<<"++++++++++++++++++++++++++++++++++++++"<<endl;*/

								if (org->pathSet.size()>1)
								{
									if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy )	UpdateHyperPathGreedyFlow(dest,org);	
									else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP )	UpdateHyperpathGPFlow_original(dest,org);
									else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP)	UpdateHyperpathGPFlowTP(dest,org);
									else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP)	UpdateHyperPathGreedyFlowTP(dest,org);	
								}
								/*if(dest->destination->id==6 && org->org->id==20)
									{
										cout<<endl;
									}*/
							}
						}

					}
					else return 1;
				}
				else
				{
					
					if (!InitializeHyperpathLS_TP2(dest->destination))//InitializeHyperpathLS_TP2(dest->destination)
					{
						for (int j=0; j<dest->numOfOrg; j++)
						{	
							
							PTOrg* org= dest->orgVector[j];
							/*if(dest->destination->id==6 && org->org->id==20)
							{
								cout<<endl;
							}*/
							//cout<<org->org->id<<"->"<<dest->destination->id<<endl;
							/*if(org->org->StgElem->cost<10000000000)
							{org->state=true;}
							else
							{org->state=false;}*/
							/*TNM_HyperPathTP* path=new TNM_HyperPathTP();
							if(path->InitializeHP_TP(org->org,dest->destination,transfer_times,ttwalktime))
							{org->state=true;}
							else
							{org->state=false;}*/
							if (org->state)
							{
								
								ColumnGeneration(dest,org) ;//
								//org->UpdatePathSetCostTP();//
								org->UpdatePathSetCost();//
								//cout<<org->pathSetTP.size()<<endl;
								//if(org->state)
								//{
								//org->UpdatePathSetCostTP();//
								if (org->pathSet.size()>1)
								{
									if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy )	UpdateHyperPathGreedyFlow(dest,org);	
									else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP )	UpdateHyperpathGPFlow(dest,org);
									else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP)	UpdateHyperpathGPFlow(dest,org);//??????
									else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP)	UpdateHyperPathGreedyFlow(dest,org);
									//else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP)	UpdateHyperpathLineSearchFlow(dest,org);//?????
								}
							/*if(dest->destination->id==6 && org->org->id==20)
							{
								cout<<endl;
							}*/
								//}
							}
							
						}
						
					}
					else return 1;
				}
				
			}

			IterMainlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;	

			/*for (int i = 0;i<numOfPTDest; i++)
			{
				dest = PTDestVector[i];
				for (int j=0; j<dest->numOfOrg; j++)
				{													
					PTOrg* org= dest->orgVector[j];
					cout<<dest->destination->id<<" : "<<org->org->id<<endl;
					for (int k = 0;k<org->pathSet.size();k++)
					{
						org->pathSet[k]->print();
					}
					cout<<"---------------------------------------------------"<<endl;
				}
			}*/

			//cout<<"2"<<endl;
			if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy|| PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP ) 
				HyperpathInnerLoop(500);//??????????
			else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP)
				HyperpathInnerLoop(500);

			/*for (int i = 0;i<numOfPTDest; i++)
			{
				PTDestination* dest=PTDestVector[i];
				for(int j=0;j<dest->numOfOrg;j++)
				{
					PTOrg* org=dest->orgVector[j];
					if(dest->destination->id==6&&org->org->id==20)
					{
						cout<<endl;
					}
				}
			}*/
			//FlowReAssignment();
			//cout<<"3"<<endl;
			//cout<<"++++++++++++++++++++++++++++++++"<<endl;
			ComputeConvGap();

			/*for (int i = 0;i<numOfPTDest; i++)
			{
				PTDestination* dest=PTDestVector[i];
				for(int j=0;j<dest->numOfOrg;j++)
				{
					PTOrg* org=dest->orgVector[j];
					if(dest->destination->id==6&&org->org->id==20)
					{
						cout<<endl;
					}
				}
			}
*/
			curIter ++;	
			//===================================
		/*	if(curIter==2)
			{
				for (int i = 0;i<numOfPTDest; i++)
				{
					PTDestination* dest=PTDestVector[i];
					for(int j=0;j<dest->numOfOrg;j++)
					{
						PTOrg* org=dest->orgVector[j];
						cout<<org->org->id<<"->"<<dest->destination->id<<" : "<<org->pathSet.size()<<endl;
						for(int k=0;k<org->pathSet.size();k++)
						{
							TNM_HyperPath* path=org->pathSet[k];
							path->print();
						}
					}
				}
			}*/
			
			//===================================
			RecordTEAPCurrentIter();

			cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<", hyperpath num:"<<numaOfHyperpath<<",inneriters:"<<InnerIters<<endl<<endl;
		}while (!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))));
	}
	cout<<"CPU TIME: "<<1.0*(clock() - m_startRunTime)<<endl;


	//for(int i=0;i<numOfLink;i++)
	//{
	//	PTLink* link= linkVector[i];
	//	//if (link->GetTransitLinkType()==PTLink::ENROUTE)
	//	//{
	//	//	cout<<"linkid:"<<link->id<<",tail:"<<link->tail->id<<",head:"<<link->head->id<<endl;
	//	//}

	//	cout<<"linkid:"<<link->id<<",flow:"<<link->volume<<",cost:"<<link->cost<<endl;
	//}
	//cout<<"netwait:"<<netTTwaitcost<<endl;
	ReportPTHyperpaths();

	ReportPTlinkflow();
	return 0;
}

//




void PTNET::UpdateHyperPathGreedyFlow(PTDestination* dest,PTOrg* org)
{

	double beta = 1 ;
	multimap<double, TNM_HyperPath*,less<double>> Orderpaths;
	for(int i = 0;i<org->pathSet.size();i++)
	{				
		TNM_HyperPath* path = org->pathSet[i];     
		path->Preflow = path->flow;
		//path->s1 = 1/ beta; //g_k choose from identity matrix
		path->s1 = path->fdcost == 0 ? 1e-10:path->fdcost/ beta; //g_k choose from identity matrix//path->fdcost==g_k//s1==g_k/miu   //g_k
		path->s2 = path->cost - path->flow * path->s1;//s_k
		Orderpaths.insert(pair<double, TNM_HyperPath*>(path->s2, path));//key is the current label value of  hyperpath  'avg cost'
	}

	multimap<double, TNM_HyperPath*,less<double>>::iterator pv= Orderpaths.begin();     
	vector<TNM_HyperPath*>  AttractivePathSet;
	double B = pv->second->s2 / pv->second->s1;
	double C = 1.0 / pv->second->s1; 
	double w = (org->assDemand + B)/C;//u_w
	AttractivePathSet.push_back(pv->second);
	pv++;
	while(pv!=Orderpaths.end() && pv->second->s2 < w)
	{
		B += pv->second->s2 / pv->second->s1;
		C += 1.0 / pv->second->s1; 
		w = (org->assDemand + B)/C;
		AttractivePathSet.push_back(pv->second);
		pv++;
	}
	
	for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
				
		vector<TNM_HyperPath*>::iterator fit = find(AttractivePathSet.begin(),AttractivePathSet.end(),path);

		if (fit!=AttractivePathSet.end()) 
		{
			path->flow = (w - path->s2 ) / path->s1;						
		}
		else
		{
			path->flow = 0.0;
		}
		double dflow = path->flow - path->Preflow;//获得path中的流量后，更新path中的link流量
		vector<GLINK*> glinks=path->GetGlinks();
		for(int i=0;i<glinks.size();++i)
		{
			PTLink* plink= glinks[i]->m_linkPtr;
			double lprob= glinks[i]->m_data;
			plink->volume += lprob * dflow;
			if (abs(plink->volume) < 1e-8)
			{
				plink->volume = 0.0;
			}
			plink->UpdatePTLinkCost();
			plink->UpdatePTDerLinkCost();
			if (plink->rLink)
			{
				plink->rLink->UpdatePTLinkCost();
				plink->rLink->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
			}
		}			
	}


	org->PathFlowConservation();


}

void PTNET::UpdateHyperPathGreedyFlowTP(PTDestination* dest,PTOrg* org)
{

	double beta = 1 ;
	multimap<double, TNM_HyperPathTP*,less<double>> Orderpaths;
	for(int i = 0;i<org->pathSetTP.size();i++)
	{				
		TNM_HyperPathTP* path = org->pathSetTP[i];     
		path->Preflow = path->flow;
		//path->s1 = 1/ beta; //g_k choose from identity matrix
		path->s1 = path->fdcost == 0 ? 1e-10:path->fdcost/ beta; //g_k choose from identity matrix//path->fdcost==g_k//s1==g_k/miu   //g_k
		path->s2 = path->cost - path->flow * path->s1;//s_k
		Orderpaths.insert(pair<double, TNM_HyperPathTP*>(path->s2, path));//key is the current label value of  hyperpath  'avg cost'
	}
	multimap<double, TNM_HyperPathTP*,less<double>>::iterator pv= Orderpaths.begin();     
	vector<TNM_HyperPathTP*>  AttractivePathSet;
	double B = pv->second->s2 / pv->second->s1;
	double C = 1.0 / pv->second->s1; 
	double w = (org->assDemand + B)/C;//u_w
	AttractivePathSet.push_back(pv->second);
	pv++;
	while(pv!=Orderpaths.end() && pv->second->s2 < w)
	{
		B += pv->second->s2 / pv->second->s1;
		C += 1.0 / pv->second->s1; 
		w = (org->assDemand + B)/C;
		AttractivePathSet.push_back(pv->second);
		pv++;
	}
	
	for (vector<TNM_HyperPathTP*>::iterator it = org->pathSetTP.begin(); it!=org->pathSetTP.end();it++) 
	{
		TNM_HyperPathTP* path = *it;
				
		vector<TNM_HyperPathTP*>::iterator fit = find(AttractivePathSet.begin(),AttractivePathSet.end(),path);

		if (fit!=AttractivePathSet.end()) 
		{
			path->flow = (w - path->s2 ) / path->s1;						
		}
		else
		{
			path->flow = 0.0;
		}
		double dflow = path->flow - path->Preflow;
		vector<GLINK*> glinks=path->GetGlinks();
		for(int i=0;i<glinks.size();++i)
		{
			PTLink* plink= glinks[i]->m_linkPtr;
			double lprob= glinks[i]->m_data;//
			plink ->volume += lprob*dflow;
			if (abs(plink->volume) < 1e-8)
			{
				plink->volume = 0.0;
			}
			plink ->UpdatePTLinkCost();
			plink ->UpdatePTDerLinkCost();
			if (plink ->rLink)
			{
				plink ->rLink ->UpdatePTLinkCost();
				plink ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
			}
		}			
	}


	org->PathFlowConservation();


}

void PTNET::UpdateHyperpathGPFlowline(PTDestination* dest,PTOrg* org)
{
	TNM_HyperPath* MinCostpath=org->pathSet[org->minIx];
	vector<GLINK*> mglinks=MinCostpath->GetGlinks();
	PTLink *link,*rlink;
	for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
		path->Preflow = path->flow;
		MinCostpath->Preflow = MinCostpath->flow;
		if (path != MinCostpath)
		{
			path->UpdateHyperpathCost();
			MinCostpath->UpdateHyperpathCost();
			vector<GLINK*> glinks= path->GetGlinks();
			int iter = 0; 
			int maxLineSearchIter = 100;
			floatType lineSearchAccuracy = 1e-12;
			floatType dev = path->cost - MinCostpath->cost;

			floatType ldflow = 0.0,rdflow = 0.0;
			if (dev>0) 
			{
				rdflow =  path->flow;
			}
			else
			{
				ldflow =  - MinCostpath->flow;	
			}
			floatType dflow=0.0 ,lastdflow = 0.0,shiftflow;

			while(iter < maxLineSearchIter && abs(dev)>=lineSearchAccuracy &&
				((dev>0 && path->flow > 0) ||(dev<0 && MinCostpath->flow > 0)))
			{
				iter = iter + 1;
				dflow = (ldflow + rdflow)/2.0 ;

				MinCostpath->flow = MinCostpath->flow + dflow -lastdflow; 	
				path->flow = path->flow - dflow + lastdflow; 			
				if((abs(path->flow)> 1e-10 && path->flow <0)||(abs(MinCostpath->flow)> 1e-10 && MinCostpath->flow<0))
				{
					cout<<path->flow<<","<<MinCostpath->flow<<endl;
					system("PAUSE");
				}
				bool resetdflow = false;

				for(int lj=0;lj<glinks.size();lj++)
				{
					link = glinks[lj]->m_linkPtr;
					link ->volume += glinks[lj]->m_data* (path->flow - path->Preflow);
					if (link->volume<0)
					{
						//cout<<link->volume<<",preflow:"<<path->Preflow<<",flow:"<<path->flow<<",dflow:"<<dflow<<endl;
					}

					if (abs(link->volume) < 1e-8) link ->volume = 0.0;
					link ->UpdatePTLinkCost();
					link ->UpdatePTDerLinkCost();
				
					if (link ->rLink)
					{
						link ->rLink ->UpdatePTLinkCost();
						link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
					
					}
						
					link->buffer[1]=0.0;// reset the link prob
				}

				//更新弧k-流量，费用及一阶导数
				for(int ii=0;ii<mglinks.size();++ii)
				{
					link = mglinks[ii]->m_linkPtr;
					link ->volume += mglinks[ii]->m_data * (MinCostpath->flow - MinCostpath->Preflow);
					if (link->volume<0)
					{
						//cout<<link->volume<<",preflow:"<<MinCostpath->Preflow<<",flow:"<<MinCostpath->flow<<",dflow:"<<dflow<<endl;
					}
					if (abs(link->volume) < 1e-8) link ->volume = 0.0;
					link->buffer[0]=0.0;// reset the link prob
					link->markStatus =0;
					link ->UpdatePTLinkCost();
					link ->UpdatePTDerLinkCost();
				
					if (link ->rLink)
					{
						link ->rLink ->UpdatePTLinkCost();
						link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
					
					}

				}

				path->UpdateHyperpathCost();
				MinCostpath->UpdateHyperpathCost();
				dev = path->cost - MinCostpath->cost;
				lastdflow = dflow;
				if(dev>0 && !resetdflow)
				{
					ldflow = dflow;
				}
				else  
				{
					rdflow = dflow;
				}

			}
		}
	}
	org->PathFlowConservation();


}

void PTNET::UpdateHyperpathGPFlow(PTDestination* dest,PTOrg* org)
{
	//cout<<dest->destination->id<<" , "<<org->org->id<<endl;
	TNM_HyperPath* MinCostpath = org->pathSet[org->minIx];
	vector<GLINK*> mglinks = MinCostpath->GetGlinks();
	PTLink *link,*rlink;
	for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
		path->Preflow = path->flow;
		MinCostpath->Preflow = MinCostpath->flow;
		if (path != MinCostpath)
		{
			path->UpdateHyperpathCost();
			MinCostpath->UpdateHyperpathCost();
			//cout<<"path cost : "<<path->cost<<endl;
			//cout<<"minpath cost : "<<MinCostpath->cost<<endl;

			vector<GLINK*> glinks = path->GetGlinks();
			floatType DerSum = 0.0;	//g_k
			//shift flow from path -> minpath
			for(int i = 0; i < mglinks.size(); i++)
			{
				link = mglinks[i]->m_linkPtr;
				link->buffer[0] = mglinks[i]->m_data;//buffer[0]：存储的是最短路上的该link的使用概率
				link->markStatus = 1;// tag 1 for arcs on the shortest path
			}
			for(int i = 0; i < glinks.size(); i++)//
			{
				glinks[i]->m_linkPtr->buffer[1] = glinks[i]->m_data;//buffer[0]：存储的是非最短路上的该link的使用概率
			}
			
			// calculate derivative for shortest links
			for(int i=0;i<mglinks.size();i++)
			{
				link = mglinks[i]->m_linkPtr;
				double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
				//如果存在rlink，继续添加tmp（参考论文公式(3-33)）
				if (link->rLink)
				{
					tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
					//cout<<"rpfdcost="<<link->rpfdcost<<","<<link->rLink->buffer[0]<<","<<link->rLink->buffer[1]<<endl;
				}
				DerSum += tmp *(link->buffer[1] - link->buffer[0]);				
			}

			// calculate derivative for non-shortest links
			for(int i=0;i<glinks.size();i++)
			{
				link = glinks[i]->m_linkPtr;
				if (link->markStatus != 1)
				{
					double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
					if (link->rLink)
					{
						tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
						//cout<<"rpfdcost="<<link->rpfdcost<<","<<link->rLink->buffer[0]<<","<<link->rLink->buffer[1]<<endl;
					}
					DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
				}
			}

			//加上waitcost的导数
			if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
			{
				//cout<<"dersum:"<<DerSum<<",path: "<<path->ComputePdfWaitcost()<<",minpath: "<<MinCostpath->ComputePdfWaitcost()<<endl;
				//cout<<"==================================="<<endl;
				DerSum += (path->ComputePfdWaitcost() - MinCostpath->ComputePfdWaitcost());
			}
			


			//更新dflow，dflow==e
			if (DerSum ==0)	DerSum = 1e-8;//no need to shit flow from current path to the shortest path 
			//cout<<"dersum : "<<DerSum<<endl;
			//cout<<"-------------------------------------"<<endl;
			double dev = path->cost - MinCostpath->cost;
			double dflow;
			if (dev>=0) 
			{
				dflow =  __min(1.0 * dev /DerSum, path->flow);
				if(dflow < 0)
				{
					system("pause");
				}
				//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
			}
			else
			{
				dflow =  __max(1.0 * dev /DerSum, -MinCostpath->flow);
				if(dflow > 0)
				{
					system("pause");
				}
				//cout<<1.0 * dev /DerSum<<","<< MinCostpath->flow<<",dflow"<<dflow<<endl;
			}
			//cout<<"org:"<<org->org->id<<",dest:"<<dest->destination->id<<",dflow:"<<dflow<<",DerSum:"<<DerSum<<",pathcost:"<<path->cost<<",minpathcost:"<<MinCostpath->cost<<",pathwaitcost:"<<path->WaitCost<<",minpathwaitcost:"<<MinCostpath->WaitCost;
			//cout<<",past path flow: "<<path->flow<<",past minpath flow:"<<MinCostpath->flow<<endl;
			//cout<<",path waitcost:"<<path->WaitCost<<",minpath waitcost:"<<MinCostpath->WaitCost<<endl;
			
			//更新path流量
			MinCostpath->flow = MinCostpath->flow + dflow; 	
			path->flow = path->flow - dflow; 	
			//cout<<",now path flow: "<<path->flow<<",now minpath flow:"<<MinCostpath->flow<<endl;
			if (path->flow<0||MinCostpath->flow<0)
			{
				cout<<"path"<<endl;
				path->ComputePfdWaitcostForShow();
				cout<<"minpath"<<endl;
				MinCostpath->ComputePfdWaitcostForShow();


				cout<<"wrong!!! "<<path->flow<<","<<MinCostpath->flow<<","<<dflow<<","<<DerSum<<","<<path->ComputePfdWaitcost()<<","<<MinCostpath->ComputePfdWaitcost()<<endl;
				system("PAUSE");
			}

			if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
			{
				path->loadingflow();//如果是有效发车频率模型，则需要考虑站点流量的加载
				MinCostpath->loadingflow();

				for(int lj=0;lj<glinks.size();lj++)
				{
					link = glinks[lj]->m_linkPtr;
					link->buffer[1]=0.0;
				}
				for(int ii=0;ii<mglinks.size();++ii)
				{
					link = mglinks[ii]->m_linkPtr;;
					link->buffer[0]=0.0;
					link->markStatus =0;
				}
			}
			else
			{
				//更新弧k流量，费用，一阶导数（非参考路径上的弧）
				for(int lj=0;lj<glinks.size();lj++)
				{
					link = glinks[lj]->m_linkPtr;
					link ->volume += glinks[lj]->m_data* (path->flow - path->Preflow);
					if (link->volume<0)
					{
						//cout<<link->volume<<",preflow:"<<path->Preflow<<",flow:"<<path->flow<<",dflow:"<<dflow<<endl;
					}

					if (abs(link->volume) < 1e-8) link ->volume = 0.0;
					link ->UpdatePTLinkCost();
					link ->UpdatePTDerLinkCost();
					if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
					{link ->UpdateEffectiveFreq();}
				
					if (link ->rLink)
					{
						link ->rLink ->UpdatePTLinkCost();
						link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
						if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
						{link ->rLink ->UpdateEffectiveFreq();}
					
					}
						
					link->buffer[1] = 0.0;// reset the link prob
				}

				//更新弧k-流量，费用及一阶导数（参考路径上的弧）
				for(int ii=0;ii<mglinks.size();++ii)
				{
					link = mglinks[ii]->m_linkPtr;
					link ->volume += mglinks[ii]->m_data * (MinCostpath->flow - MinCostpath->Preflow);
					if (link->volume<0)
					{
						//cout<<link->volume<<",preflow:"<<MinCostpath->Preflow<<",flow:"<<MinCostpath->flow<<",dflow:"<<dflow<<endl;
					}
					if (abs(link->volume) < 1e-8) link ->volume = 0.0;
					link->buffer[0] = 0.0;// reset the link prob
					link->markStatus =0;
					link ->UpdatePTLinkCost();
					link ->UpdatePTDerLinkCost();
					if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
					{link ->UpdateEffectiveFreq();}
				
					if (link ->rLink)
					{
						link ->rLink ->UpdatePTLinkCost();
						link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
						if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
						{link ->rLink ->UpdateEffectiveFreq();}
					
					}

				}
			}
			
			//cout<<""<<endl;
		}
	}
	if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)
	{org->PathFlowConservation_eff();}
	else
	{org->PathFlowConservation();}
	
}


void PTNET::UpdateHyperpathGPFlow_original(PTDestination* dest,PTOrg* org)
{
	TNM_HyperPath* MinCostpath=org->pathSet[org->minIx];
	vector<GLINK*> mglinks=MinCostpath->GetGlinks();
	PTLink *link,*rlink;

	for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
		path->Preflow = path->flow;
		MinCostpath->Preflow = MinCostpath->flow;
		if (path != MinCostpath)
		{
			path->UpdateHyperpathCost();
			MinCostpath->UpdateHyperpathCost();
			vector<GLINK*> glinks= path->GetGlinks();
			floatType DerSum = 0.0;	
			//shift flow from path -> minpath
			for(int i=0;i<mglinks.size();i++)
			{
				link = mglinks[i]->m_linkPtr;
				link->buffer[0] = mglinks[i]->m_data;
				link->markStatus = 1;// tag 1 for arcs on the shortest path
			}
			for(int i=0;i<glinks.size();i++)
			{
				glinks[i]->m_linkPtr->buffer[1] = glinks[i]->m_data;
			}
				
			for(int i=0;i<mglinks.size();i++)
			{
				link = mglinks[i]->m_linkPtr;
				double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
				if (link->rLink)
				{
					tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
					//cout<<"rpfdcost="<<link->rpfdcost<<","<<link->rLink->buffer[0]<<","<<link->rLink->buffer[1]<<endl;
				}
				DerSum += tmp *(link->buffer[1] - link->buffer[0]);				
			}
			// calculate derivative for non-shortest links
			for(int i=0;i<glinks.size();i++)
			{
				link = glinks[i]->m_linkPtr;
				if (link->markStatus != 1)
				{
					double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
					if (link->rLink)
					{
						tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
						//cout<<"rpfdcost="<<link->rpfdcost<<","<<link->rLink->buffer[0]<<","<<link->rLink->buffer[1]<<endl;
					}
					DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
				
				}
			}


			if (DerSum ==0)	DerSum = 1e-8;//no need to shit flow from current path to the shortest path 
			double dev = path->cost - MinCostpath->cost;
			double dflow;
			if (dev>=0) 
			{
				dflow =  __min(1.0 * dev /DerSum, path->flow);
				//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
			}
			else
			{
				dflow =  __max(1.0 * dev /DerSum, -MinCostpath->flow);	
				//cout<<1.0 * dev /DerSum<<","<< MinCostpath->flow<<",dflow"<<dflow<<endl;
			}
			//cout<<"org:"<<org->org->id<<",dest:"<<dest->destination->id<<",dflow:"<<dflow<<",DerSum:"<<DerSum<<",pathcost:"<<path->cost<<",minpathcost:"<<MinCostpath->cost<<endl;
				
			MinCostpath->flow = MinCostpath->flow + dflow; 	
			path->flow = path->flow - dflow; 			
			if (path->flow<0||MinCostpath->flow<0)
			{
				cout<<path->flow<<","<<MinCostpath->flow<<endl;
				system("PAUSE");
			}
			for(int lj=0;lj<glinks.size();lj++)
			{
				link = glinks[lj]->m_linkPtr;
				link ->volume += glinks[lj]->m_data* (path->flow - path->Preflow);
				if (link->volume<0)
				{
					//cout<<link->volume<<",preflow:"<<path->Preflow<<",flow:"<<path->flow<<",dflow:"<<dflow<<endl;
				}

				if (abs(link->volume) < 1e-8) link ->volume = 0.0;
				
				//link->UpdatePTLinkCost();
				//link->UpdatePTDerLinkCost();
				/*不考虑拥挤*/
				link->UpdatePTLinkCost_const();
				link->UpdatePTDerLinkCost_const();

				
				if (link ->rLink)
				{
					
					//link ->rLink ->UpdatePTLinkCost();
					//link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
					/*不考虑拥挤*/
					link->UpdatePTLinkCost_const();
					link->UpdatePTDerLinkCost_const();
				}
						
				link->buffer[1] = 0.0;// reset the link prob
			}

			for(int ii=0;ii<mglinks.size();++ii)
			{
				link = mglinks[ii]->m_linkPtr;
				link ->volume += mglinks[ii]->m_data * (MinCostpath->flow - MinCostpath->Preflow);
				if (link->volume<0)
				{
					//cout<<link->volume<<",preflow:"<<MinCostpath->Preflow<<",flow:"<<MinCostpath->flow<<",dflow:"<<dflow<<endl;
				}
				if (abs(link->volume) < 1e-8) link ->volume = 0.0;
				link->buffer[0]=0.0;// reset the link prob
				link->markStatus =0;
				//link ->UpdatePTLinkCost();
				//link ->UpdatePTDerLinkCost();
				/*不考虑拥挤*/
				link->UpdatePTLinkCost_const();
				link->UpdatePTDerLinkCost_const();

				if (link ->rLink)
				{
					//link ->rLink ->UpdatePTLinkCost();
					//link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
					/*不考虑拥挤*/
					link->UpdatePTLinkCost_const();
					link->UpdatePTDerLinkCost_const();

				}
			}
		}
	}
	org->PathFlowConservation();
}




void PTNET::UpdateHyperpathGPFlowTP(PTDestination* dest,PTOrg* org)
{
	TNM_HyperPathTP* MinCostpath=org->pathSetTP[org->minIx];
	vector<GLINK*> mglinks=MinCostpath->GetGlinks();
	PTLink *link,*rlink;
	for (vector<TNM_HyperPathTP*>::iterator it = org->pathSetTP.begin(); it!=org->pathSetTP.end();it++) 
	{
		TNM_HyperPathTP* path = *it;
		path->Preflow = path->flow;
		MinCostpath->Preflow = MinCostpath->flow;
		if (path != MinCostpath)
		{
			path->UpdateHyperpathCost();
			MinCostpath->UpdateHyperpathCost();
			vector<GLINK*> glinks= path->GetGlinks();
			floatType DerSum = 0.0;	//g_k
			//shift flow from path -> minpath
			for(int i=0;i<mglinks.size();i++)
			{
				link = mglinks[i]->m_linkPtr;
				link->buffer[0] = mglinks[i]->m_data;
				link->markStatus = 1;// tag 1 for arcs on the shortest path
			}
			for(int i=0;i<glinks.size();i++)//
			{
				glinks[i]->m_linkPtr->buffer[1] = glinks[i]->m_data;
			}
				
			for(int i=0;i<mglinks.size();i++)
			{
				link = mglinks[i]->m_linkPtr;
				double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
				if (link->rLink)
				{
					tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
					//cout<<"rpfdcost="<<link->rpfdcost<<","<<link->rLink->buffer[0]<<","<<link->rLink->buffer[1]<<endl;
				}
				DerSum += tmp *(link->buffer[1] - link->buffer[0]);				
			}
			// calculate derivative for non-shortest links
			for(int i=0;i<glinks.size();i++)
			{
				link = glinks[i]->m_linkPtr;
				if (link->markStatus != 1)
				{
					double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
					if (link->rLink)
					{
						tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
						//cout<<"rpfdcost="<<link->rpfdcost<<","<<link->rLink->buffer[0]<<","<<link->rLink->buffer[1]<<endl;
					}
					DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
				
				}
			}

			//更新dflow，dflow==e
			if (DerSum ==0)	DerSum = 1e-8;//no need to shit flow from current path to the shortest path 
			double dev = path->cost - MinCostpath->cost;
			double dflow;
			if (dev>=0) 
			{
				dflow =  __min(1.0 * dev /DerSum, path->flow);
				//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
			}
			else
			{
				dflow =  __max(1.0 * dev /DerSum, -MinCostpath->flow);	
				//cout<<1.0 * dev /DerSum<<","<< MinCostpath->flow<<",dflow"<<dflow<<endl;
			}
			//cout<<"org:"<<org->org->id<<",dest:"<<dest->destination->id<<",dflow:"<<dflow<<",DerSum:"<<DerSum<<",pathcost:"<<path->cost<<",minpathcost:"<<MinCostpath->cost<<endl;
			
			//更新path流量
			MinCostpath->flow = MinCostpath->flow + dflow; 	
			path->flow = path->flow - dflow; 			
			if (path->flow<0||MinCostpath->flow<0)
			{
				cout<<path->flow<<","<<MinCostpath->flow<<endl;
				system("PAUSE");
			}
			//更新弧k流量，费用，一阶导数
			for(int lj=0;lj<glinks.size();lj++)
			{
				link = glinks[lj]->m_linkPtr;
				link ->volume += glinks[lj]->m_data* (path->flow - path->Preflow);
				if (link->volume<0)
				{
					//cout<<link->volume<<",preflow:"<<path->Preflow<<",flow:"<<path->flow<<",dflow:"<<dflow<<endl;
				}

				if (abs(link->volume) < 1e-8) link ->volume = 0.0;
				link ->UpdatePTLinkCost();
				link ->UpdatePTDerLinkCost();
				if (link ->rLink)
				{
					link ->rLink ->UpdatePTLinkCost();
					link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				}
						
				link->buffer[1]=0.0;// reset the link prob
			}

			//更新弧k-流量，费用及一阶导数
			for(int ii=0;ii<mglinks.size();++ii)
			{
				link = mglinks[ii]->m_linkPtr;
				link ->volume += mglinks[ii]->m_data * (MinCostpath->flow - MinCostpath->Preflow);
				if (link->volume<0)
				{
					//cout<<link->volume<<",preflow:"<<MinCostpath->Preflow<<",flow:"<<MinCostpath->flow<<",dflow:"<<dflow<<endl;
				}
				if (abs(link->volume) < 1e-8) link ->volume = 0.0;
				link->buffer[0]=0.0;// reset the link prob
				link->markStatus =0;
				link ->UpdatePTLinkCost();
				link ->UpdatePTDerLinkCost();
				if (link ->rLink)
				{
					link ->rLink ->UpdatePTLinkCost();
					link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				}
			}
			//cout<<""<<endl;
		}
	}
	org->PathFlowConservationTP();
}

void PTNET::UpdateHyperpathLineSearchFlow(PTDestination* dest,PTOrg* org)
{
	/*PTOrg* org = (MMOrg*)orig;*/
	TNM_HyperPath* MinCostpath=org->pathSet[org->minIx];
	vector<GLINK*> mglinks=MinCostpath->GetGlinks();
	PTLink *link,*rlink;
	/*if(curIter >=100 && org->assDemand >0)
	{
		cout<<"shift before:"<<endl;
		org->printPathSet();
	}*/
	for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
		path->Preflow = path->flow;
		MinCostpath->Preflow = MinCostpath->flow;
		if (path != MinCostpath)
		{
			path->UpdateHyperpathCost();
			MinCostpath->UpdateHyperpathCost();
			vector<GLINK*> glinks= path->GetGlinks();
			floatType DerSum = 0.0;	
	
			int iter = 0; 
			int maxLineSearchIter = 100;
			floatType lineSearchAccuracy = 1e-12;
			floatType dev = path->cost - MinCostpath->cost;
			floatType ldflow = 0.0,rdflow = 0.0;
			if (dev>0) 
			{
				rdflow =  path->flow;
			}
			else
			{
				ldflow =  - MinCostpath->flow;	
			}
			floatType dflow=0.0 ,lastdflow = 0.0,shiftflow;
			/*cout<<"iter:"<<iter<<"; dev:"<<dev<<"; ldflow:"<<ldflow<<"; rdflow:"<<rdflow<<"; dflow:"<<dflow
					<<"; lastdflow:"<<lastdflow<<"; pathflow:"<<path->flow<<"; minflow:"<<MinCostpath->flow<<"\n"<<endl;*/
			while(iter < maxLineSearchIter && abs(dev)>=lineSearchAccuracy &&
				((dev>0 && path->flow > 0) ||(dev<0 && MinCostpath->flow > 0)))
			{
				iter = iter + 1;
				dflow = (ldflow + rdflow)/2.0 ;

				MinCostpath->flow = MinCostpath->flow + dflow -lastdflow; 	
				path->flow = path->flow - dflow + lastdflow; 			
				if((abs(path->flow)> 1e-10 && path->flow <0)||(abs(MinCostpath->flow)> 1e-10 && MinCostpath->flow<0))
				{
					cout<<path->flow<<","<<MinCostpath->flow<<endl;
					system("PAUSE");
				}
				bool resetdflow = false;
			
				for(int lj=0;lj<glinks.size();lj++)
				{
					link = glinks[lj]->m_linkPtr;
					
					floatType linkpreflow = link->volume;
					floatType glinkpreflow = glinks[lj]->m_gflow;
					
					link->volume += glinks[lj]->m_data * (path->flow + lastdflow - path->Preflow);
					
					if (abs(link->volume) > 1e-10 && link->volume < 0.0)
					{
						cout<<link->volume<<",path preflow:"<<path->Preflow<<",flow:"<<path->flow<<",dflow:"<<dflow<<endl;
						//system("PAUSE");
						//resetdflow = true;	
					}

					if (abs(link->volume) < 1e-10 && link->volume < 0.0) link ->volume = 0.0;
					link ->UpdatePTLinkCost();
					link ->UpdatePTDerLinkCost();
					rlink = link ->rLink;
					if (rlink)
					{
						rlink->UpdatePTLinkCost();
						rlink ->UpdatePTDerLinkCost();
					}						
					
					glinks[lj]->m_gflow += glinks[lj]->m_data * (path->flow + lastdflow - path->Preflow);
					if (abs(glinks[lj]->m_gflow) < 1e-10 && glinks[lj]->m_gflow < 0.0) glinks[lj]->m_gflow  = 0.0;
					/*if(curIter >=0)
					{
						cout<<"link id :"<<link->id<<", linkpreflow:"<<linkpreflow <<",flow:"<<link->volume
							<<", glinkpreflow:"<<glinkpreflow<<", glinkflow:"<<glinks[lj]->m_gflow <<" ,lprob:"<<glinks[lj]->m_data<<",dflow:"<<path->flow - path->Preflow<<endl;
					}*/
					
				}
				
				for(int ii=0;ii<mglinks.size();++ii)
				{
					link = mglinks[ii]->m_linkPtr;
					floatType linkpreflow = link->volume;
					floatType mglinkpreflow = mglinks[ii]->m_gflow;

					link->volume += mglinks[ii]->m_data * (MinCostpath->flow - lastdflow - MinCostpath->Preflow);
					
					if (abs(link->volume) > 1e-10 && link->volume < 0.0)
					{
						cout<<link->volume<<",minpath preflow:"<<MinCostpath->Preflow<<",flow:"<<MinCostpath->flow<<",dflow:"<<dflow<<",lastdflow:"<<lastdflow<<endl;
						//system("PAUSE");
						//resetdflow = true;
					}
					if (abs(link->volume) < 1e-10 && link->volume < 0.0) link ->volume = 0.0;
					link->UpdatePTLinkCost();
					link->UpdatePTDerLinkCost();
					rlink = link ->rLink;
					if (rlink)
					{
						rlink->UpdatePTLinkCost();
						rlink ->UpdatePTDerLinkCost();
				
					}
					//cout<<mglinks[ii]->m_gflow<<",mpath preflow:"<<MinCostpath->Preflow<<",flow:"<<MinCostpath->flow<<",dflow:"<<lastdflow<<endl;
						
				
					mglinks[ii]->m_gflow += mglinks[ii]->m_data * (MinCostpath->flow - lastdflow - MinCostpath->Preflow);						
					if (abs(mglinks[ii]->m_gflow) < 1e-10 && mglinks[ii]->m_gflow < 0.0)mglinks[ii]->m_gflow  = 0.0;
					mglinks[ii]->UpdateGLinkCost();
					/*if(curIter >=0 )
					{
						cout<<"mlink id :"<<link->id<<", mlinkpreflow:"<<linkpreflow <<",flow:"<<link->volume
							<<", mglinkpreflow:"<<mglinkpreflow<<", mglinkflow:"<<mglinks[ii]->m_gflow <<",lprob:"<<mglinks[ii]->m_data<<",dflow:"<<MinCostpath->flow - MinCostpath->Preflow<<endl;
					}*/
				
				}
				
				path->UpdateHyperpathCost();
				MinCostpath->UpdateHyperpathCost();
				dev = path->cost - MinCostpath->cost;
				lastdflow = dflow;
				if(dev>0 && !resetdflow)
				{
					ldflow = dflow;
				}
				else  
				{
					rdflow = dflow;
				}
				//if(curIter >=0 &&org->assDemand >0)
				/*if(tempcountc>=493515)
				{
					cout<<"iter:"<<iter<<"; dev:"<<dev<<"; ldflow:"<<ldflow<<"; rdflow:"<<rdflow<<"; dflow:"<<dflow
						<<"; lastdflow:"<<lastdflow<<"; pathflow:"<<path->flow<<"; minflow:"<<MinCostpath->flow<<"\n"<<endl;

					cout<<"print minpath:"<<endl;
					MinCostpath->print();
					cout<<"print path:"<<endl;
					path->print();
				}
*/
				shiftflow = dflow;
				
			}
			path->UpdateGLinksCost();
			MinCostpath->UpdateGLinksCost();

			/*if(curIter >=0 &&org->assDemand >0)
			{
				cout<<"org:"<<org->org->name<<",dest:"<<dest->destination->name<<",pathcost:"<<path->cost
					<<",minpathcost:"<<MinCostpath->cost<<"; pathflow:"<<path->flow<<"; minflow:"<<MinCostpath->flow<<"; shiftflow:"<<shiftflow<<"\n"<<endl;
				cout<<"print minpath:"<<endl;
				MinCostpath->print();
				cout<<"print path:"<<endl;
				path->print();
			}*/
		}
	}
	/*if(curIter >=100 &&org->assDemand >0)
	{
		cout<<"shift after:"<<endl;
		org->printPathSet();
	}*/
	
	org->PathFlowConservationTP();	
}


void PTNET::HyperpathInnerLoop(int maxiters)
{
	int n = 0;
	int numofbadODpairs;
	PTDestination* dest;
	clock_t sclock = clock();
	double inner_indicator = RGapIndicator;
	while (n < maxiters)//设置最大内循环次数
	{
		numofbadODpairs = 0;
		double tmpcost = 0.0;
		double tmpmincost = 0.0;
		for (int i = 0;i<numOfPTDest; i++)
		{
			dest = PTDestVector[i];
			for (int j=0; j<dest->numOfOrg; j++)
			{
				PTOrg* org= dest->orgVector[j];//对于每一个OD对，判断是否需要执行内循环
				/*if (org->pathSet.size()==0)
				{org->state=false;}
				else
				{org->state=true;}*/
				/*if (org->state)
				{*/
				org->UpdatePathSetCost();//更新OD对下的路径费用，以及该OD对下的收敛指标
				if(!org->pathSet.empty())
				{
					double mincost = org->pathSet[org->minIx]->cost;
					double rg = fabs( 1 - mincost * org->assDemand / org->currentTotalCost);//计算收敛指标		
					if(rg > inner_indicator && org->pathSet.size() > 1)//如果该OD对下的收敛指标不佳
					{
						if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy)		UpdateHyperPathGreedyFlow(dest,org);	
						else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP)	UpdateHyperpathGPFlow_original(dest,org);
						//else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP) UpdateHyperpathLineSearchFlow(dest,org);
						else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP)		UpdateHyperpathGPFlow(dest,org);
						else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP)		UpdateHyperPathGreedyFlow(dest,org);
						numofbadODpairs++;
					}
					//}
					tmpcost += org->currentTotalCost;//叠加所有OD对的currentTotalCost
					tmpmincost += mincost * org->assDemand;//叠加所有OD对的xxx
				}
			}
		}
		double crg =  fabs((tmpcost - tmpmincost)/tmpcost);
		inner_indicator = crg;//更新内循环的收敛指标
		n++;
		if(numofbadODpairs == 0 ) break;
	}
	IterInnerlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;
	InnerIters = n;
}



void PTNET::HyperpathInnerLoopTP(int maxiters)
{
	int n = 0;
	int numofbadODpairs;
	PTDestination* dest;
	clock_t sclock = clock();
	double inner_indicator = RGapIndicator;
	while (n < maxiters)
	{
		numofbadODpairs = 0;
		double tmpcost = 0.0;
		double tmpmincost = 0.0;
		for (int i = 0;i<numOfPTDest; i++)
		{
			dest = PTDestVector[i];
			for (int j=0; j<dest->numOfOrg; j++)
			{
				PTOrg* org= dest->orgVector[j];
				/*if (org->pathSet.size()==0)
				{org->state=false;}
				else
				{org->state=true;}*/
				/*if (org->state)
				{*/
				org->UpdatePathSetCostTP();
				if(!org->pathSetTP.empty())
				{
					double mincost = org->pathSetTP[org->minIx]->cost;
					double rg = fabs( 1 - mincost * org->assDemand / org->currentTotalCost);		
					if(rg > inner_indicator && org->pathSetTP.size() > 1)
					{
						if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy)		UpdateHyperPathGreedyFlow(dest,org);
						else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP)		UpdateHyperpathGPFlow(dest,org);
						else if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP)	UpdateHyperpathGPFlowTP(dest,org);
						else if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP)		UpdateHyperPathGreedyFlowTP(dest,org);
						numofbadODpairs++;
					}
					//}
					tmpcost += org->currentTotalCost;
					tmpmincost += mincost * org->assDemand;
				}
			}
		}
		double crg =  fabs((tmpcost -tmpmincost)/tmpcost);
		inner_indicator = crg;
		n++;
		if(numofbadODpairs == 0 ) break;
	}
	IterInnerlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;
	InnerIters = n;
}

int	PTNET::SolveBushTEAP()
{
	m_startRunTime = clock();
	AllocateLinkBuffer(2);
	AllocateNodeBuffer(2);
	UpatePTNetworkLinkCost();
	StgNetInitialize();
	UpatePTNetworkLinkCost();
	
	//PrintNetLinks();
	//ComputeConvGap();
	//RecordTEAPCurrentIter();
	//cout<<"iter:"<<curIter<<",\t current gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<",pIter:"<<0<<endl<<endl;
	if(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))))
	{
		do
		{
			PTDestination* dest;
			PTOrg* org;
			floatType gap, tgap; //set initial value of the maximum flow 

			floatType looptime1 = 0.0, markstgtime = 0.0,remarkstgtime = 0.0, updatestgtime = 0.0, expandstgtime = 0.0;
			clock_t t0 = clock();

			//tt = 0;
			//expand network
			for (int i = 0;i<numOfPTDest;i++)
			{		
				dest = PTDestVector[i];

				clock_t t1 = clock();

				dest->MarkStgLinksOnNet();

				clock_t t2 = clock();
				markstgtime += (t2 - t1)*1.0 / CLOCKS_PER_SEC;

				if(dest->m_trimmed)  
				{
					if(!dest->StgTopologOrder(this)) return -1; //before start, first build the topolog order.		
				}
				tgap = dest->UpdateStgPotential(true);	

				clock_t t3 = clock();
				updatestgtime += (t3 - t2)*1.0 / CLOCKS_PER_SEC;	

				int nAdded = dest->ExpandStgSubNet(this);

				clock_t t4 = clock();
				expandstgtime += (t4 - t3) * 1.0 / CLOCKS_PER_SEC;

				dest->RemarkStgLinksOnNet();	

				clock_t t5 = clock();
				remarkstgtime += (t5 - t4) * 1.0 / CLOCKS_PER_SEC;	
			}
			//cout<<"common-line problem:"<<tt<<endl;
			clock_t t6 = clock();
			looptime1 = (t6 - t0)*1.0 / CLOCKS_PER_SEC;		

			int pIter = 0, pBad = 0;
			gap = 1.0;
			floatType oldgap = gap;

			floatType looptime2=0.0,marktime2 = 0.0,remarktime2 = 0.0,updatestg2 = 0.0,shifttime = 0.0;
			
			numaOfShift = 0;
			while (gap > m_innerConv && pIter < 10 && pBad < 5)
			{
				gap = 0.0;
				int bad = 0;
				int NumofLowGapDest = 0;

				for (int i = 0;i<numOfPTDest;i++)
				{
					dest = PTDestVector[i];

					clock_t t1 = clock();

					dest->MarkStgLinksOnNet();

					clock_t t2 = clock();
					marktime2 += (t2 - t1)*1.0 / CLOCKS_PER_SEC;

					tgap = dest->UpdateStgPotential(false);

					clock_t t3 = clock();
					updatestg2 += (t3 - t2)*1.0 / CLOCKS_PER_SEC;

					if(tgap> m_innerConv) 
						OBStgFlowShift(dest);

					clock_t t4 = clock();
					shifttime += (t4 - t3)*1.0 / CLOCKS_PER_SEC;

					if(gap< tgap) gap = tgap;		
					dest->RemarkStgLinksOnNet();

					clock_t t5 = clock();
					remarktime2 += (t5 - t4)*1.0 / CLOCKS_PER_SEC;		
				}
				pIter++;
				if(oldgap - gap <= m_innerConv) 
				{
					pBad++;			
				}
				else pBad = 0;
				oldgap = gap;
			}
			InnerIters = pIter;
			clock_t t7 = clock();
			looptime2 = (t7 - t6)*1.0 / CLOCKS_PER_SEC;		
			//cout<<"perform number of shift flow:"<<netTTwaitcost<<endl;
			netTTwaitcost = 0.0;
			//m_walkNum = 0;
			for (int i = 0;i<numOfPTDest;i++)
			{
				//cout<<"Trim for dest id:"<<dest->destination->id<<endl;		
				dest = PTDestVector[i];
				dest->MarkStgLinksOnNet();//first mark all OB nodes;
				AggregateDestStgFlows(dest);
				dest->TrimStgSubNet(this);
				dest->RemarkStgLinksOnNet();
			}
			clock_t t8 = clock();
			floatType trimlooptime = (t8 - t7)*1.0 / CLOCKS_PER_SEC;	

			//cout<<"num of stgs:"<<m_walkNum<<endl;
			if (false)
			{
			cout<<"loop1:"<<TNM_FloatFormat(looptime1,5,2)<<" (markstgtime:"<<TNM_FloatFormat(markstgtime,5,2)<<", remarkstgtime:"<<TNM_FloatFormat(remarkstgtime,5,2)
				<<", updatestgtime:"<<TNM_FloatFormat(updatestgtime,5,2)<<", expandstgtime:"<<TNM_FloatFormat(expandstgtime,5,2)<<")"<<endl;

			cout<<"loop2:"<<TNM_FloatFormat(looptime2,5,2)<<" (marktime2:"<<TNM_FloatFormat(marktime2,5,2)<<", remarktime2:"<<TNM_FloatFormat(remarktime2,5,2)
				<<", updatestg2:"<<TNM_FloatFormat(updatestg2,5,2)<<",shift:"<<TNM_FloatFormat(shifttime,5,2)<<")"<<endl;
			
			cout<<"trim:"<<TNM_FloatFormat(trimlooptime,5,2)<<endl;
			}		
			curIter++;
			ComputeConvGap();
			RecordTEAPCurrentIter();
			cout<<"iter:"<<curIter<<",\t current gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<",pIter:"<<pIter<<",shifttimes:"<<numaOfShift<<endl<<endl;

		}while(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))));
	}
	//cout<<"iter:"<<curIter<<",\t current gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<endl<<endl;
	cout<<"CPU TIME: "<<1.0*(clock() - m_startRunTime)<<endl;
	//ReportPTStrategies();
	return 0;
}

/*
floatType PTNET::eOBStgFlowShift(PTDestination* dest)
{
	floatType dev = 0.0;
	PTNode *node;
	vector<GNODE *>::reverse_iterator po;
	dest->destination->StgElem->via = NULL;
	dest->destination->rStgElem->via = NULL;
	GNODE* gnode;
	for(po = dest->tplNodeVec.rbegin(); po != dest->tplNodeVec.rend(); po++)
	{
		gnode = *po;
		node=(*po)->m_ptnodePtr;
		StgLinks* mfstg = NULL;
		floatType f = 1e-10;
		for (vector<StgLinks*>::iterator pt = gnode->m_StgsVec.begin();pt != gnode->m_StgsVec.end();pt++)
		{
			StgLinks* stg = *pt;
			//if (stg->sflow>f && stg!=node->StgElem->via)
			//{
			//	f = stg->sflow;
			//	mfstg = stg;
			//}
			if (stg->sflow>0 && stg!=node->StgElem->via)
			{
				//cout<<stg->sname<<"，"<<stg->sflow<<endl;
				node->eStgElem->via = stg;
				dest->eStgNodeFlowShift(node,this);
			}
		}
		//if (mfstg)
		//{
		//	node->eStgElem->via = mfstg;
		//	//cout<<dest->destination->id;
		//	dest->eStgNodeFlowShift(node,this);
		//}

		
	
	}
	return dev;

}
*/

floatType PTNET::OBStgFlowShift(PTDestination* dest)
{
	
	floatType dev;
	PTNode *node;
	vector<GNODE *>::reverse_iterator po,no;
	dest->destination->StgElem->via = NULL;
	dest->destination->rStgElem->via = NULL;
	//cout<<endl;
	int i = 0;
	//do
	//{
		i++;
		dev = 0.0;
		numaOfShift++;
		for(po = dest->tplNodeVec.rbegin(); po != dest->tplNodeVec.rend(); po++)
		{

			node=(*po)->m_ptnodePtr;
			if (node->StgElem->via != node->rStgElem->via && node->rStgElem->via) //&& (node->StgElem->via->sflow+ node->rStgElem->via->sflow ) > flowPrecision
			{
			//	cout<<"dest:"<<dest->destination->id<<",nodeid:"<<node->id<<", via maxstg:"<<node->m_rStgVia->sname<<", stgflow"<<node->m_rStgVia->sflow<<", via minstg:"<<node->m_StgVia->sname<<", stgflow"<<node->m_StgVia->sflow
				//		<<" to lcnId:"<<(*po)->m_ptnodeLCNPtr->id<<endl;

				//compute cost difference for ephs
				//for(no = dest->tplNodeVec.rbegin(); no != dest->tplNodeVec.rend(); no++)
				//{
				//	PTNode *bnode = (*no)->m_ptnodePtr;
				//	if (bnode->StgElem->via != bnode->rStgElem->via && bnode->rStgElem->via)
				//	{
				//		double prediff = dest->StgNodeCostDiff((*no)->m_ptnodePtr,this);
				//		(*no)->prediff = prediff;
				//	}
				//}



				// compute relative gap before shifting flow
				//netTTwaitcost = 0;
				//for (int i = 0;i<numOfPTDest;i++)
				//{
				//	PTDestination* pdest = PTDestVector[i];
				//	pdest->MarkStgLinksOnNet();//first mark all OB nodes;
				//
				//	for (vector<GNODE *>::iterator pv = pdest->tplNodeVec.begin(); pv!=pdest->tplNodeVec.end(); pv++)
				//	{
				//		GNODE * gnode = (*pv);
	
				//		for (vector<StgLinks*>::iterator it = gnode->m_StgsVec.begin();it!=gnode->m_StgsVec.end();it++)
				//		{
				//			netTTwaitcost += (*it)->sflow * (*it)->waitT;
				//		}
				//	}
				//	pdest->RemarkStgLinksOnNet();
				//}
				//dest->MarkStgLinksOnNet();
				//ComputeConvGap();
				//double r1 = RGapIndicator;
				//double prediff = dest->StgNodeCostDiff(node,this);
				
				dev = __max(dev,dest->StgNodeFlowShift(node,this)) ;				

				//double afterdiff = dest->StgNodeCostDiff(node,this);

//				for(no = dest->tplNodeVec.rbegin(); no != dest->tplNodeVec.rend(); no++)
//				{
//						PTNode *bnode = (*no)->m_ptnodePtr;
//						if (bnode->StgElem->via != bnode->rStgElem->via && bnode->rStgElem->via)
//						{
//							double afterdiff = dest->StgNodeCostDiff((*no)->m_ptnodePtr,this);
//							(*no)->afterdiff = afterdiff;
//
//							if ((*no)->prediff - (*no)->afterdiff <0)
//							{
///*								if ((*no)->m_tpLevel>(*po)->m_tpLevel)
//									cout<<"Upper dest:"<<dest->destination->id<<",node:"<<node->id<<",bad node:"<<(*no)->m_ptnodePtr->id<<",prediff:"<<(*no)->prediff<<",afterdiff:"<<(*no)->afterdiff<<",gap:"<<(*no)->prediff-(*no)->afterdiff<<endl;
//								else
//									cout<<"Lower dest:"<<dest->destination->id<<",node:"<<node->id<<",bad node:"<<(*no)->m_ptnodePtr->id<<",prediff:"<<(*no)->prediff<<",afterdiff:"<<(*no)->afterdiff<<",gap:"<<(*no)->prediff-(*no)->afterdiff<<endl;
//			*/					
//								if ((*no) == (*po))
//								{
//									cout<<"Itself dest:"<<dest->destination->id<<",node:"<<node->id<<",bad node:"<<(*no)->m_ptnodePtr->id<<",prediff:"<<(*no)->prediff<<",afterdiff:"<<(*no)->afterdiff<<",gap:"<<(*no)->prediff-(*no)->afterdiff<<endl;
//								
//								}
//							}
//						}
//				}
//
//				// compute relative gap before shifting flow
//				netTTwaitcost = 0;
//				for (int i = 0;i<numOfPTDest;i++)
//				{
//					PTDestination* pdest = PTDestVector[i];
//					pdest->MarkStgLinksOnNet();//first mark all OB nodes;
//				
//					for (vector<GNODE *>::iterator pv = pdest->tplNodeVec.begin(); pv!=pdest->tplNodeVec.end(); pv++)
//					{
//						GNODE * gnode = (*pv);
//	
//						for (vector<StgLinks*>::iterator it = gnode->m_StgsVec.begin();it!=gnode->m_StgsVec.end();it++)
//						{
//							netTTwaitcost += (*it)->sflow * (*it)->waitT;
//						}
//					}
//					pdest->RemarkStgLinksOnNet();
//				}
//				dest->MarkStgLinksOnNet();
//				ComputeConvGap();
//				double r2= RGapIndicator;
//
//				if (r1-r1<0)
//					cout<<"dest:"<<dest->destination->id<<",node:"<<node->id<<",prerg"<<r1<<",afterrg:"<<r2<<",gap:"<<r1-r2<<endl;
			}
		}
		//cout<<"numaOfShift::"<<numaOfShift<<",dev:"<<dev<<",rg:"<<RGapIndicator<<endl;
	//}while(dev>RGapIndicator*1000.0 && i<1);

	return dev;

}

void PTNET::AggregateDestStgFlows(PTDestination* dest)
{
	vector<GNODE *>::reverse_iterator po;
	GNODE *gnode;
	PTNode *node;
	PTLink* link;
	floatType ep = 1e-10;
	
	for(int i = 0;i<dest->numOfOrg;i++)
	{
		PTOrg* org= dest->orgVector[i];	
		org->org->buffer[0] = org->assDemand;
	}

	for(po = dest->tplNodeVec.rbegin(); po != dest->tplNodeVec.rend(); po++)
	{
		gnode = *po;
		node = gnode->m_ptnodePtr;
		StgLinks* maxStg; //find the stg with max flow
		floatType maxFlow = -1.0, tf;
		floatType af = node->buffer[0];// total assignment out flow
		if (node != dest->destination)
		{
			for (vector<StgLinks*>::iterator pt = gnode->m_StgsVec.begin();pt != gnode->m_StgsVec.end();pt++)
			{
				StgLinks* stg = *pt;
				tf = stg->sflow;
				if (tf >  maxFlow)
				{
					maxStg = stg; 
					maxFlow = tf;
				}
				if (stg->sflow < ep)
				{
					stg->sflow = 0.0;
					for (int i=0;i<stg->numofstglinks;i++)
					{
						int j = stg->StgLinkPosVec[i];
						link = node->forwStar[j];
						link->volume -=  tf * stg->StgLinkProbVec[i];
						if (link->volume<flowPrecision) link->volume = 0.0;
						link ->UpdatePTLinkCost();
						link ->UpdatePTDerLinkCost();
						if (link ->rLink)
						{
							link ->rLink ->UpdatePTLinkCost();
							link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
						}
					}
				}
				else
				{
					af -= tf;
					for (int i=0;i<stg->numofstglinks;i++)
					{
						int j = stg->StgLinkPosVec[i];
						link = node->forwStar[j];
						link->head->buffer[0] += tf * stg->StgLinkProbVec[i];

					}	
				}
			}
			if(af!=0) // give the surplus flow to maxstg 
			{
				maxStg->sflow += af;
				for (int i=0;i<maxStg->numofstglinks;i++)
				{
					int j = maxStg->StgLinkPosVec[i];
					link = node->forwStar[j];
					link->volume +=  af * maxStg->StgLinkProbVec[i];
					if (link->volume<flowPrecision) link->volume = 0.0;
					link ->UpdatePTLinkCost();
					link ->UpdatePTDerLinkCost();
					if (link ->rLink)
					{
						link ->rLink ->UpdatePTLinkCost();
						link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
					}
					link->head->buffer[0] += af * maxStg->StgLinkProbVec[i];
				}
			}
		}
		node->buffer[0] = 0.0;

	}
}
/*
floatType	PTDestination::eStgNodeFlowShift(PTNode *node,PTNET *net)
{
	floatType dev =0.0,DerSum = 0.0;
	StgLinks *stg;
	vector<GNODE*> Q,rQ;// this store the node on shortest/longest hyperpath
	vector<GNODE*>::iterator ig;
	double maxcost = 0.0,mincost = 0.0,maxstgflow = POS_INF_FLOAT,minstgflow = POS_INF_FLOAT ,shift;
	GNODE* node0 =  node->m_stgNode->m_ptnodeLCNPtr->m_stgNode;
	GNODE *minNode,*maxNode,*headnode,*sinknode //real LCN;
	GNODE *lastNode = NULL;
	PTLink *link;
	bool flag = false; // this indicate whether node0 is not the real LCN
	int ix = 0; //node index in the vector

	//cout<<"pair:"<<node->id<<","<<node->m_stgNode->m_ptnodeLCNPtr->id<<endl;
	#pragma region // scan to obtan node queue
	Q.push_back(node->m_stgNode);
	ix = 0;
	while (ix<Q.size())
	{
		minNode = Q[ix];
		stg = minNode->m_ptnodePtr->StgElem->via;
		if (stg)
		{
			//cout<<stg->numofstglinks<<",";
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->scanStatus==0&&link->head->m_stgNode!=node0)
				{
					link->head->scanStatus = 3;
					Q.push_back(link->head->m_stgNode);				
				}		
			}
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since stg does not exist from node"<<minNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}

	//obtain node in the largest hyperpath
	rQ.push_back(node->m_stgNode);
	ix = 0;
	//cout<<endl;
	while (ix<rQ.size())
	{
		maxNode = rQ[ix];
		if (maxNode!=node->m_stgNode) 
		{
			floatType f = -1;
			StgLinks* mfstg = NULL;
			for (vector<StgLinks*>::iterator pt = maxNode->m_StgsVec.begin();pt != maxNode->m_StgsVec.end();pt++)
			{	
				//cout<<(*pt)->sname<<","<<(*pt)->sflow<<endl;
				if ((*pt)->sflow>f )
				{
					f = (*pt)->sflow;
					mfstg = (*pt);
				}
			}
			if (mfstg)
			{
				 maxNode->m_ptnodePtr->eStgElem->via = mfstg;
			}
		}
		stg = maxNode->m_ptnodePtr->eStgElem->via;
		//cout<<stg->sname<<"->";
		if (stg)
		{
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = maxNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->m_stgNode->status==0&&link->head->m_stgNode!=node0)
				{
					link->head->m_stgNode->status = 3;
					rQ.push_back(link->head->m_stgNode);			
				}		
			}
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since maxstg does not exist from node"<<maxNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}
	//cout<<endl;
	sort(Q.begin(),Q.end(),comp_gnode_level);
	sort(rQ.begin(),rQ.end(),comp_gnode_level);
	#pragma endregion

	#pragma region// update node link prob on the shortest and longest hyperpath
	node->buffer[0] = 1.0;
	node->buffer[1] = 1.0;
	for (ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;	
		stg = minNode->m_ptnodePtr->StgElem->via;
		mincost += stg->waitT * minNode->m_ptnodePtr->buffer[0];
			
		for (int i=0;i<stg->numofstglinks;i++)
		{
			int j = stg->StgLinkPosVec[i];
			link = minNode->m_ptnodePtr->forwStar[j];
			floatType p = minNode->m_ptnodePtr->buffer[0] * stg->StgLinkProbVec[i];		
			mincost += p * link->cost;	
			link->buffer[0] = p;	
			link->head->buffer[0] += p;
		}	

		if (lastNode)
			minNode->minShiftflow = minstgflow;
		if (stg->sflow / minNode->m_ptnodePtr->buffer[0] < minstgflow) 
			minstgflow = stg->sflow / minNode->m_ptnodePtr->buffer[0];
		lastNode = minNode;
	}
	node0->minShiftflow = minstgflow;
	//cout<<node0->m_ptnodePtr->buffer[0]<<endl;
	node0->m_ptnodePtr->buffer[0] = 0.0;
	
	lastNode = NULL;
	for (ig=rQ.begin();ig != rQ.end();ig++)
	{
		maxNode = *ig;
		//cout<<maxNode->m_ptnodePtr->id<<"->";
		if (maxNode->m_ptnodePtr->scanStatus==3
			&&  fabs(maxNode->m_ptnodePtr->buffer[0]-1.0) < 1e-8
			&&  fabs(maxNode->m_ptnodePtr->buffer[1]-1.0) < 1e-8
			)
		{
			flag = true;
			maxNode->m_ptnodePtr->buffer[1] = 0.0;
			sinknode = maxNode;
			sinknode->maxShiftflow = maxstgflow;
			break;
		}
		stg = maxNode->m_ptnodePtr->eStgElem->via;	
		maxcost += stg->waitT * maxNode->m_ptnodePtr->buffer[1];
		
		for (int i=0;i<stg->numofstglinks;i++)
		{
			int j = stg->StgLinkPosVec[i];
			link = maxNode->m_ptnodePtr->forwStar[j];
			floatType p = maxNode->m_ptnodePtr->buffer[1] * stg->StgLinkProbVec[i];		
			maxcost += p * link->cost;		
			link->buffer[1] = p;	
			link->head->buffer[1] += p;
		}
		if (lastNode)
			maxNode->maxShiftflow = maxstgflow;
		if ((stg->sflow / maxNode->m_ptnodePtr->buffer[1]) < maxstgflow) 
			maxstgflow = stg->sflow / maxNode->m_ptnodePtr->buffer[1];

		lastNode = maxNode;
	}
	#pragma endregion

	#pragma region // cost recomputation according to flag
	//
	if (flag)
	{
		ig = find(Q.begin(), Q.end(),sinknode);
		if (ig!=Q.end())
		{
			while(ig!=Q.end())
			{
				minNode = *ig;
				minNode->m_ptnodePtr->scanStatus = 0;
				stg = minNode->m_ptnodePtr->StgElem->via;	
			    mincost -= stg->waitT * minNode->m_ptnodePtr->buffer[0];
				minNode->m_ptnodePtr->buffer[0] = 0.0;

				for (int i=0;i<stg->numofstglinks;i++)
				{
					int j = stg->StgLinkPosVec[i];
					link = minNode->m_ptnodePtr->forwStar[j];
					mincost -= link->buffer[0] * link->cost;	 	
					link->buffer[0] = 0.0;
				}	
				ig++;
			}
		}
		else
		{
			cout<<"can not find real common node in set Q, impossible"<<endl;
			system("PAUSE");
		}
	}
	else 
	{
		node0->m_ptnodePtr->buffer[1] = 0;
		node0->maxShiftflow = maxstgflow;
		sinknode = node0;
	}
	#pragma endregion

	#pragma region //calculate dervatives
	for(int i = 0;i<net->numOfLink;i++)
	{
		link =  net->linkVector[i];
		if (link->buffer[0] > 0 || link->buffer[1] > 0)
		{
			double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
			if (link->rLink)
			{
				tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
			}
			DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
		}
	}
	#pragma endregion

	dev = maxcost - mincost;
	
	if (dev>=0)
	{
		shift = __min(1.0 * dev /DerSum, sinknode->maxShiftflow);	
	}
	else
	{
		//shift = 0;
		shift = __max(1.0 * dev /DerSum, -sinknode->minShiftflow);	
	}

	#pragma region //shift flow on shortest hyperpath
	bool preflag=true;
	for (ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;
		minNode->m_ptnodePtr->scanStatus = 0;//reset node status
		if (minNode==sinknode) preflag=false;
		if (preflag)
		{
			stg = minNode->m_ptnodePtr->StgElem->via;	
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[j];
				link->volume +=  shift * link->buffer[0];
				if (link->volume<-1e-8)
				{
					cout<<"link flow is negative:"<<link->volume<<endl;
					system("PAUSE");
				}
				link->buffer[0] = 0.0;
				link ->UpdatePTLinkCost();
				link ->UpdatePTDerLinkCost();
				if (link ->rLink)
				{
					link ->rLink ->UpdatePTLinkCost();
					link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				}
			}	
			stg->sflow += minNode->m_ptnodePtr->buffer[0] * shift ;
			minNode->m_ptnodePtr->buffer[0] = 0.0;
		}
	}
	#pragma endregion

	preflag=true;
	#pragma region //shift flow on logest hyperpath
	for (ig=rQ.begin();ig != rQ.end();ig++)
	{
		maxNode = *ig;
		maxNode->status = 0;
		if (maxNode==sinknode) preflag=false;
		if (preflag)
		{

			stg = maxNode->m_ptnodePtr->eStgElem->via;
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = maxNode->m_ptnodePtr->forwStar[j];
				link->volume -=  shift * link->buffer[1];
				if (link->volume<-1e-8)
				{
					cout<<"link flow is negative:"<<link->volume<<endl;
					system("PAUSE");
				}
				link->buffer[1] = 0.0;
				link ->UpdatePTLinkCost();
				link ->UpdatePTDerLinkCost();
				if (link ->rLink)
				{
					link ->rLink ->UpdatePTLinkCost();
					link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				}
			}
			stg->sflow -= maxNode->m_ptnodePtr->buffer[1] * shift ;
			maxNode->m_ptnodePtr->buffer[1] = 0;
			maxNode->m_ptnodePtr->eStgElem->via = NULL;
		}
	}
	#pragma endregion

	////cout<<"asas"<<endl;
	return dev;
	
}
*/

floatType  PTDestination::StgNodeCostDiff(PTNode *node,PTNET *net)
{
	floatType dev =0.0,DerSum = 0.0;
	StgLinks *stg;
	vector<GNODE*> Q,rQ;// this store the node on shortest/longest hyperpath
	vector<GNODE*>::iterator ig;
	double maxcost = 0.0,mincost = 0.0,maxstgflow = POS_INF_FLOAT,minstgflow = POS_INF_FLOAT ,shift;
	GNODE* node0 =  node->m_stgNode->m_ptnodeLCNPtr->m_stgNode;
	GNODE *minNode,*maxNode,*headnode,*sinknode /*real LCN */;
	GNODE *lastNode = NULL;
	PTLink *link;
	bool flag = false; // this indicate whether node0 is not the real LCN
	int ix = 0; //node index in the vector

	Q.push_back(node->m_stgNode);
	ix = 0;
	while (ix<Q.size())
	{
		minNode = Q[ix];
		stg = minNode->m_ptnodePtr->StgElem->via;
		if (stg)
		{
			//cout<<stg->numofstglinks<<",";
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->scanStatus==0&&link->head->m_stgNode!=node0)
				{
					link->head->scanStatus = 3;
					Q.push_back(link->head->m_stgNode);				
				}		
			}
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since stg does not exist from node"<<minNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}
	////cout<<endl;
	//obtain node in the largest hyperpath
	rQ.push_back(node->m_stgNode);
	ix = 0;
	while (ix<rQ.size())
	{
		maxNode = rQ[ix];
		stg = maxNode->m_ptnodePtr->rStgElem->via;
		//cout<<stg->sname<<"->";
		if (stg)
		{
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = maxNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->m_stgNode->status==0&&link->head->m_stgNode!=node0)
				{
					link->head->m_stgNode->status = 3;
					rQ.push_back(link->head->m_stgNode);			
				}		
			}
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since maxstg does not exist from node"<<maxNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}
	//cout<<endl;
	sort(Q.begin(),Q.end(),comp_gnode_level);
	sort(rQ.begin(),rQ.end(),comp_gnode_level);

	node->buffer[0] = 1.0;
	node->buffer[1] = 1.0;
	for (ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;	
		stg = minNode->m_ptnodePtr->StgElem->via;
		mincost += stg->waitT * minNode->m_ptnodePtr->buffer[0];
			
		for (int i=0;i<stg->numofstglinks;i++)
		{
			int j = stg->StgLinkPosVec[i];
			link = minNode->m_ptnodePtr->forwStar[j];
			floatType p = minNode->m_ptnodePtr->buffer[0] * stg->StgLinkProbVec[i];		
			mincost += p * link->cost;	
			link->buffer[0] = p;	
			link->head->buffer[0] += p;
		}	

		if (lastNode)
			minNode->minShiftflow = minstgflow;
		if (stg->sflow / minNode->m_ptnodePtr->buffer[0] < minstgflow) 
			minstgflow = stg->sflow / minNode->m_ptnodePtr->buffer[0];
		lastNode = minNode;
	}
	node0->minShiftflow = minstgflow;
	//cout<<node0->m_ptnodePtr->buffer[0]<<endl;
	node0->m_ptnodePtr->buffer[0] = 0.0;
	
	lastNode = NULL;
	for (ig=rQ.begin();ig != rQ.end();ig++)
	{
		maxNode = *ig;
		//cout<<maxNode->m_ptnodePtr->id<<"->";
		if (maxNode->m_ptnodePtr->scanStatus==3
			&&  fabs(maxNode->m_ptnodePtr->buffer[0]-1.0) < 1e-8
			&&  fabs(maxNode->m_ptnodePtr->buffer[1]-1.0) < 1e-8
			)
		{
			flag = true;
			maxNode->m_ptnodePtr->buffer[1] = 0.0;
			sinknode = maxNode;
			sinknode->maxShiftflow = maxstgflow;
			break;
		}
		stg = maxNode->m_ptnodePtr->rStgElem->via;	
		maxcost += stg->waitT * maxNode->m_ptnodePtr->buffer[1];
		
		for (int i=0;i<stg->numofstglinks;i++)
		{
			int j = stg->StgLinkPosVec[i];
			link = maxNode->m_ptnodePtr->forwStar[j];
			floatType p = maxNode->m_ptnodePtr->buffer[1] * stg->StgLinkProbVec[i];		
			maxcost += p * link->cost;		
			link->buffer[1] = p;	
			link->head->buffer[1] += p;
		}
		if (lastNode)
			maxNode->maxShiftflow = maxstgflow;
		if ((stg->sflow / maxNode->m_ptnodePtr->buffer[1]) < maxstgflow) 
			maxstgflow = stg->sflow / maxNode->m_ptnodePtr->buffer[1];

		lastNode = maxNode;
	}

	if (flag)
	{
		ig = find(Q.begin(), Q.end(),sinknode);
		if (ig!=Q.end())
		{
			while(ig!=Q.end())
			{
				minNode = *ig;
				minNode->m_ptnodePtr->scanStatus = 0;
				stg = minNode->m_ptnodePtr->StgElem->via;	
			    mincost -= stg->waitT * minNode->m_ptnodePtr->buffer[0];
				minNode->m_ptnodePtr->buffer[0] = 0.0;

				for (int i=0;i<stg->numofstglinks;i++)
				{
					int j = stg->StgLinkPosVec[i];
					link = minNode->m_ptnodePtr->forwStar[j];
					mincost -= link->buffer[0] * link->cost;	 	
					link->buffer[0] = 0.0;
				}	
				ig++;
			}
		}
		else
		{
			cout<<"can not find real common node in set Q, impossible"<<endl;
			system("PAUSE");
		}
	}
	else 
	{
		node0->m_ptnodePtr->buffer[1] = 0;
		node0->maxShiftflow = maxstgflow;
		sinknode = node0;
	}


	

#pragma region //shift flow on shortest hyperpath
	bool preflag=true;
	for (ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;
		minNode->m_ptnodePtr->scanStatus = 0;//reset node status
		if (minNode==sinknode) preflag=false;
		if (preflag)
		{
			stg = minNode->m_ptnodePtr->StgElem->via;	
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[j];
				link->buffer[0] = 0.0;
			}	
			minNode->m_ptnodePtr->buffer[0] = 0.0;
		}
	}
	#pragma endregion

	preflag=true;
	#pragma region //shift flow on logest hyperpath
	for (ig=rQ.begin();ig != rQ.end();ig++)
	{
		maxNode = *ig;
		maxNode->status = 0;
		if (maxNode==sinknode) preflag=false;
		if (preflag)
		{

			stg = maxNode->m_ptnodePtr->rStgElem->via;
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = maxNode->m_ptnodePtr->forwStar[j];			
				link->buffer[1] = 0.0;
			}
			maxNode->m_ptnodePtr->buffer[1] = 0;
		}
	}
	#pragma endregion

	dev = abs( maxcost - mincost);
	return dev;
}

floatType	PTDestination::StgNodeFlowShift(PTNode *node,PTNET *net)
{
	
	//for (int i= 0;i<net->numOfNode;i++)
	//{
	//	PTNode* nd = net->nodeVector[i];
	//	if (nd->scanStatus!=0)
	//	{
	//		cout<<"dnwoudnwlnd"<<endl;
	//	}
	//	if (nd->m_stgNode)
	//	{
	//		//cout<<nd->id<<","<<nd->m_stgNode<<endl;
	//		if (nd->m_stgNode->status!=0) cout<<"---------------"<<endl;
	//	}
	//}

	
	floatType dev =0.0,DerSum = 0.0;
	StgLinks *stg;
	vector<GNODE*> Q,rQ;// this store the node on shortest/longest hyperpath
	vector<GNODE*>::iterator ig;
	double maxcost = 0.0,mincost = 0.0,maxstgflow = POS_INF_FLOAT,minstgflow = POS_INF_FLOAT ,shift;
	GNODE* node0 =  node->m_stgNode->m_ptnodeLCNPtr->m_stgNode;
	GNODE *minNode,*maxNode,*headnode,*sinknode /*real LCN */;
	GNODE *lastNode = NULL;
	PTLink *link;
	bool flag = false; // this indicate whether node0 is not the real LCN
	int ix = 0; //node index in the vector

	//cout<<"pair:"<<node->id<<","<<node->m_stgNode->m_ptnodeLCNPtr->id<<endl;
	#pragma region // scan to obtan node queue
	Q.push_back(node->m_stgNode);
	ix = 0;
	while (ix<Q.size())
	{
		minNode = Q[ix];
		stg = minNode->m_ptnodePtr->StgElem->via;
		if (stg)
		{
			//cout<<stg->numofstglinks<<",";
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->scanStatus==0&&link->head->m_stgNode!=node0)
				{
					link->head->scanStatus = 3;
					Q.push_back(link->head->m_stgNode);				
				}		
			}
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since stg does not exist from node"<<minNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}
	////cout<<endl;
	//obtain node in the largest hyperpath
	rQ.push_back(node->m_stgNode);
	ix = 0;
	while (ix<rQ.size())
	{
		maxNode = rQ[ix];
		stg = maxNode->m_ptnodePtr->rStgElem->via;
		//cout<<stg->sname<<"->";
		if (stg)
		{
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = maxNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->m_stgNode->status==0&&link->head->m_stgNode!=node0)
				{
					link->head->m_stgNode->status = 3;
					rQ.push_back(link->head->m_stgNode);			
				}		
			}
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since maxstg does not exist from node"<<maxNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}
	//cout<<endl;
	sort(Q.begin(),Q.end(),comp_gnode_level);
	sort(rQ.begin(),rQ.end(),comp_gnode_level);
	#pragma endregion
	
	#pragma region// update node link prob on the shortest and longest hyperpath
	node->buffer[0] = 1.0;
	node->buffer[1] = 1.0;
	for (ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;	
		stg = minNode->m_ptnodePtr->StgElem->via;
		mincost += stg->waitT * minNode->m_ptnodePtr->buffer[0];
			
		for (int i=0;i<stg->numofstglinks;i++)
		{
			int j = stg->StgLinkPosVec[i];
			link = minNode->m_ptnodePtr->forwStar[j];
			floatType p = minNode->m_ptnodePtr->buffer[0] * stg->StgLinkProbVec[i];		
			mincost += p * link->cost;	
			link->buffer[0] = p;	
			link->head->buffer[0] += p;
		}	

		if (lastNode)
			minNode->minShiftflow = minstgflow;
		if (stg->sflow / minNode->m_ptnodePtr->buffer[0] < minstgflow) 
			minstgflow = stg->sflow / minNode->m_ptnodePtr->buffer[0];
		lastNode = minNode;
	}
	node0->minShiftflow = minstgflow;
	//cout<<node0->m_ptnodePtr->buffer[0]<<endl;
	node0->m_ptnodePtr->buffer[0] = 0.0;
	
	lastNode = NULL;
	for (ig=rQ.begin();ig != rQ.end();ig++)
	{
		maxNode = *ig;
		//cout<<maxNode->m_ptnodePtr->id<<"->";
		if (maxNode->m_ptnodePtr->scanStatus==3
			&&  fabs(maxNode->m_ptnodePtr->buffer[0]-1.0) < 1e-8
			&&  fabs(maxNode->m_ptnodePtr->buffer[1]-1.0) < 1e-8
			)
		{
			flag = true;
			maxNode->m_ptnodePtr->buffer[1] = 0.0;
			sinknode = maxNode;
			sinknode->maxShiftflow = maxstgflow;
			break;
		}
		stg = maxNode->m_ptnodePtr->rStgElem->via;	
		maxcost += stg->waitT * maxNode->m_ptnodePtr->buffer[1];
		
		for (int i=0;i<stg->numofstglinks;i++)
		{
			int j = stg->StgLinkPosVec[i];
			link = maxNode->m_ptnodePtr->forwStar[j];
			floatType p = maxNode->m_ptnodePtr->buffer[1] * stg->StgLinkProbVec[i];		
			maxcost += p * link->cost;		
			link->buffer[1] = p;	
			link->head->buffer[1] += p;
		}
		if (lastNode)
			maxNode->maxShiftflow = maxstgflow;
		if ((stg->sflow / maxNode->m_ptnodePtr->buffer[1]) < maxstgflow) 
			maxstgflow = stg->sflow / maxNode->m_ptnodePtr->buffer[1];

		lastNode = maxNode;
	}
	#pragma endregion
	////cout<<endl;
	////cout<<"lcn:"<<node0->m_ptnodePtr->id<<",prob"<<node0->m_ptnodePtr->buffer[1]<<endl;

	#pragma region // cost recomputation according to flag
	//
	if (flag)
	{
		ig = find(Q.begin(), Q.end(),sinknode);
		if (ig!=Q.end())
		{
			while(ig!=Q.end())
			{
				minNode = *ig;
				minNode->m_ptnodePtr->scanStatus = 0;
				stg = minNode->m_ptnodePtr->StgElem->via;	
			    mincost -= stg->waitT * minNode->m_ptnodePtr->buffer[0];
				minNode->m_ptnodePtr->buffer[0] = 0.0;

				for (int i=0;i<stg->numofstglinks;i++)
				{
					int j = stg->StgLinkPosVec[i];
					link = minNode->m_ptnodePtr->forwStar[j];
					mincost -= link->buffer[0] * link->cost;	 	
					link->buffer[0] = 0.0;
				}	
				ig++;
			}
		}
		else
		{
			cout<<"can not find real common node in set Q, impossible"<<endl;
			system("PAUSE");
		}
	}
	else 
	{
		node0->m_ptnodePtr->buffer[1] = 0;
		node0->maxShiftflow = maxstgflow;
		sinknode = node0;
	}
	#pragma endregion

	#pragma region //calculate dervatives
	for(int i = 0;i<net->numOfLink;i++)
	{
		link =  net->linkVector[i];
		if (link->buffer[0] > 0 || link->buffer[1] > 0)
		{
			double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
			if (link->rLink)
			{
				tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
			}
			DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
		}
	}
	#pragma endregion

	dev = maxcost - mincost;
	
	if (dev>=0)
	{
		shift = __min(1.0 * dev /DerSum, sinknode->maxShiftflow);	
	}
	else
	{
		//shift = 0;
		shift = __max(1.0 * dev /DerSum, -sinknode->minShiftflow);	
	}

	//cout<<"maxcost:"<<maxcost<<",mincost:"<<mincost<<",dev:"<<dev<<",DerSum:"<<DerSum<<",shift:"<<shift<<",maxstgflow:"<<sinknode->maxShiftflow<<endl;
	#pragma region //shift flow on shortest hyperpath
	bool preflag=true;
	for (ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;
		minNode->m_ptnodePtr->scanStatus = 0;//reset node status
		if (minNode==sinknode) preflag=false;
		if (preflag)
		{
			stg = minNode->m_ptnodePtr->StgElem->via;	
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[j];
				link->volume +=  shift * link->buffer[0];
				if (link->volume<-1e-8)
				{
					cout<<"link flow is negative:"<<link->volume<<endl;
					system("PAUSE");
				}
				link->buffer[0] = 0.0;
				link ->UpdatePTLinkCost();
				link ->UpdatePTDerLinkCost();
				if (link ->rLink)
				{
					link ->rLink ->UpdatePTLinkCost();
					link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				}
			}	
			stg->sflow += minNode->m_ptnodePtr->buffer[0] * shift ;
			minNode->m_ptnodePtr->buffer[0] = 0.0;
		}
	}
	#pragma endregion

	preflag=true;
	#pragma region //shift flow on logest hyperpath
	for (ig=rQ.begin();ig != rQ.end();ig++)
	{
		maxNode = *ig;
		maxNode->status = 0;
		if (maxNode==sinknode) preflag=false;
		if (preflag)
		{

			stg = maxNode->m_ptnodePtr->rStgElem->via;
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				link = maxNode->m_ptnodePtr->forwStar[j];
				link->volume -=  shift * link->buffer[1];
				if (link->volume<-1e-8)
				{
					cout<<"link flow is negative:"<<link->volume<<endl;
					system("PAUSE");
				}
				link->buffer[1] = 0.0;
				link ->UpdatePTLinkCost();
				link ->UpdatePTDerLinkCost();
				if (link ->rLink)
				{
					link ->rLink ->UpdatePTLinkCost();
					link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				}
			}
			stg->sflow -= maxNode->m_ptnodePtr->buffer[1] * shift ;
			maxNode->m_ptnodePtr->buffer[1] = 0;
		}
	}
	#pragma endregion

	////cout<<"asas"<<endl;
	return fabs(dev);
	
}

floatType	PTDestination::StgNodeFlowShiftI(PTNode *node,PTNET *net)
{
	//PTRTRACE pt;
	//floatType dev =0.0,DerSum = 0.0;
	//StgLinks *stg;
	//multimap<int, GNODE*,less<int>>::iterator firstElem;
	//double maxcost = 0.0,mincost = 0.0,maxstgflow = POS_INF_FLOAT,minstgflow = POS_INF_FLOAT ,shift;
	//GNODE* node0 =  node->m_stgNode->m_ptnodeLCNPtr->m_stgNode;
	//GNODE *minNode,*maxNode,*headnode,*sinknode /*real LCN */;
	//GNODE *lastNode = NULL;
	//PTLink *link,*rlink;
	////set<TNM_PTLink*> AffectedLinks;
	//multimap<int, GNODE*,less<int>> Q;

	//#pragma region //calculate mincost for shortest hyperpath
	//Q.insert(pair<int, GNODE*>(- node->m_stgNode->m_tpLevel,  node->m_stgNode));
	//node->buffer[0] = 1.0;
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();	
	//	minNode = firstElem->second;
	//	//cout<<minNode->m_ptnodePtr->id<<"->";
	//	minNode->status = 0;// indicate whether in the queue
	//	Q.erase(firstElem);
	//	stg = minNode->m_ptnodePtr->m_StgVia;
	//	if (stg)
	//	{
	//		mincost += stg->waitT * minNode->m_ptnodePtr->buffer[0];
	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			floatType p = minNode->m_ptnodePtr->buffer[0] * sglink->m_prob;		
	//			mincost += p * link->cost;			
	//			link->buffer[0] = p;	
	//			link->head->buffer[0] += p;
	//			if (link->head->m_stgNode->status==0&&link->head->m_stgNode!=node0)
	//			{
	//				Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//				link->head->m_stgNode->status = 1;
	//				link->head->scanStatus = 3;
	//			}			
	//			sglink = sglink->revStgLink;
	//		}

	//		if (lastNode)
	//			minNode->minShiftflow = minstgflow;
	//		if (stg->sflow / minNode->m_ptnodePtr->buffer[0] < minstgflow) 
	//			minstgflow = stg->sflow / minNode->m_ptnodePtr->buffer[0];
	//		lastNode = minNode;
	//	}
	//	else
	//	{
	//		cout<<"Can not compute hyperpath link probability, since minstg does not exist from node"<<minNode->m_ptnodePtr->id<<endl;
	//		system("PAUSE");
	//	}
	//}
	////cout<<endl<<endl;;
	////cout<<node0->m_ptnodePtr->buffer[0] <<endl;
	//node0->m_ptnodePtr->buffer[0] = 0; // reset the LCN prob
	//node0->minShiftflow = minstgflow;
	//#pragma endregion

	//#pragma region //calculate maxcost for longest hyperpath
	//lastNode = NULL;
	//Q.insert(pair<int, GNODE*>(- node->m_stgNode->m_tpLevel,  node->m_stgNode));
	//node->buffer[1] = 1.0; //store the node prob on longest hyperpath
	//bool flag = false; // this indicate whether node0 is not the real LCN
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();
	//	maxNode = firstElem->second;
	//	//cout<<maxNode->m_ptnodePtr->id<<"->";
	//	maxNode->status = 0; //tag whether it in the queue
	//	Q.erase(firstElem);
	//	//cout<<maxNode->m_ptnodePtr->id<<"->";
	//	stg = maxNode->m_ptnodePtr->m_rStgVia;
	//	if (stg)
	//	{
	//		//if  (node->id==7) 
	//		//	cout<<"dest:"<<destination->id<<",tainode:"<<maxNode->m_ptnodePtr->id<<", stgflow:"<<stg->sflow<<endl;

	//		maxcost += stg->waitT * maxNode->m_ptnodePtr->buffer[1];

	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			floatType p = maxNode->m_ptnodePtr->buffer[1] * sglink->m_prob;		
	//			maxcost += p * link->cost;			
	//			link->buffer[1] = p;	
	//			link->head->buffer[1] += p;

	//			if (link->head->m_stgNode!=node0)
	//			{
	//				if (link->head->m_stgNode->status==0)
	//				{
	//					Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//					link->head->m_stgNode->status = 1;
	//				}

	//				if (link->head->scanStatus==3 && fabs(link->head->buffer[0]-1.0) < 1e-8 &&fabs(link->head->buffer[1]-1.0) < 1e-8)
	//				{
	//					if (Q.size()==1)
	//					{
	//						link->head->buffer[1] = 0.0;
	//						sinknode = link->head->m_stgNode; // this should be 
	//						flag =true;
	//						break;
	//					}
	//					else
	//					{
	//						cout<<"something wrong!"<<endl;
	//						system("PAUSE");
	//					}
	//				
	//				}
	//			}
	//			
	//			sglink = sglink->revStgLink;
	//		}
	//		if (lastNode)
	//			maxNode->maxShiftflow = maxstgflow;
	//		//if(lastNode&&net->curIter>4&&node->id==225)
	//		//	cout<<"id:"<<lastNode->m_ptnodePtr->id<<",maxstgflow:"<<lastNode->m_rStgVia->sflow/lastNode->m_ptnodePtr->buffer[1]<<", maxshiftflow:"<<maxstgflow<<",prob:"<<lastNode->m_ptnodePtr->buffer[1]<<endl;
	//		if ((stg->sflow / maxNode->m_ptnodePtr->buffer[1]) < maxstgflow) 
	//			maxstgflow = stg->sflow / maxNode->m_ptnodePtr->buffer[1];
	//		if (flag)
	//		{
	//			sinknode->maxShiftflow = maxstgflow;
	//			break;
	//		}
	//		lastNode = maxNode;

	//	}
	//	else
	//	{
	//		cout<<"Can not compute hyperpath link probability, since maxstg does not exist from node"<<maxNode->m_ptnodePtr->id<<endl;
	//		system("PAUSE");
	//	}
	//}

	////cout<<endl;

	//#pragma endregion

	//#pragma region // cost recomputation according to flag
	//if (flag)
	//{
	//	while(!Q.empty())
	//	{
	//		firstElem = Q.begin();
	//		minNode = firstElem->second;
	//		minNode->status = 0;
	//		minNode->m_ptnodePtr->scanStatus = 0; //reset the status 
	//		Q.erase(firstElem);
	//		stg = minNode->m_ptnodePtr->m_StgVia;
	//		
	//		mincost -= stg->waitT * minNode->m_ptnodePtr->buffer[0];
	//		
	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			mincost -= link->buffer[0] * link->cost;	
	//			if (link->head->m_stgNode->status == 0 &&  link->head->m_stgNode!= node0)
	//			{
	//				Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//				link->head->scanStatus = 1;
	//			}
	//			link->buffer[0] = 0.0;
	//			sglink = sglink->revStgLink;
	//		}
	//		minNode->m_ptnodePtr->buffer[0] = 0.0;
	//	}
	//	node0->m_ptnodePtr->buffer[0] = 0;	
	//}
	//else 
	//{
	//	node0->m_ptnodePtr->buffer[1] = 0;
	//	node0->maxShiftflow = maxstgflow;
	//	sinknode = node0;
	//}
	//#pragma endregion

	////if (destination->id==11&&node->id==5)
	////{
	////	cout<<endl;
	////	cout<<flag<<endl;
	////}
	//#pragma region //calculate dervatives
	//for(int i = 0;i<net->numOfLink;i++)
	//{
	//	link =  net->linkVector[i];
	//	if (link->buffer[0] > 0 || link->buffer[1] > 0)
	//	{
	//		//if (destination->id==11&&node->id==5)
	//		//	cout<<link->buffer[0]<<","<<link->buffer[1]<<endl;
	//		double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
	//		if (link->rLink)
	//		{
	//			tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
	//		}
	//		DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
	//	}
	//}
	//#pragma endregion

	//dev = maxcost - mincost;
	//
	//if (dev>=0)
	//{
	//	shift = __min(1.0 * dev /DerSum, sinknode->maxShiftflow);	
	//}
	//else
	//{
	//	//shift = 0;
	//	shift = __max(1.0 * dev /DerSum, -sinknode->minShiftflow);	
	//}
	//	//
	////if (net->curIter>4&&node->id==225) 
	////cout<<"maxcost:"<<maxcost<<",mincost:"<<mincost<<",dev:"<<dev<<",DerSum:"<<DerSum<<",shift:"<<shift<<",maxstgflow:"<<sinknode->maxShiftflow<<endl;
	//#pragma region //shift flow on shortest hyperpath
	//Q.insert(pair<int, GNODE*>(- node->m_stgNode->m_tpLevel, node->m_stgNode));
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();	
	//	minNode = firstElem->second;
	//	minNode->status = 0;// indicate whether in the queue
	//	minNode->m_ptnodePtr->scanStatus = 0;


	//	Q.erase(firstElem);
	//	stg = minNode->m_ptnodePtr->m_StgVia;

	//	GLINK* sglink = stg->StgLinkVia;
	//
	//	while(sglink)
	//	{
	//		link = sglink->m_linkPtr;
	//		if (link->head->m_stgNode->status == 0 && link->head->m_stgNode != sinknode)
	//		{
	//			Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//			link->head->m_stgNode->status  = 1;
	//		}
	//		link->volume +=  shift * link->buffer[0];
	//		if (link->volume<-1e-8)
	//		{
	//			cout<<"link flow is negative:"<<link->volume<<endl;
	//			system("PAUSE");
	//		}
	//		link->buffer[0] = 0.0;
	//		link ->UpdatePTLinkCost();
	//		link ->UpdatePTDerLinkCost();
	//		if (link ->rLink)
	//		{
	//			link ->rLink ->UpdatePTLinkCost();
	//			link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
	//		}
	//		sglink = sglink->revStgLink;
	//	}
	//	stg->sflow += minNode->m_ptnodePtr->buffer[0] * shift ;
	//	minNode->m_ptnodePtr->buffer[0] = 0.0;
	//}
	//#pragma endregion

	////cout<<"reset maxnode:"<<endl;
	//#pragma region //shift flow on longest hyperpath
	//Q.insert(pair<int, GNODE*>(- node->m_stgNode->m_tpLevel, node->m_stgNode));
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();	
	//	maxNode = firstElem->second;
	//	maxNode->status = 0;// indicate whether in the queue
	//	Q.erase(firstElem);
	//	stg = maxNode->m_ptnodePtr->m_rStgVia;
	//	GLINK* sglink = stg->StgLinkVia;
	//	
	//	while(sglink)
	//	{
	//		link =  sglink->m_linkPtr;
	//		if (link->head->m_stgNode->status == 0 && link->head->m_stgNode != sinknode)
	//		{
	//			Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//			link->head->m_stgNode->status  = 1;
	//		}
	//		link->volume -=  shift * link->buffer[1];
	//		if (link->volume<-1e-8)
	//		{
	//			cout<<"link flow is negative:"<<link->volume<<endl;
	//			system("PAUSE");
	//		}
	//		if (link->volume<1e-10) link->volume = 0.0;
	//		link->buffer[1] = 0.0;
	//		link ->UpdatePTLinkCost();
	//		link ->UpdatePTDerLinkCost();
	//		if (link ->rLink)
	//		{
	//			link ->rLink ->UpdatePTLinkCost();
	//			link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
	//		}
	//		sglink = sglink->revStgLink;
	//	}

	//	stg->sflow -= maxNode->m_ptnodePtr->buffer[1] * shift ;
	//	maxNode->m_ptnodePtr->buffer[1] = 0;
	//	
	//}
	//#pragma endregion
	//
	//return dev;
return 0.0;
}

floatType PTDestination::StgNodeFlowShiftII(PTNode *node,PTNET *net)
{
	PTLink *link;
	StgLinks *stg;
	PTNode* node0 = node->m_stgNode->m_ptnodeLCNPtr;
	GNODE *minNode,*maxNode,*lastNode,*sinknode;
	multimap<int, GNODE*,less<int>>::iterator firstElem;
	multimap<int, GNODE*,less<int>> Q;
	floatType dev =0.0,DerSum = 0.0;
	//double maxcost = 0.0,mincost = 0.0,maxstgflow = POS_INF_FLOAT,minstgflow = POS_INF_FLOAT ,shift;
	//#pragma region //calculate mincost for shortest hyperpath
	//Q.insert(pair<int, GNODE*>(- node->m_stgNode->m_tpLevel,node->m_stgNode));
	//node->buffer[0] = 1.0;
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();	
	//	minNode = firstElem->second;
	//	//cout<<minNode->m_ptnodePtr->id<<"->";
	//	minNode->status = 0;// indicate whether in the queue
	//	Q.erase(firstElem);
	//	stg = minNode->m_ptnodePtr->m_StgVia;
	//	if (stg)
	//	{
	//		mincost += stg->waitT * minNode->m_ptnodePtr->buffer[0];
	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			floatType p = minNode->m_ptnodePtr->buffer[0] * sglink->m_prob;		
	//			mincost += p * link->cost;			
	//			link->buffer[0] = p;	
	//			link->head->buffer[0] += p;
	//			if (link->head->m_stgNode->status == 0 &&link->head != node0)
	//			{
	//				Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//				link->head->m_stgNode->status = 1;
	//				link->head->scanStatus = 3;
	//			}

	//			sglink = sglink->revStgLink;
	//		}
	//		if (lastNode)
	//			minNode->minShiftflow = minstgflow;
	//		if (stg->sflow / minNode->m_ptnodePtr->buffer[0] < minstgflow) 
	//			minstgflow = stg->sflow / minNode->m_ptnodePtr->buffer[0];
	//		lastNode = minNode;
	//	}
	//	else
	//	{
	//		cout<<"Can not compute hyperpath link probability, since minstg does not exist from node"<<minNode<<endl;
	//		system("PAUSE");
	//	}
	//}
	//node0->buffer[0] = 0;// reset the LCN prob
	//node0->m_stgNode->minShiftflow = minstgflow;
	//#pragma endregion

	//#pragma region //calculate maxcost for longest hyperpath
	//lastNode = NULL;
	//Q.insert(pair<int, GNODE*>(-node->m_stgNode->m_tpLevel,node->m_stgNode));
	//node->buffer[1] = 1.0; //store the node prob on longest hyperpath
	//bool flag = false; // this indicate whether node0 is not the real LCN

	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();
	//	maxNode = firstElem->second;
	//	//cout<<maxNode->m_ptnodePtr->id<<"->";
	//	maxNode->status = 0; //tag whether it in the queue
	//	Q.erase(firstElem);
	//	//cout<<maxNode->m_ptnodePtr->id<<"->";
	//	stg = maxNode->m_ptnodePtr->m_rStgVia;
	//	if (stg)
	//	{
	//		maxcost += stg->waitT * maxNode->m_ptnodePtr->buffer[1];
	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			floatType p = maxNode->m_ptnodePtr->buffer[1] * sglink->m_prob;		
	//			maxcost += p * link->cost;			
	//			link->buffer[1] = p;	
	//			link->head->buffer[1] += p;
	//			if (link->head!=node0)
	//			{
	//				if (link->head->m_stgNode->status==0)
	//				{
	//					Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//					link->head->m_stgNode->status = 1;
	//				}
	//				if (link->head->scanStatus==3 && fabs(link->head->buffer[0]-1.0) < 1e-8 &&fabs(link->head->buffer[1]-1.0) < 1e-8)
	//				{
	//					if (Q.size()==1)
	//					{
	//						link->head->buffer[1] = 0.0;
	//						sinknode = link->head->m_stgNode; // this should be 
	//						flag =true;
	//						break;
	//					}
	//					else
	//					{
	//						cout<<"something wrong!"<<endl;
	//						system("PAUSE");
	//					}

	//				}
	//			}
	//			sglink = sglink->revStgLink;
	//		}
	//		if (lastNode)
	//			maxNode->maxShiftflow = maxstgflow;
	//		if ((stg->sflow / maxNode->m_ptnodePtr->buffer[1]) < maxstgflow) 
	//			maxstgflow = stg->sflow / maxNode->m_ptnodePtr->buffer[1];
	//		if (flag)
	//		{
	//			sinknode->maxShiftflow = maxstgflow;
	//			break;
	//		}
	//		lastNode = maxNode;

	//	}
	//	else
	//	{
	//		cout<<"Can not compute hyperpath link probability, since maxstg does not exist from node"<<maxNode<<endl;
	//		system("PAUSE");
	//	}
	//}

	//#pragma endregion

	//#pragma region // cost recomputation according to flag
	//if (flag)
	//{
	//	while(!Q.empty())
	//	{
	//		firstElem = Q.begin();
	//		minNode = firstElem->second;
	//		minNode->status = 0;
	//		minNode->m_ptnodePtr->scanStatus = 0; //reset the status 
	//		Q.erase(firstElem);
	//		stg = minNode->m_ptnodePtr->m_StgVia;
	//		
	//		mincost -= stg->waitT * minNode->m_ptnodePtr->buffer[0];
	//		
	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			mincost -= link->buffer[0] * link->cost;	
	//			if (link->head->m_stgNode->status == 0 &&  link->head!= node0)
	//			{
	//				Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//				link->head->scanStatus = 1;
	//			}
	//			link->buffer[0] = 0.0;
	//			sglink = sglink->revStgLink;
	//		}
	//		minNode->m_ptnodePtr->buffer[0] = 0.0;
	//	}
	//	node0->buffer[0] = 0;	
	//}
	//else 
	//{
	//	node0->buffer[1] = 0;
	//	node0->m_stgNode->maxShiftflow = maxstgflow;
	//	sinknode = node0->m_stgNode;
	//}
	//#pragma endregion

	//#pragma region //calculate dervatives
	//for(int i = 0;i<net->numOfLink;i++)
	//{
	//	link =  net->linkVector[i];
	//	if (link->buffer[0] > 0 || link->buffer[1] > 0)
	//	{
	//		//if (destination->id==11&&node->id==5)
	//		//	cout<<link->buffer[0]<<","<<link->buffer[1]<<endl;
	//		double tmp = link->pfdcost * (link->buffer[1] - link->buffer[0]);
	//		if (link->rLink)
	//		{
	//			tmp += link->rpfdcost * (link->rLink->buffer[1] - link->rLink->buffer[0]);
	//		}
	//		DerSum += tmp *(link->buffer[1] - link->buffer[0]);	
	//	}
	//}
	//#pragma endregion

	//dev = maxcost - mincost;
	//
	//if (dev>=0)
	//{
	//	shift = __min(1.0 * dev /DerSum, sinknode->maxShiftflow);	
	//}
	//else
	//{
	//	//shift = 0;
	//	shift = __max(1.0 * dev /DerSum, -sinknode->minShiftflow);	
	//}
	////cout<<"maxcost:"<<maxcost<<",mincost:"<<mincost<<",dev:"<<dev<<",DerSum:"<<DerSum<<",shift:"<<shift<<",maxstgflow:"<<sinknode->maxShiftflow<<endl;
	//#pragma region //shift flow on shortest hyperpath
	//Q.insert(pair<int, GNODE*>(-node->m_stgNode->m_tpLevel,node->m_stgNode));
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();	
	//	minNode = firstElem->second;
	//	minNode->status = 0;// indicate whether in the queue
	//	minNode->m_ptnodePtr->scanStatus = 0;
	//	Q.erase(firstElem);
	//	stg = minNode->m_ptnodePtr->m_StgVia;
	//	GLINK* sglink = stg->StgLinkVia;
	//	while(sglink)
	//	{
	//		link =  sglink->m_linkPtr;
	//		if (link->head->m_stgNode->status == 0 && link->head->m_stgNode != sinknode)
	//		{
	//			Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//			link->head->m_stgNode->status  = 1;
	//		}
	//		link->volume +=  shift * link->buffer[0];
	//		if (link->volume<-1e-8)
	//		{
	//			cout<<"link flow is negative:"<<link->volume<<endl;
	//			system("PAUSE");
	//		}
	//		link->buffer[0] = 0.0;
	//		link ->UpdatePTLinkCost();
	//		link ->UpdatePTDerLinkCost();
	//		if (link ->rLink)
	//		{
	//			link ->rLink ->UpdatePTLinkCost();
	//			link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
	//		}
	//		sglink = sglink->revStgLink;
	//	}
	//	stg->sflow += minNode->m_ptnodePtr->buffer[0] * shift ;
	//	minNode->m_ptnodePtr->buffer[0] = 0.0;
	//}
	//#pragma endregion

	//#pragma region //shift flow on longest hyperpath
	//Q.insert(pair<int, GNODE*>(-node->m_stgNode->m_tpLevel,node->m_stgNode));
	//while(!Q.empty())
	//{
	//	firstElem = Q.begin();	
	//	maxNode = firstElem->second;
	//	maxNode->status = 0;// indicate whether in the queue
	//	Q.erase(firstElem);
	//	stg = maxNode->m_ptnodePtr->m_rStgVia;
	//	GLINK* sglink = stg->StgLinkVia;
	//	while(sglink)
	//	{
	//		link = sglink->m_linkPtr;
	//		if (link->head->m_stgNode->status == 0 && link->head->m_stgNode != sinknode)
	//		{
	//			Q.insert(std::pair<int, GNODE*>(-link->head->m_stgNode->m_tpLevel, link->head->m_stgNode));
	//			link->head->m_stgNode->status  = 1;
	//		}
	//		link->volume -=  shift * link->buffer[1];
	//		if (link->volume<-1e-8)
	//		{
	//			cout<<"link flow is negative:"<<link->volume<<endl;
	//			system("PAUSE");
	//		}
	//		if (link->volume<1e-10) link->volume = 0.0;
	//		link->buffer[1] = 0.0;
	//		link ->UpdatePTLinkCost();
	//		link ->UpdatePTDerLinkCost();
	//		if (link ->rLink)
	//		{
	//			link ->rLink ->UpdatePTLinkCost();
	//			link ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
	//		}
	//		sglink = sglink->revStgLink;
	//	}
	//	stg->sflow -= maxNode->m_ptnodePtr->buffer[1] * shift ;
	//	maxNode->m_ptnodePtr->buffer[1] = 0;
	//}
	//#pragma endregion

	//return dev;
return 0.0;
}

int PTDestination:: ExpandStgSubNet(PTNET *net)
{
	m_expanded = false;
	int n = 0; // store expanded number of strategys
	vector<GNODE*>::iterator pv;
	GNODE  *gnode;
	PTNode *node;
	PTLink* plink;
	PTRTRACE pt;
	//bool loopfree =false;
	//if (net->RGapIndicator<1e-8) loopfree = true;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		gnode = *pv;
		node = gnode->m_ptnodePtr;
		if (node->GetTransitNodeType() != PTNode::TRANSFER)
		{
			for (pt = node->forwStar.begin(); pt != node->forwStar.end(); pt++)
			{
				plink = *pt;
				if (plink->head->m_stgNode // this ensure head node on the bush
					&& plink->markStatus == 0 //this ensure the link is not on the bush
					&& node->StgElem->cost  > plink->head->StgElem->cost + plink->cost
					&& node->rStgElem->cost  > plink->head->rStgElem->cost + plink->cost 
					)
				{
					StgLinks* newstg = net->GenerateNonBoardingStg(plink);
					gnode->m_StgsVec.push_back(newstg);				
					plink->markStatus += 1;
					m_expanded = true;
					//cout<<"add transit  strategy for tailnode:"<<gnode->m_ptnodePtr->id<<", stg name:"<<newstg->sname<<" with cost:"<<plink->head->m_cost + plink->cost<<" less than:"<< node->m_cost<<endl;
					n++;
					node->StgElem->cost = plink->head->StgElem->cost + plink->cost;
				}
			}
		}
		else
		{
			vector<PTLink*> candidatelinks;
			bool attractive = false;
			for (pt = node->forwStar.begin(); pt != node->forwStar.end(); pt++)
			{
				plink = *pt;
				if (plink->head->m_stgNode) // this ensure head node on the bush
				{
					if( plink->GetTransitLinkType() != PTLink::ABOARD 
					&& plink->markStatus == 0 //this ensure the link is not on the bush
					&& node->StgElem->cost  > plink->head->StgElem->cost + plink->cost
					&& node->rStgElem->cost  > plink->head->rStgElem->cost + plink->cost
					)
					{
						StgLinks* newstg = net->GenerateNonBoardingStg(plink);
						gnode->m_StgsVec.push_back(newstg);
						//cout<<"add walking strategy for tailnode:"<<gnode->m_ptnodePtr->id<<", stg name:"<<newstg->sname<<endl;
						plink->markStatus += 1;
						m_expanded = true;
						n++;
						node->StgElem->cost = plink->head->StgElem->cost + plink->cost;

					}

					if (plink->GetTransitLinkType() == PTLink::ABOARD 
						&& ((node->rStgElem->cost  > plink->head->rStgElem->cost + plink->cost  && plink->markStatus ==0 ) || plink->markStatus >0)
						)
					{
						//linkids.insert(pair<double, int>(plink->head->m_cost + plink->cost, plink->id));
						//plines.insert(pair<double, PTLink*>(plink->head->m_cost + plink->cost, plink));		
						candidatelinks.push_back(plink);						
						plink->tmpuse = plink->head->StgElem->cost + plink->cost;
						if (plink->head->StgElem->cost + plink->cost<node->StgElem->cost)  
							attractive =true;
					}
				}

			}
			//if (linkids.size()>0 && attractive)
			//if (plines.size()>0 && attractive)
			if (candidatelinks.size()>0 && attractive)
			{
				clock_t t4 = clock();
				vector<PTLink*> attlinks;
				floatType w,c;
				net->GenerateBoardingStg(candidatelinks,attlinks,c,w);
				//StgLinks* newstg = net->GenerateBoardingStg(linkids);	
				//StgLinks* newstg = net->GenerateBoardingStg(plines);	
				//StgLinks* newstg = net->GenerateBoardingStg(candidatelinks);
				//if (newstg->sname!= newstg1->sname|| newstg1->sname!= newstg2->sname)
				//{
				//	cout<<"linkids,  stg:"<<newstg->sname<<", cost:"<<newstg->cost<<",size:"<<linkids.size()<<", prob:"<<newstg->approach<<endl;
				//	cout<<"plines,  stg:"<<newstg1->sname<<", cost:"<<newstg1->cost<<",size:"<<plines.size()<<", prob:"<<newstg1->approach<<endl;
				//	cout<<"vector,  stg:"<<newstg2->sname<<", cost:"<<newstg2->cost<<",size:"<<candidatelinks.size()<<", prob:"<<newstg2->approach<<endl;
				//	
				//}
				string name = "";
				for(int i = 0;i<attlinks.size();i++)
				{
					name += std::to_string(attlinks[i]->fID) + "-";
				}
				//net->tt += clock() - t4;
				if (c < node->StgElem->cost && !gnode->SearchStgbyName(name))
				{
					StgLinks* newstg = new StgLinks;
					newstg->waitT = w;					
					vector<floatType> vpro;
					vector<int> vpoi;
					
					for(int i = 0;i<attlinks.size();i++)
					{
						vpro.push_back(attlinks[i]->m_probdata);				
						vpoi.push_back(attlinks[i]->fID);					
						if (attlinks[i]->markStatus==0) m_expanded = true;
						attlinks[i]->markStatus++;
					}
					newstg->SetLinkProbVec(vpro);
					newstg->StgLinkPosVec = vpoi;
					newstg->sname = name;
					gnode->m_StgsVec.push_back(newstg);	
					//cout<<"Destnation:"<<destination->id<<", add boarding strategy for tailnode:"<<gnode->m_ptnodePtr->id<<", stg name:"<<newstg->sname<<" with cost:"<<newstg->cost<<", less than:"<<node->m_cost<<endl;
					node->StgElem->cost = c;
						
					/*if (!attractive)
					{

						cout<<endl;
						cout<<"something wrong, stg name:"<<newstg->sname<<", cost:"<<newstg->cost<<endl;
						system("PAUSE");
					}*/
									
					
				}
			
			
			}		
		}
	}

	if(m_expanded)
	{	
		//cout<<"stg expanded"<<endl;
		if(!StgTopologOrder(net)) return -1;
	}
	return n;
	
}

double PTDestination::UpdateStgPotential(bool iscontributing)
{
	double gap =0.0, tgap;
	PTNode *node;
	GNODE *tailnode;
	vector<GNODE*>::iterator pv;
	PTLink* plink;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		node = (*pv)->m_ptnodePtr;
		node->StgElem->cost = POS_INF_FLOAT;//store the shortest cost
		node->rStgElem->cost =  -POS_INF_FLOAT;// store the longest cost
		node->StgElem->via = NULL;
		node->rStgElem->via = NULL;
	}
	destination->StgElem->cost = 0.0;
	destination->rStgElem->cost = 0.0;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		tailnode = (*pv);
		int nn = 0;
		for(vector<StgLinks*>::iterator it = tailnode->m_StgsVec.begin(); it!= tailnode->m_StgsVec.end();it++)
		{
			StgLinks* stg = *it;	
			double rstgcost = stg->waitT;
			double stgcost = stg->waitT;
			double maxS = -1.0;
			bool flag = true; // ensure head node stg have positive flow
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int ix = stg->StgLinkPosVec[i];
				PTLink* plink = tailnode->m_ptnodePtr->forwStar[ix];
				double tmp = plink->cost + plink->head->rStgElem->cost;

				stgcost += stg->StgLinkProbVec[i] *   (plink->cost + plink->head->StgElem->cost) ;
				rstgcost += stg->StgLinkProbVec[i] *   tmp ;
			
				if (tmp > maxS) maxS = tmp;

				if (plink->head->rStgElem->cost<0)	flag =false;

			}
			if ((maxS -  rstgcost) >0  && stg->waitT > 0 && iscontributing  ) // must be a boarding stg,
			{
				rstgcost = maxS;
			}
			if (rstgcost >tailnode->m_ptnodePtr->rStgElem->cost  
				&& ((stg->sflow > 0 && flag)|| iscontributing) )
			{
				tailnode->m_ptnodePtr->rStgElem->via = stg;
				tailnode->m_ptnodePtr->rStgElem->cost = rstgcost;
				//cout<<"update node:"<<tailnode->m_ptnodePtr->id<<", longest cost = "<<rstgcost<<endl;

			}		
			if (stgcost < tailnode->m_ptnodePtr->StgElem->cost )
			{
				tailnode->m_ptnodePtr->StgElem->via = stg;
				tailnode->m_ptnodePtr->StgElem->cost = stgcost;
				//cout<<"update node:"<<tailnode->m_ptnodePtr->id<<", shortest cost = "<<stgcost<<endl;
				//cout<<"update shortest via name:"<<tailnode->m_StgVia->sname<<", shortest cost = "<<stgcost<<endl;
			}
			nn++;
		}
		tgap = tailnode->m_ptnodePtr->rStgElem->cost - tailnode->m_ptnodePtr->StgElem->cost;
		if(tgap > gap) gap = tgap;

	}
	return gap;

	/*
	double gap =0.0, tgap;
	PTNode *node;
	GNODE *tailnode;
	vector<GNODE*>::iterator pv;
	PTLink* plink;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		node = (*pv)->m_ptnodePtr;
		node->m_cost = POS_INF_FLOAT;//store the shortest cost
		node->m_rcost =  -POS_INF_FLOAT;// store the longest cost
		node->m_StgVia = NULL;
		node->m_rStgVia = NULL;
	}
	destination->m_rcost = 0.0;
	destination->m_cost = 0.0;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		tailnode = (*pv);
		int nn = 0;
		for(vector<StgLinks*>::iterator it = tailnode->m_StgsVec.begin(); it!= tailnode->m_StgsVec.end();it++)
		{
			StgLinks* stg = *it;	
			double rstgcost = stg->waitT;
			double stgcost = stg->waitT;
			double maxS = -1.0;
			bool flag = true; // ensure head node stg have positive flow
			GLINK* sglink = stg->StgLinkVia;
			while(sglink)
			{
				plink =  sglink->m_linkPtr;
				

				rstgcost += sglink->m_prob *   (plink->cost + plink->head->m_rcost) ;
				stgcost += sglink->m_prob *   (plink->cost + plink->head->m_cost) ;
			
				if ((plink->cost + plink->head->m_rcost) > maxS) maxS = plink->cost + plink->head->m_rcost;

				if (plink->head->m_rcost<0)	flag =false;

				sglink = sglink->revStgLink;
			}

			if ((maxS -  rstgcost) >0  && stg->waitT > 0 && iscontributing  ) // must be a boarding stg,
			{
				rstgcost = maxS;
			}
			if (rstgcost >tailnode->m_ptnodePtr->m_rcost  
				&& ((stg->sflow > 0 && flag)|| iscontributing) )
			{
				tailnode->m_ptnodePtr->m_rStgVia = stg;
				tailnode->m_ptnodePtr->m_rcost = rstgcost;
				//cout<<"update node:"<<tailnode->m_ptnodePtr->id<<", longest cost = "<<rstgcost<<endl;

			}		
			if (stgcost < tailnode->m_ptnodePtr->m_cost )
			{
				tailnode->m_ptnodePtr->m_StgVia = stg;
				tailnode->m_ptnodePtr->m_cost = stgcost;
				//cout<<"update node:"<<tailnode->m_ptnodePtr->id<<", shortest cost = "<<stgcost<<endl;
				//cout<<"update shortest via name:"<<tailnode->m_StgVia->sname<<", shortest cost = "<<stgcost<<endl;
			}
			nn++;

		}
		tgap = tailnode->m_ptnodePtr->m_rcost - tailnode->m_ptnodePtr->m_cost;
		if(tgap > gap) gap = tgap;
	}
	return gap;
	*/
	
}

bool PTDestination::StgTopologOrder(PTNET *net)
{
	PTNode *node,*node0,*node1;
	PTRTRACE pv;
	PTLink* link;
	int zeroOut = 1; // number of nodes that not in the bush 
	for(int i = 0;i<net->numOfNode;i++)
	{	
		node = net->nodeVector[i];

		node->tmpNumOfIn = node->NumOfDestOutLink(); // store the number of outgoing links  
		if(node->tmpNumOfIn == 0 && node !=  destination)  //for such node, no orgnode associated except destination.
		{
			zeroOut ++;
		}
		else
		{
			node->m_stgNode->m_ptnodeLCNPtr = NULL;
			node->m_stgNode->m_tpLevel = 0;

			if(node->tmpNumOfIn > 1) node->m_stgNode->m_mutliStart = true;
			else node->m_stgNode->m_mutliStart = false;

			if (node->m_stgNode->m_StgsVec.size()>1) node->m_stgNode->m_mutliStg =true;
			else node->m_stgNode->m_mutliStg =false;
		}
	}
	//set topological order
	tplNodeVec.clear();
	queue<PTNode*> nq;
	nq.push(destination);
	tplNodeVec.push_back(destination->m_stgNode);
	int tL, count = 0;
	while(!nq.empty() && count<= net->numOfNode)
	{
		count ++;
		node= nq.front();
		nq.pop();
		for (pv = node->backStar.begin(); pv != node->backStar.end(); pv++)
		{
			link = *pv;
			PTNode* tailnode = link->tail;
			if(link->markStatus > 0)
			{
				tailnode->tmpNumOfIn--;
				if(tailnode->tmpNumOfIn == 0) 
				{
					nq.push(tailnode);
					tailnode->m_stgNode->m_tpLevel = node->m_stgNode->m_tpLevel + 1;
					tplNodeVec.push_back(tailnode->m_stgNode);
				}
			}
		
		}
	}
	// acyclicity check
	if(count<net->numOfNode - zeroOut + 1)
	{
		cout<<"\tFail to create topological order: network may contain cycles."<<endl; 
		system("PAUSE");
		return false;
	}
	//set LCN
	GNODE *dnode;
	vector<GNODE *>::iterator po;
	for(po = tplNodeVec.begin(); po != tplNodeVec.end(); po++)
	{
		dnode = *po;
		node = dnode->m_ptnodePtr;
		for(PTRTRACE pv1 = node->forwStar.begin(); pv1!=node->forwStar.end(); pv1++)
		{
			link = *pv1;
			if(link->markStatus >0)
			{
				if(!dnode->m_mutliStart) //single incoming node.
				{
					dnode->m_ptnodeLCNPtr  = link->head;//we actually store a link pointer but force it to a node.		
				}
				else
				{
					// obtain the common node that has the minimum toplogical distance 
					if(dnode->m_ptnodeLCNPtr== NULL)   //for the first link, just plug in its tail node					
					{
						dnode->m_ptnodeLCNPtr = link->head;
					}
					else
					{
						node0 = dnode->m_ptnodeLCNPtr;
						node1= link->head;
						while(node0 !=  node1) 
						{

							if(node0->m_stgNode->m_tpLevel ==  node1->m_stgNode->m_tpLevel)
							{
								node0 =  node0->m_stgNode->m_ptnodeLCNPtr;
								node1 =  node1->m_stgNode->m_ptnodeLCNPtr;
								
							}
							else
							{
								while( node0->m_stgNode->m_tpLevel >  node1->m_stgNode->m_tpLevel)
								{
									node0 = node0->m_stgNode->m_ptnodeLCNPtr;
								}
								while( node0->m_stgNode->m_tpLevel <  node1->m_stgNode->m_tpLevel)
								{
									node1 =  node1->m_stgNode->m_ptnodeLCNPtr;
								}
							}
						}
						dnode->m_ptnodeLCNPtr= node0;
					}
				
				
				}

			}

		}
	}
	return true;
}

void PTNET::StgNetInitialize()
{
	PTDestination* dest;
	PTOrg* org;
	for (int i = 0;i<numOfPTDest;i++)
	{
		dest = PTDestVector[i];
		InitializeHyperpathLS(dest->destination);
		dest->InitialStgSubNet(this);
	
	}
}

void PTDestination::RemarkStgLinksOnNet()
{
	GNODE *gnode;
	PTRTRACE pt;
	vector<GNODE *>::iterator po;
	for(po = tplNodeVec.begin(); po != tplNodeVec.end(); po++)
	{
		gnode = *po;

		for(pt = gnode->m_ptnodePtr->forwStar.begin(); pt != gnode->m_ptnodePtr->forwStar.end(); pt++)
		{
			PTLink* link = *pt;
			link->markStatus = 0;//reset number fo stgs that link belong to on the bush
			link->buffer[0] = 0;//shortest stg prob
			link->buffer[1] = 0;//longest stg prob			
		}

		 gnode->m_ptnodePtr->m_stgNode = NULL;
	}



}

void PTDestination::MarkStgLinksOnNet()
{
	GNODE *gnode;
	vector<GNODE *>::iterator po;

	for(po = tplNodeVec.begin(); po != tplNodeVec.end(); po++)
	{
		gnode = *po;
		gnode->m_ptnodePtr->m_stgNode = gnode;
		for (vector<StgLinks*>::iterator pt = gnode->m_StgsVec.begin();pt != gnode->m_StgsVec.end();pt++)
		{
			StgLinks* stg = *pt;
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int j = stg->StgLinkPosVec[i];
				PTLink* link = gnode->m_ptnodePtr->forwStar[j];
				link->markStatus++;
			}	
		}
	}

	//double wd = 0.0; //waiting delay
	//for(po = tplNodeVec.begin(); po != tplNodeVec.end(); po++)
	//{
	//	gnode = *po;
	//	gnode->m_ptnodePtr->m_stgNode = gnode;
	//	//cout<<gnode->m_ptnodePtr->id<<endl;
	//	for (vector<StgLinks*>::iterator pt = gnode->m_StgsVec.begin();pt != gnode->m_StgsVec.end();pt++)
	//	{
	//		StgLinks* stg = *pt;
	//		//cout<<stg->sname<<endl;
	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			sglink->m_linkPtr->markStatus ++;
	//			sglink= sglink->revStgLink;
	//		}
	//	}
	//}


}

void PTDestination::InitialStgSubNet(PTNET *net)
{
	
	tplNodeVec.clear();
	GNODE* gnode;
	PTNode* node;
	PTLink *link;
	for (int i = 0;i<net->numOfNode;i++)
	{
		node = net->nodeVector[i];
		if (node == destination || node->StgElem->vialink!=NULL)
		{
			gnode = new GNODE(node);
			node->m_stgNode = gnode;
			tplNodeVec.push_back(gnode);
		}
		if (node->StgElem->vialink!=NULL)
		{
			StgLinks* newstg = new StgLinks;
			newstg->waitT = node->m_wait;
			newstg->SetLinkProbVec(node->m_attProb);		
			link = node->StgElem->vialink;		
			while (link)
			{
				link->markStatus++;
				newstg->sname += std::to_string(link->fID) + "-";
				newstg->StgLinkPosVec.push_back(link->fID);		
				link = link->stglinkptr;
			}
			node->StgElem->via = newstg;
			gnode->m_StgsVec.push_back(newstg);
		}
	
	}
	
	vector<GNODE*>(tplNodeVec).swap(tplNodeVec);
	if(!StgTopologOrder(net)) return ;

	//assign all demand to the shortest strategy
	for (int j=0; j<numOfOrg; j++)
	{
		PTOrg* org= orgVector[j];	
		//if (!org->org->m_StgVia)
		//	cout<<destination->id<<","<<org->org->id<<endl;
		/*
		for (int i=0;i<net->numOfNode;i++) 
		{
			if (net->nodeVector[i]->scanStatus != 0 || net->nodeVector[i]->buffer[0]!=0)
			{
				cout<<"node id:"<<net->nodeVector[i]->id<<","<<net->nodeVector[i]->scanStatus<<","<< net->nodeVector[i]->buffer[0]<<endl;
			}
		}*/
		//cout<<endl<<"od:"<<org->org->id<<","<<destination->id<<endl;
		AssignInitialStgFlow(org->org,destination,org->assDemand);
	}
	RemarkStgLinksOnNet();
	
}

void PTDestination::AssignInitialStgFlow(PTNode* onode, PTNode* dnode, double assignDemand)
{
	StgLinks *stg;
	vector<GNODE*> Q;// this store the node on shortest hyperpath
	GNODE *minNode;
	PTLink *link;
	Q.push_back(onode->m_stgNode);
	int ix = 0;//node index in the vector
	while (ix<Q.size())
	{
		minNode = Q[ix];
		//cout<<minNode->m_ptnodePtr->id<<",";
		stg = minNode->m_ptnodePtr->StgElem->via;
		if (stg)
		{
			//cout<<stg->numofstglinks<<",";			
			for (int i=0;i<stg->numofstglinks;i++)
			{
				
				int j = stg->StgLinkPosVec[i];
				//cout<<i<<","<<stg->StgLinkPosVec.size()<<","<<stg->numofstglinks<<","<<j<<","<<minNode->m_ptnodePtr->forwStar.size()<<endl;
				link = minNode->m_ptnodePtr->forwStar[j];
				//cout<<link->id;
				if (link->head->scanStatus==0&&link->head!=dnode)
				{
					link->head->scanStatus = 3;
					Q.push_back(link->head->m_stgNode);				
				}		
			}

		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since stg does not exist from node"<<minNode->m_ptnodePtr->id<<endl;
			system("PAUSE");
		}
		ix++;
	}
	sort(Q.begin(),Q.end(),comp_gnode_level);

	
	onode->buffer[0] = 1.0;
	for (vector<GNODE*>::iterator ig=Q.begin();ig != Q.end();ig++)
	{
		minNode = *ig;	
		//cout<<minNode->m_ptnodePtr->id<<",";
		minNode->m_ptnodePtr->scanStatus = 0;//reset the status
		stg = minNode->m_ptnodePtr->StgElem->via;
		if (stg)
		{
			for (int i=0;i<stg->numofstglinks;i++)
			{
				int ix = stg->StgLinkPosVec[i];
				link = minNode->m_ptnodePtr->forwStar[ix];
				floatType p = minNode->m_ptnodePtr->buffer[0] * stg->StgLinkProbVec[i];
				link->volume += assignDemand * p;//assign the flow 
				link->UpdatePTLinkCost();
				link->UpdatePTDerLinkCost();
				if (link->rLink)
				{
					link->rLink->UpdatePTLinkCost();
					link->rLink->UpdatePTDerLinkCost();
				}
				link->head->buffer[0] += p;
			}
		}
		
		stg->sflow += assignDemand * minNode->m_ptnodePtr->buffer[0]; 

		minNode->m_ptnodePtr->buffer[0]= 0.0;
	}
	dnode->buffer[0] = 0.0;
	//cout<<endl;
	//{
	//	minNode = *ig;	
	//	stg = minNode->m_ptnodePtr->m_StgVia;
	//	mincost += stg->waitT * minNode->m_ptnodePtr->buffer[0];
	//	GLINK* sglink = stg->StgLinkVia;
	//	while(sglink)
	//	{
	//		link = sglink->m_linkPtr;
	//		floatType p = minNode->m_ptnodePtr->buffer[0] * sglink->m_prob;		
	//		mincost += p * link->cost;			
	//		link->buffer[0] = p;	
	//		link->head->buffer[0] += p;
	//		sglink = sglink->revStgLink;
	//	}


	//		GLINK* sglink = stg->StgLinkVia;
	//		while(sglink)
	//		{
	//			link = sglink->m_linkPtr;
	//			if (link->head->scanStatus==0&&link->head->m_stgNode!=node0)
	//			{
	//				link->head->scanStatus = 3;
	//				Q.push_back(link->head->m_stgNode);				
	//			}
	//			sglink = sglink->revStgLink;
	//		}
	//	}
	//	else
	//	{
	//		cout<<"Can not compute hyperpath link probability, since minstg does not exist from node"<<minNode->m_ptnodePtr->id<<endl;
	//		system("PAUSE");
	//	}
	//	ix++;
	//}





	/*
	//GNODE *gnode,*headnode;
	PTNode *node;
	PTLink* plink;
	StgLinks* stg;
	multimap<int, PTNode*,less<int>> Q;
	multimap<int, PTNode*,less<int>>::iterator firstElem;
	Q.insert(pair<int, PTNode*>(-onode->m_stgNode->m_tpLevel, onode));
	onode->scanStatus = 1;
	onode->buffer[0] = 1.0;
	while(!Q.empty())
	{
		firstElem = Q.begin();
		node = firstElem->second;
		node->scanStatus = 0;
		Q.erase(firstElem);
		stg = node->m_StgVia;
		if (stg)
		{
			GLINK* sglink = stg->StgLinkVia;
			while(sglink)
			{
				double p = sglink->m_prob * node->buffer[0];
				plink =  sglink->m_linkPtr;
				plink->volume += assignDemand * p;//assign the flow 
				plink->UpdatePTLinkCost();
				plink->UpdatePTDerLinkCost();
				if (plink->rLink)
				{
					plink->rLink->UpdatePTLinkCost();
					plink->rLink->UpdatePTDerLinkCost();
				}
				plink->head->buffer[0] += p;
				//if ( sglink->m_head == NULL)
				//{
				//	cout<<"tailnode:"<<node->id<<", stgname:"<<stg->sname<<",tail"<<sglink->m_tail->m_ptnodePtr->id<<", fail to have head stg node"<<endl;
				//}
				//cout<<sglink->m_head<<endl;
				if (plink->head->scanStatus == 0 && plink->head != dnode)
				{
					Q.insert(std::pair<int, PTNode*>(-plink->head->m_stgNode->m_tpLevel, plink->head));
					plink->head->scanStatus = 1;
				}
				sglink = sglink->revStgLink;
			}		
		}
		else
		{
			cout<<"Can not compute hyperpath link probability, since stg does not exist from node"<<node->id<<endl;
			system("PAUSE");
		}
		stg->sflow += assignDemand * node->buffer[0]; 
		//cout<<"stg:"<<stg->sname<<", add assign flow:"<<assignDemand * node->buffer[0]<<endl;
		node->buffer[0] = 0.0;
	}
	//cout<<dnode->buffer[0]<<endl;
	dnode->buffer[0]=0.0;
	*/
}

void PTDestination::UpdateStgSPTreeOnly()
{
	
	vector<GNODE*>::iterator pv;
	PTLink* plink;
	PTNode *node;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{	
		node = (*pv)->m_ptnodePtr;
		node->StgElem->cost = POS_INF_FLOAT;//store the shortest cost
		node->StgElem->via = NULL;
	}
	destination->StgElem->cost = 0;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		node = (*pv)->m_ptnodePtr;
		for (vector<StgLinks*>::iterator it = (*pv)->m_StgsVec.begin();it != (*pv)->m_StgsVec.end();it++)
		{
			StgLinks* stg = *it;
			double stgcost = stg->waitT;

			for (int i=0;i<stg->numofstglinks;i++)
			{
				int ix = stg->StgLinkPosVec[i];
				PTLink* plink = node->forwStar[ix];
				stgcost += stg->StgLinkProbVec[i] *   (plink->cost + plink->head->StgElem->cost) ;
			}
			stg->cost = stgcost;
			if (stgcost < node->StgElem->cost )
			{
				node->StgElem->via = stg;
				node->StgElem->cost = stgcost;
				//cout<<"update shortest via name:"<<tailnode->m_StgVia->sname<<", shortest cost = "<<stgcost<<endl;
			}
		}
	}
	



}

void PTDestination::TrimStgSubNet(PTNET *net)
{
	
	int n = 0;
	m_trimmed = false;
	UpdateStgSPTreeOnly();
	vector<GNODE*>::iterator pv;
	PTNode *tailnode;
	GNODE* gnode;
	for (pv = tplNodeVec.begin(); pv!=tplNodeVec.end(); pv++)
	{
		gnode = (*pv);
		tailnode = gnode->m_ptnodePtr;
		vector<StgLinks*>::iterator it = gnode->m_StgsVec.begin();
		while(it != gnode->m_StgsVec.end())
		{
			StgLinks* stg = *it;
			net->netTTwaitcost += stg->sflow * stg->waitT;
			//net->m_walkNum ++;
			if (stg->sflow==0 && stg!= tailnode->StgElem->via ) //   &&  && stg->cost - tailnode->m_cost >0
			{			
				for (int i=0;i<stg->numofstglinks;i++)
				{
					int ix = stg->StgLinkPosVec[i];
					tailnode->forwStar[ix]->markStatus--;
					if (tailnode->forwStar[ix]->markStatus == 0) m_trimmed = true;
				}
				it = gnode->m_StgsVec.erase(it);
				n++;
			}
			else it++;	
		}

	}
	
}


int	PTNET::SolveSDTEAP()
{
	SDInitializedCH();
	SDProjGap = 100; 
	ComputeConvGap();
	cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<endl;
	m_startRunTime = clock();
	if(!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))))
	{
		do
		{
			clock_t sclock = clock();
			SDMasterProblem();
			IterMainlooptime = 1.0 * (clock() - sclock)/CLOCKS_PER_SEC;		
			// solve sub-problem: SPP
			UpatePTNetworkLinkCost(); 
							
			ComputeConvGap();

			curIter ++;			
			numaOfHyperpath =c_h.size();// add the number of extreme point into the 'column of hyperpath' 
		
			RecordTEAPCurrentIter();
			
			if (!SDAddCurrentSolution2CH())
				break;
			cout<<"iter:"<<curIter<<",\tcurrent gap:"<<TNM_FloatFormat(GapIndicator,5,15)<<", relative gap:"<<TNM_FloatFormat(RGapIndicator,5,15)<<", extreme points:"<<c_h.size()<<endl;

			
		}while (!(ReachAccuracy(RGapIndicator) || ReachMaxIter() || ReachMaxTime((clock() - m_startRunTime)*1.0 / (CLOCKS_PER_SEC*60.0))));
	}
	cout<<"CPU TIME: "<<1.0*(clock() - m_startRunTime)<<endl;
	return 0;
}

void PTNET::SDInitializedCH()
{
	c_h.clear();
	UpatePTNetworkLinkCost();
	//initialize two extreme points
	PTAllOrNothing();
	
	SDAddCurrentSolution2CH();
	
	PTAllOrNothing();

	SDAddCurrentSolution2CH();
}

bool PTNET::SDAddCurrentSolution2CH()
{
	floatType* solution;
	bool flag; // the flag indicate there exist the same solution in the convex hull
	int ix=1;
	UpatePTNetworkLinkCost();
	
	flag = true;			
	//store a new extreme point
	solution = new floatType[numOfLink+1];// the last variable is the total waiting time 
	for(int i = 0;i<numOfLink;i++)
	{
		PTLink* link =  linkVector[i];
		solution[i] = link->volume;
	}
	solution[numOfLink] = netTTwaitcost;
	// check whether current solution is in the convex hull set 
	for (int i=0; i<c_h.size();i++)
	{
		floatType* h = c_h[i];
		bool isSame=true;
		for (int j =0;j<=numOfLink;j++)
		{
			if (solution[j] != h[j]) isSame=false;
		}

		if (isSame)
		{
			flag =false;
			cout<<"Find the same solution with "<<i<<" extreme point!"<<endl;
			break;
			//system("PAUSE");
		}
	}
	if (flag)  c_h.push_back(solution);

	return flag;
}

double ObjFunc2(unsigned n, const double *x, double *grad, void *data) 
{
	PTNET* net = (PTNET*) data;
	PTLink* link;
	double obj = 0.0;
	for(int i = 0;i<net->numOfLink;i++)
	{
		link =  net->linkVector[i];
		double tmpFlow = 0.0;
		
		for(int j=0;j<n;j++)
		{
			floatType* es = net->c_h[j];
			tmpFlow += x[j] * es[i];	
			//cout<<es[i]<<"\t";
		}
		//cout<<link->volume<<"\t"<<link->cost<<"\t"<<link->pfdcost<<endl;
		//double flow = link ->SCvolume;

		link->volume = link ->SCvolume;
		if (link->rLink) link->rLink->volume = link->rLink ->SCvolume;

		obj += tmpFlow * link->GetPTLinkCost() + (0.5 * link->GetPTLinkDerCost()* tmpFlow * tmpFlow - link->GetPTLinkDerCost() * link->volume * tmpFlow) * net->stepSize;

	}
	for(int j=0;j<n;j++)
	{
		floatType* es = net->c_h[j];
		obj += x[j] *  es[net->numOfLink];
	}
	//cout<<obj<<","<<net->stepSize<<endl;

	return obj;
}

double SimplexEqualityConstraint(unsigned int n, const double *x, double *grad, void *data) 
{
	double s = 0.0;

	for (int i=0;i<n;i++) s += x[i];

	return s - 1.0;
}

void PTNET::SDMasterProblem()
{
	//floatType VIvalue, oldGap = POS_INF_FLOAT;
	//double perr = POS_INF_FLOAT,err,MinVI = -100;
	//double tol = 1e-6;
	//double f_min1,f_min2;
	//PTLink* link;
	//int iters = 0;
	//int n = c_h.size(); // number of extreme points in the convex hull  
	//double* lb1 = new double[n];
	//double* ub1 = new double[n];
	//double* x = new double[n]; //the initial value for quadratic minimization

	//SDProjGap = 1e-8;
	//stepSize = 1.0;
	//for (int i=0;i<n;i++)
	//{
	//	lb1[i] = 0.0;
	//	ub1[i] = 1.0;
	//	if (i==0) x[i]=1.0;
	//	else x[i]=0.0;
	//}	
	//do
	//{
	//	iters++;
	//	// we first solve quadratic program with current solution
	//	nlopt_opt opter1 = nlopt_create(NLOPT_LN_COBYLA, n);

	//	nlopt_set_min_objective(opter1, ObjFunc2, this);// objective function
	//
	//	// lower and upper bound, and initialize solution
	//	nlopt_set_lower_bounds(opter1, lb1); 
	//	nlopt_set_upper_bounds(opter1, ub1);
	//	nlopt_set_maxtime(opter1,60);// in seconds
	//	nlopt_add_equality_constraint(opter1, SimplexEqualityConstraint, NULL, tol);// equality constraint

	//	nlopt_set_xtol_rel(opter1, tol);// stopping criterion
	//	nlopt_set_ftol_abs(opter1, tol);
	//	nlopt_set_force_stop(opter1, tol);

	//	nlopt_result result1 = nlopt_optimize(opter1, x, &f_min1);
	//	if (result1>0 || result1 ==-4)
	//	{
	//	}
	//	else
	//	{
	//		cout<<"something wrong:"<<result1<<endl;
	//		system("PAUSE");
	//	}
	//	nlopt_destroy(opter1);
	//	
	//	err = 0.0;
	//	// update iterative link flow
	//	for(int i = 0;i<numOfLink;i++)
	//	{
	//		link =  linkVector[i];
	//		double tmpFlow = 0.0;
	//		for(int j=0;j<n;j++)
	//		{
	//			tmpFlow += x[j] * c_h[j][i];	
	//		}
	//		err += pow(tmpFlow - link->SCvolume,2);
	//		link->SCvolume = tmpFlow;
	//		link->volume = tmpFlow;
	//	}
	//	//for(int i = 0;i<numOfLink;i++)
	//	//{
	//	//	link = (TNM_PTLink*) linkVector[i];
	//	//	cout<<"id:"<<link->id<<",volume:"<<link->volume<<endl;
	//	//}
	//	double tmpwait = 0.0;
	//	for(int j=0;j<n;j++)
	//	{
	//		tmpwait += x[j]* c_h[j][numOfLink];
	//	}
	//	err += pow(tmpwait - SCnetTTwaitcost,2);
	//	SCnetTTwaitcost = tmpwait;
	//	netTTwaitcost = tmpwait;


	//	oldGap = err;

	//	//cout<<"Minimum utility="<<TNM_FloatFormat(f_min1,5,15)<<". with "<<n<<" simplex combination"<<"\t\t";
	//}while(iters<100 && err > SDProjGap);

	//// remove extreme point such that the weight = 0 
	//int ix = 0;
	//multimap<floatType, floatType*,less<double>> OrderCHs;
	//for (vector<floatType*>::iterator it = c_h.begin(); it!=c_h.end();) 
	//{
	//	if (x[ix] == 0) it = c_h.erase(it);
	//	else 
	//	{
	//		OrderCHs.insert(pair<floatType, floatType*>(-x[ix], c_h[ix]));	
	//		it++;
	//	}
	//	ix ++;
	//}
}

void PTNET::InitialSubnetStgNode()
{
	PTDestination* dest;
	PTOrg* org;
	GNODE* gnode;
	PTNode* node;
	PTLink *link;
	for (int i = 0;i<numOfPTDest;i++)
	{
		dest = PTDestVector[i];
		InitializeHyperpathLS(dest->destination);

		for (int i = 0;i<numOfNode;i++)
		{
			node = nodeVector[i];
			if (node == dest->destination || node->StgElem->vialink!=NULL)
			{
				gnode = new GNODE(node);				
				dest->tplNodeVec.push_back(gnode);
			}
		}
	}

}

void PTNET::ExportStgUEsolution()
{

	if (PCTAE_ALG !=PCTAE_algorithm::PCTAE_B_DSB)
	{
		cout<<"Please call bush-based algs to write stg flow.";
		system("PAUSE");
	}
	string fileName  = networkName + ".stgue";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, fileName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .stgsolution file to write  Information!"<<endl;
		return;
	}
	cout<<"\tWriting stg solution into "<<fileName<<" stg solution!"<<endl;

	netTTwaitcost = 0.0;
	//cout<<"numOfNode:"<<numOfNode<<endl;
	for(int i=0;i<numOfNode;i++)
	{
		PTNode* node = nodeVector[i];
		outfile<<"Node:"<<node->id<<endl;
		//cout<<"Node:"<<node->id<<endl;
		for (int j = 0;j<numOfPTDest;j++)
		{		
			PTDestination* dest = PTDestVector[j];
			dest->MarkStgLinksOnNet();
			ostringstream line("");
			line<<"Dest:"<<dest->destination->id<<";";
			if (node->m_stgNode)
			{
				for (vector<StgLinks*>::iterator pt = node->m_stgNode->m_StgsVec.begin();pt != node->m_stgNode->m_StgsVec.end();pt++)
				{
					StgLinks* stg = *pt;
					line<<stg->sname<<","<<TNM_FloatFormat(stg->sflow,15,10)<<";";
					netTTwaitcost += stg->sflow * stg->waitT;
				}
			}
			string li = line.str().substr(0,line.str().size()-1);
			outfile<<li<<endl;
			//cout<<li<<endl;
			dest->RemarkStgLinksOnNet();
		}
	
	}

	outfile<<"ttwait:"<<TNM_FloatFormat(netTTwaitcost,15,10)<<endl;

	outfile.close();
}

void PTNET::ExportStgUEsolutionII()
{

	if (PCTAE_ALG !=PCTAE_algorithm::PCTAE_B_DSB)
	{
		cout<<"Please call bush-based algs to write stg flow.";
		system("PAUSE");
	}
	string fileName  = networkName + ".stgue";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, fileName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .stgsolution file to write  Information!"<<endl;
		return;
	}
	cout<<"\tWriting stg solution into "<<fileName<<" stg solution!"<<endl;

	netTTwaitcost = 0.0;
	//cout<<"numOfNode:"<<numOfNode<<endl;

	for (int i = 0;i<numOfPTDest;i++)
	{		
		PTDestination* dest = PTDestVector[i];
		dest->MarkStgLinksOnNet();
		outfile<<"Dest:"<<dest->destination->id<<endl;
		for(int j=0;j<numOfNode;j++)
		{
			PTNode* node = nodeVector[j];
			ostringstream line("");
			line<<"Node:"<<node->id<<";";
			if (node->m_stgNode)
			{
				for (vector<StgLinks*>::iterator pt = node->m_stgNode->m_StgsVec.begin();pt != node->m_stgNode->m_StgsVec.end();pt++)
				{
					StgLinks* stg = *pt;
					line<<stg->sname<<","<<TNM_FloatFormat(stg->sflow,18,12)<<";";
					netTTwaitcost += stg->sflow * stg->waitT;
				}
			
			}
			string li = line.str().substr(0,line.str().size()-1);
			outfile<<li<<endl;
		
		}
		dest->RemarkStgLinksOnNet();
	}

	outfile<<"ttwait:"<<TNM_FloatFormat(netTTwaitcost,18,12)<<endl;

	outfile.close();

}

StgLinks*	PTNET::GenerateStgByName(PTNode* tnode,string name)
{
	StgLinks* newstg = new StgLinks;
	vector<string> words;
	vector<floatType> probs;
	newstg->sname = name;
	TNM_GetWordsFromLine(name.substr(0,name.size()-1), words, '-', '"');
	int fid;
	double ttfreq = 0.0;
	PTLink* link;
	bool isboarding = false;
	for(int i=0;i<words.size();i++)
	{
		TNM_FromString(fid, words[i], std::dec);
		link = tnode->forwStar[fid];
		if (link->GetTransitLinkType()==PTLink::ABOARD)
		{
			ttfreq += 1.0/ link->m_hwmean;
			isboarding = true;
		}
	
		newstg->StgLinkPosVec.push_back(fid);	
	}

	for(int i=0;i<words.size();i++)
	{
		TNM_FromString(fid, words[i], std::dec);
		link = tnode->forwStar[fid];
		if (isboarding)
		{
			probs.push_back((1.0/link->m_hwmean)/ttfreq);		
		}
		else
		{
			probs.push_back(1.0);
		}
	}
	if (isboarding)
	{
		newstg->waitT =  1.0/ttfreq;
	}
	else newstg->waitT = 0;

	newstg->SetLinkProbVec(probs);		
	return newstg;


}

bool	PTNET::ImportStgUESolutionII()
{
	
	netTTwaitcost = 0.0;
	InitialSubnetStgNode();
	string fileName  = networkName + ".stgue";
	ifstream infile;
    if(!TNM_OpenInFile(infile, fileName))
    {
		cout<<"\t cannot read file"<<fileName<<" to read"<<endl;
        return false;
    }
	vector<string> words,iwords;
	string pline;
	int nid;
	int dix,did;
	PTNode* node;
	PTDestination* dest;
	GNODE* gnode;
	floatType ttwait;
	while(getline(infile, pline))
    {
		//cout<<pline<<endl;
        if(!pline.empty())//skip an empty line
        {
			if (pline.substr(0,4)=="Dest")
			{
				TNM_GetWordsFromLine(pline, words, ':', '"');
				TNM_FromString(did, words[1], std::dec);
				dest = CatchDestPtr(did);
				if (!dest)
				{
					cout<<"Does not exists dest:"<<did<<endl;
					return false;
				}			
				dest->m_trimmed = true; //this ensure to reset topologic order in the mainloop
			}
			else if  (pline.substr(0,4)=="Node")
			{
				TNM_GetWordsFromLine(pline, words, ';', ' ');
				TNM_GetWordsFromLine(words[0], iwords, ':', ' ');
				TNM_FromString(nid, iwords[1], std::dec);
				node = CatchNodePtr(nid);
				if (!node)
				{
					cout<<"Does not exists node:"<<nid<<endl;
					return false;
				}
				if (words.size()>1)
				{
					for(int i=1;i<words.size();i++)
					{
						TNM_GetWordsFromLine(words[i], iwords, ',', '"');	
						floatType sflow;
						TNM_FromString(sflow, iwords[1], std::dec);
						StgLinks* stg = GenerateStgByName(node,iwords[0]);
						stg->sflow = sflow;

						//assign flow and waitcost
						vector<GNODE*>::iterator ig;
						ig = find_if(dest->tplNodeVec.begin(), dest->tplNodeVec.end(), predP(&GNODE::id_, node->id));
						if(ig == dest->tplNodeVec.end())
						{
							cout<<"Cannot find Gnode for node:"<<node->id<<" for dest:"<<dest->destination->id<<endl;
							return false;
						}				                      
						gnode = *ig;
						gnode->m_StgsVec.push_back(stg);
						

						netTTwaitcost += stg->sflow * stg->waitT;
						for (int ix=0;ix<stg->StgLinkPosVec.size();ix++)
						{
							int fix = stg->StgLinkPosVec[ix];
							node->forwStar[fix]->volume += stg->sflow * stg->StgLinkProbVec[ix];
						}
						
					}
					
					
				}
			}
			else
			{
				if (pline.substr(0,6)!="ttwait")
				{
					cout<<"The last row is not ttwait!"<<endl;
					return false;
				}
				else
				{
					TNM_GetWordsFromLine(pline, words, ':', ' ');
					
					TNM_FromString(ttwait, words[1], std::dec);		
					if (fabs(ttwait - netTTwaitcost)>1e-8)
					{
						cout<<"The netTTwaitcost is wrongly imported of"<<ttwait<<" regarding "<<netTTwaitcost<<", gap:"<<TNM_FloatFormat(fabs(ttwait - netTTwaitcost),15,10)<<endl;
						return false;
					}
				}
			}
		}

	}
	cout<<"imported ttwait:"<<TNM_FloatFormat(ttwait,18,12)<<", loaded netTTwaitcost:"<<TNM_FloatFormat(netTTwaitcost,18,12)<<endl;
	return true;

}

bool	PTNET::ImportStgUESolution()
{
	
	netTTwaitcost = 0.0;
	InitialSubnetStgNode();
	string fileName  = networkName + ".stgue";
	ifstream infile;
    if(!TNM_OpenInFile(infile, fileName))
    {
		cout<<"\t cannot read file"<<fileName<<" to read"<<endl;
        return false;
    }
	vector<string> words,iwords;
	string pline;
	int nid;
	int dix,did;
	PTNode* node;
	GNODE* gnode;
	while(getline(infile, pline))
    {
		//cout<<pline<<endl;
        if(!pline.empty())//skip an empty line
        {
			if (pline.substr(0,4)=="Node")
			{
				TNM_GetWordsFromLine(pline, words, ':', '"');
				TNM_FromString(nid, words[1], std::dec);
				node = CatchNodePtr(nid);
				if (!node)
				{
					cout<<"Does not exists node:"<<nid<<endl;
					return false;
				}
				dix = 0;
				
			}
			else if (pline.substr(0,4)=="Dest")
			{
				TNM_GetWordsFromLine(pline, words, ';', ' ');
				TNM_GetWordsFromLine(words[0], iwords, ':', ' ');
				TNM_FromString(did, iwords[1], std::dec);
				//DestStgInfo* stginfo = new DestStgInfo;
				PTDestination* dest = CatchDestPtr(did);
				dest->m_trimmed = true; //this ensure to reset topologic order in the mainloop
				if(!dest)
				{
					cout<<"Does not exists dest:"<<did<<endl;
					return false;
				}

				//stginfo->dest = dest;

				if (words.size()>1)
				{
					for(int i=1;i<words.size();i++)
					{
						TNM_GetWordsFromLine(words[i], iwords, ',', '"');	
						floatType sflow;
						TNM_FromString(sflow, iwords[1], std::dec);
						StgLinks* stg = GenerateStgByName(node,iwords[0]);
						stg->sflow = sflow;

						//assign flow and waitcost
						vector<GNODE*>::iterator ig;
						ig = find_if(dest->tplNodeVec.begin(), dest->tplNodeVec.end(), predP(&GNODE::id_, node->id));
						if(ig == dest->tplNodeVec.end())
						{
							cout<<"Cannot find Gnode for node:"<<node->id<<" for dest:"<<dest->destination->id<<endl;
							return false;
						}				                      
						gnode = *ig;
						gnode->m_StgsVec.push_back(stg);
						

						netTTwaitcost += stg->sflow * stg->waitT;
						for (int ix=0;ix<stg->StgLinkPosVec.size();ix++)
						{
							int fix = stg->StgLinkPosVec[ix];
							node->forwStar[fix]->volume += stg->sflow * stg->StgLinkProbVec[ix];
						}
						
					}
					
					dix++;
				}
			}
			else
			{
				if (pline.substr(0,6)!="ttwait")
				{
					cout<<"The last row is not ttwait!"<<endl;
					return false;
				}
				else
				{
					TNM_GetWordsFromLine(pline, words, ':', ' ');
					floatType ttwait;
					TNM_FromString(ttwait, words[1], std::dec);		
					if (abs(ttwait - netTTwaitcost)>1e-10)
					{
						cout<<"The netTTwaitcost is wrongly imported of"<<ttwait<<" regarding "<<netTTwaitcost<<endl;
						return false;
					}
				}
			}
		}
	}
	//cout<<netTTwaitcost<<endl;
	return true;

}


void	PTNET::ExportHyperpathUEsolution()
{
	if (PCTAE_ALG ==PCTAE_algorithm::PCTAE_P_Greedy || PCTAE_ALG ==PCTAE_algorithm::PCTAE_P_iGreedy 
		||PCTAE_ALG ==PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG ==PCTAE_algorithm::PCTAE_P_iNGP 
		)
	{
		string fileName  = networkName + ".hyperpathue";
		ofstream outfile;
		if (!TNM_OpenOutFile(outfile, fileName))
		{
			cout<<"\n\t Fail to prepare report: Cannot open .hyperpathue file to write information!"<<endl;
			return;
		}
		cout<<"\tWriting hyperpath solution into "<<fileName<<" hyperpath solution!"<<endl;

		PTDestination* dest;
		PTOrg* org;

		for (int i = 0;i<numOfPTDest;i++)
		{
			dest = PTDestVector[i];
			for (int j=0; j<dest->numOfOrg; j++)
			{
				
				org= dest->orgVector[j];
				outfile<<"OD-"<<TNM_IntFormat(j)<<"-"<<TNM_IntFormat(i)<<endl;
				for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();it++) 
				{
					TNM_HyperPath* path = *it;
					ostringstream line("");
					line<<TNM_FloatFormat(path->flow,18,12)<<","<<TNM_FloatFormat(path->WaitCost,18,12)<<","<<TNM_FloatFormat(path->cost,18,12)<<",";
					for(int k=0;k<path->GetGlinks().size();k++)
					{
						GLINK* glink = path->GetGlinks()[k];
						line<<TNM_IntFormat(glink->m_linkPtr->id)<<";"<< TNM_FloatFormat(glink->m_data,18,12) <<",";
					}
					string li = line.str().substr(0,line.str().size()-1);
					outfile<<li<<endl;
				}
			}
		}
		cout<<"finished writing hyperpath solution"<<endl;
		outfile.close();
	}
	else
	{
		cout<<"Please call hyperpath-based algs to write hyperpath flow.";
		system("PAUSE");
	}
}


bool	PTNET::ImportHyperpathUESolution()
{
	string fileName  = networkName + ".hyperpathue";
	ifstream infile;
	PTDestination* dest;
	int oix,dix,lid;
	PTLink* link;
	PTOrg* org;
	floatType flow,wc,p;
    if(!TNM_OpenInFile(infile, fileName))
    {
		cout<<"\t cannot read file"<<fileName<<" to read"<<endl;
        return false;
    }
	vector<string> words,iwords;
	string pline;
	while(getline(infile, pline))
    {
		//cout<<pline<<endl;
        if(!pline.empty())//skip an empty line
        {
			if (pline.substr(0,2)=="OD")
			{
				TNM_GetWordsFromLine(pline, words, '-', '"');
				TNM_FromString(oix, words[1], std::dec);
				TNM_FromString(dix, words[2], std::dec);
				dest = PTDestVector[dix];
				org = dest->orgVector[oix];
				if (!dest || !org)
				{
					cout<<"org or dest does not exist."<<endl;
					return false;
				}
			}
			else
			{
				TNM_HyperPath* path = new TNM_HyperPath();		
				TNM_GetWordsFromLine(pline, words, ',', '"');
				TNM_FromString(flow, words[0], std::dec);
				TNM_FromString(wc, words[1], std::dec);
				path->flow = flow;
				path->WaitCost = wc;
				for(int i=2;i<words.size();i++)
				{
					TNM_GetWordsFromLine(words[i], iwords, ';', '"');
					TNM_FromString(lid, iwords[0], std::dec);
					TNM_FromString(p, iwords[1], std::dec);
					link = CatchLinkPtr(lid);
					path->name += std::to_string(link->id) + "-";
					GLINK* newglink = new GLINK(link);
					newglink->m_data = p;
					link->volume += flow * p;
					path->m_links.push_back(newglink);
				}
				org->pathSet.push_back(path);
				
			}
		}
	}
	return true;
}



void	PTNET::GenerateSPP()
{
	string fileName  = networkName + "-route.txt";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, fileName))
	{
		cout<<"\n\t Fail to prepare report: Cannot open .hyperpathue file to write information!"<<endl;
		return;
	}
	outfile<<"ORGSTOP,DESTSTOP,LINENAME,DIRECTION,TAILSTOP,HEADSTOP,TRANSFERTYPE,SEQUENCE"<<endl;
	for(int i=0;i<numOfLink;i++)
	{
		PTLink* link = linkVector[i];
		if (link->GetTransitLinkType() ==  PTLink::ENROUTE) 
		{
			link->cost = link->length/35.0*60 ;
		}
		else if (link->GetTransitLinkType() ==  PTLink::ABOARD) link->cost = 0;
		else link->cost = 0.01;
	
	}
	vector<string> words;
	int c=0;
	for(PTStopMapIter pv = m_stops.begin(); pv!= m_stops.end(); pv++) 
	{
		c++;
		if (c%10==0) cout<<c<<endl;
		PTStop* deststop=pv->second;
		PTNode* dest = deststop->GetTransferNode();
		InitializeFeasiblePathLS(dest);

		for(PTStopMapIter pt = m_stops.begin(); pt!= m_stops.end(); pt++) 
		{
			PTStop* orgstop=pt->second;
			PTNode* org = orgstop->GetTransferNode();
			PTNode* node = org;
			int n=0;
			if (orgstop!=deststop)
			{
				//cout<<"org:"<<orgstop->m_id<<",dest:"<<deststop->m_id<<endl;
				string lend = "-1";
				while(node!=dest)
				{
					PTLink* link  = node->StgElem->vialink;
					if (link->GetTransitLinkType() ==  PTLink::ENROUTE)
					{
						n++;
						//cout<<link->m_shape->m_id<<","<<link->tail->GetStopPtr()->m_id<<","<<link->head->GetStopPtr()->m_id<<endl;

						TNM_GetWordsFromLine(link->m_shape->m_id, words, '-');
						if (lend =="-1") outfile<<orgstop->m_id<<","<<deststop->m_id<<","<<words[0]<<","<<words[1]<<","<<link->tail->GetStopPtr()->m_id<<","<<link->head->GetStopPtr()->m_id<<","<<1<<","<<n<<endl;
						else
						{
							if (words[0] == lend )  outfile<<orgstop->m_id<<","<<deststop->m_id<<","<<words[0]<<","<<words[1]<<","<<link->tail->GetStopPtr()->m_id<<","<<link->head->GetStopPtr()->m_id<<","<<0<<","<<n<<endl;
							else outfile<<orgstop->m_id<<","<<deststop->m_id<<","<<words[0]<<","<<words[1]<<","<<link->tail->GetStopPtr()->m_id<<","<<link->head->GetStopPtr()->m_id<<","<<2<<","<<n<<endl;
						}
						

						lend = words[0];
					}
					node = link->head;
				}
				//cout<<"total "<<n<<" stops"<<endl;
			
			}
			//cout<<endl<<endl;;
		
		}


	}


	
}
