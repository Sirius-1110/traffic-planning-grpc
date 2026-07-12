#include "PTNet.h"
#include "TNM_utility.h"
//#include "TNM_utility.cpp"
//#include "..\..\..\include\tnm\TNM_utility.h"

PTNode* PTStop::GetTransferNodeTP(int index)
{
	vector<PTNode*>::iterator it;
	for(it=m_transferNodes.begin();it!=m_transferNodes.end();it++)
	{
		PTNode* node= (*it);
		if((node)->m_type==index)
		{
			return *it;
		}
	}
	return NULL;
}

bool comp_label_g_cost(const TPHYPERPATHELEM* a,const TPHYPERPATHELEM* b )
{
	return a->tempuse < b->tempuse;
}

/*
void PTNET::ReprojectNodeCoordinates()
{
	int nnode = nodeVector.size();
    double centralx = 0.0,  maxy = -POS_INF_FLOAT, miny = POS_INF_FLOAT;
    for(int i = 0;i<nnode;i++)
    {
        PTNode *node = nodeVector[i];
        centralx += node->GetStopPtr()->GetLon();
        double ty = node->GetStopPtr()->GetLat();
        if(maxy < ty) maxy = ty;
        if(miny > ty) miny = ty;
            
    }
    centralx/=nnode;

	//cout<<"maxy:"<<maxy<<"miny:"<<miny;
	//cout<<"centralx:"<<centralx<<"  miny:"<<miny<<"  maxy:"<<maxy<<endl;
    if((centralx >=-180 && centralx <=180 && maxy <= 90 && miny >=-90)
        )//confirm if the coordinates was in lat/lon format. 
    {
		
        m_geoIF.InitializeOutGeoRef("NAD83", centralx, true);//geo interface. initialized in ReadPTNode; check if average stop x is between -180 and 180.
        m_geoIF.InitializeInGeoRefAsLatLonOfOutRef();
        m_geoIF.InitializeTransform(false);
		SetProjection(true);
        for(int i = 0;i<nnode;i++)
        {
            PTNode *node = nodeVector[i];
            double tx = node->GetStopPtr()->GetLon(), 
                ty = node->GetStopPtr()->GetLat();
			//cout<<"id:"<<node->id<<" tx:"<<tx<<" ty:"<<ty<<endl;
            m_geoIF.ReprojectPoints(1, &tx,&ty);
			//cout<<"id:"<<node->id<<" tx:"<<tx<<" ty:"<<ty<<endl;
            node->xCord = tx;
            node->yCord = ty;
        }
    }
    else
    {
        for(int i = 0;i<nnode;i++)
        {
            PTNode *node = nodeVector[i];
            double tx = node->GetStopPtr()->GetLon(), 
                ty = node->GetStopPtr()->GetLat();           
            node->xCord = tx;
            node->yCord = ty;
        }
    }
	

}
*/

int  PTNET::CreateTEAPNodeLinks()
{
	cout<<endl<<"============= Start to create nodes and links ============="<<endl;
	cout<<"\tCreating transfer nodes for each stop..."<<endl;
	int nid = 0,lid = 0;//node   link
	
	floatType power = 2;
	floatType alpha_2 = 1.0, beta_2 = 1.0, gama_2 = 1.2;
	floatType beta_3 = 1.0, gama_3 = 0.2;
	floatType alpha_3 = 1.0;
	for(PTStopMapIter pv = m_stops.begin(); pv!= m_stops.end(); pv++) //为每一个stop创造一个transfer node
	{
		PTStop* stop=pv->second;
		nid++;
		PTNode* transfernode = new PTNode();
		transfernode->id = nid;
		transfernode->xCord = stop->GetLon();
		transfernode->yCord = stop->GetLat(); 
		transfernode->SetTransitNodeType(PTNode::TRANSFER);
		transfernode->SetStopPtr(stop);
		stop->SetTransferNode(transfernode);
		nodeVector.push_back(transfernode);
	}
	//ReprojectNodeCoordinates();
	cout<<"\tCreating transit nodes and links for each pattern..."<<endl;	 
	
	 for(PTShapeMapIter ps = m_shapes.begin(); ps!=m_shapes.end();ps++)
	 {		 
		PTShape* shp = ps->second;		
		
		vector<PTStop*> pvec = shp->m_stops;
		//double* shpdist=shp->GetShapeDistancePtr();
		int numofstops=pvec.size();
		double freq=shp->m_freq;
		double cap=shp->m_cap;
		//cout<<shp->m_id<<","<<numofstops<<endl;
		if(numofstops > 1 )
		{
			for (int nseq=0;nseq<numofstops;nseq++)
			{
				PTStop *stop = pvec[nseq];
				PTNode *transfernode=stop->GetTransferNode();
		
				if (nseq>0 && pvec[nseq]->m_id==pvec[nseq-1]->m_id)
				{
					cout<<"exception occurs because stop "<<stop->m_id<<" in pattern "<<shp->m_id<<" visit to itself !"<<endl;
					return 4;
				}
				
				PTNode *transitnode = new PTNode();//()
				nid++;
				transitnode->id = nid;
				nodeVector.push_back(transitnode);
				
				//cout<<"transfernnode id:"<<transfernode->id<<"     "<<"transitnode id:"<<transitnode->id<<endl;
					
				if(!transitnode)
				{
					cout<<"\tFailed to create a new transit node for shape "<<shp->m_id<<" in sequence of "<<nseq<<" at stop "<<stop->m_id<<endl;
					return 1;
				}
				transitnode->SetTransitNodeType(PTNode::ENROUTE);
				transitnode->SetStopPtr(stop);  
				transitnode->SetShapePtr(shp);
				transitnode->SetRoutePtr(shp->m_routePtr);		
				if (stop->AddNode(shp->m_id+";ENROUTE",transitnode))
				{
					//No transit node for the stop have ever created, we can create /transit/board/alight links
					if (nseq<numofstops-1)// last stop cannot aborad
					{
						lid ++;  
						PTLink* boardlink = new PTLink(lid,transfernode,transitnode);	 
						boardlink->SetTransitLinkType(PTLink::ABOARD);
						boardlink->m_hwmean = timescaler / freq;//set the mean headway time to the line!!!!!!
						linkVector.push_back(boardlink);		
						if(boardlink == NULL)
						{
							cout<<"Error: Fail to create a boarding link for "<<stop->m_id<<" in pattern "<<shp->m_id<<endl;
							return 7;
						}
						else
						{
							//boardlink->pars.push_back(freq);
							boardlink->m_shape=shp;
							boardlink->seq = nseq;
							shp->m_boardlinks.push_back(boardlink);
							boardlink->pars.push_back(beta_3);
							boardlink->pars.push_back(gama_3);
							boardlink->pars.push_back(power);
							boardlink->pars.push_back(cap);
							boardlink->cap = cap;
							boardlink->freq = freq / timescaler;

							//cout << boardlink->id << " " << freq << " " << timescaler << " " << boardlink->freq << endl;
						}			
					}
						
					if(nseq>0)//first stop cannot alight
					{
						lid ++;   
						//========**initial setting for alghting link**======
						PTLink* alghtinglink = new PTLink(lid,transitnode,transfernode);	 
						alghtinglink->SetTransitLinkType(PTLink::ALIGHT);

						alghtinglink->m_shape=shp;
						alghtinglink->seq = nseq - 1;
						shp->m_alightlinks.push_back(alghtinglink);

						alghtinglink->pars.push_back(alpha_3);//alpha_4
						alghtinglink->pars.push_back(m_alightLoss);	
						alghtinglink->cap = cap;
						linkVector.push_back(alghtinglink);	

						//transit link initialize
						lid ++;    
						PTNode* tailnode = pvec[nseq-1]->GetNode(shp->m_id+";ENROUTE");//transit node
						PTLink* transitlink = new PTLink(lid,tailnode,transitnode);	 
						/////===========initial setting for transit link 
						transitlink->SetTransitLinkType(PTLink::ENROUTE);
						transitlink->m_shape=shp;
						transitlink->seq = nseq-1;

						transitlink->pars.push_back(alpha_2);
						transitlink->pars.push_back(beta_2);
						transitlink->pars.push_back(gama_2);
						transitlink->pars.push_back(power);//power
						transitlink->pars.push_back(cap);
						transitlink->cap = cap;
						linkVector.push_back(transitlink);	
					}
						
				}
				else
				{
					//transit node must have been created,we only create transit link

					cout<<"\n  pattern "<<ps->second->m_id<<", stop "<<stop->m_id<<" is visited more than once, please make sure the connection is right!"<<endl;
					nodeVector.pop_back();
					//transit link from laststop dwell node to current stop updated transit node
					lid ++;                   
					PTNode* tail = pvec[nseq-1]->GetNode(shp->m_id+";ENROUTE");
					PTNode* head = stop->GetNode(shp->m_id+";ENROUTE");
					PTLink *transitlink = new PTLink(lid,tail,head);	 
					transitlink->SetTransitLinkType(PTLink::ENROUTE);			
					//===========initial setting for transit link===============
					if(transitlink == NULL)
					{
						cout<<"Error: Fail to create a transit link for "<<stop->m_id<<" to "<<transitnode->GetStopPtr()->m_id<<" in pattern "<<shp->m_id<<endl;						
					}		
					else
					{
						transitlink->m_shape=shp;
						transitlink->pars.push_back(alpha_2);
						transitlink->pars.push_back(beta_2);
						transitlink->pars.push_back(gama_2);
						transitlink->pars.push_back(power);//power
						transitlink->pars.push_back(cap);
						transitlink->cap = cap;
					}	
					linkVector.push_back(transitlink);	
				}
			}				
		}
		else
		{
			cout<<"\n  pattern "<<ps->second->m_id<<" has less than 2 stops"<<endl;
			return 5;
		}
	}
	UpdateNodeNum();
	UpdateLinkNum();
	return 0;
}

int  PTNET::CreateTEAPNodeLinksTP()
{
	cout<<endl<<"============= Start to create nodes and links ============="<<endl;
	cout<<"\tCreating transfer nodes for each stop..."<<endl;
	int nid = 0,lid = 0;//node   link
	
	floatType power = 2;
	floatType alpha_2 = 1.0, beta_2 = 1.0, gama_2 = 1.2;
	floatType beta_3 = 1.0, gama_3 = 0.2;
	floatType alpha_3 = 1.0;
	for(PTStopMapIter pv = m_stops.begin(); pv!= m_stops.end(); pv++) //为每一个stop创造一个transfer node
	{
		PTStop* stop=pv->second;
		nid++;
		PTNode* transfernode0 = new PTNode();
		transfernode0->id = nid;
		transfernode0->xCord = stop->GetLon();
		transfernode0->yCord = stop->GetLat(); 
		transfernode0->SetTransitNodeType(PTNode::TRANSFER);
		transfernode0->setType(0);//board（补充添加）
		transfernode0->SetStopPtr(stop);
		stop->SetTransferNodes(transfernode0);//只是为每一个stop创造一个transfer node？这里应当只有一个元素？？
		stop->SetTransferNode(transfernode0);
		nodeVector.push_back(transfernode0);
		nid++;

		PTNode* transfernode1 = new PTNode();
		transfernode1->id = nid;
		transfernode1->xCord = stop->GetLon();
		transfernode1->yCord = stop->GetLat(); 
		transfernode1->SetTransitNodeType(PTNode::TRANSFER);
		transfernode1->setType(1);//alight	（补充添加）
		transfernode1->SetStopPtr(stop);
		stop->SetTransferNodes(transfernode1);
		nodeVector.push_back(transfernode1);
		lid++;

		PTLink* transferlink=new PTLink(lid,transfernode1,transfernode0);
		transferlink->SetTransitLinkType(PTLink::TRANSFER);
		linkVector.push_back(transferlink);		
		if(transferlink == NULL)
		{
			cout<<"Error: Fail to create a transfer link for "<<stop->m_id<<" in pattern "<<endl;
			return 10;
		}
	}
	//ReprojectNodeCoordinates();
	cout<<"\tCreating transit nodes and links for each pattern..."<<endl;	 
	
	 for(PTShapeMapIter ps = m_shapes.begin(); ps!=m_shapes.end();ps++)
	 {		 
		PTShape* shp = ps->second;		
		
		vector<PTStop*> pvec = shp->m_stops;
		//double* shpdist=shp->GetShapeDistancePtr();
		int numofstops=pvec.size();
		double freq=shp->m_freq;
		double cap=shp->m_cap;
		//cout<<shp->m_id<<","<<numofstops<<endl;
		if(numofstops > 1 )
		{
			for (int nseq=0;nseq<numofstops;nseq++)
			{
				PTStop *stop = pvec[nseq];
				PTNode *transfernode0=stop->GetTransferNodeTP(0);
				PTNode *transfernode1=stop->GetTransferNodeTP(1);
				
		
				if (nseq>0 && pvec[nseq]->m_id==pvec[nseq-1]->m_id)
				{
					cout<<"exception occurs because stop "<<stop->m_id<<" in pattern "<<shp->m_id<<" visit to itself !"<<endl;
					return 4;
				}
				
				PTNode *transitnode = new PTNode();//()
				nid++;
				transitnode->id = nid;
				nodeVector.push_back(transitnode);
				
				//cout<<"transfernnode id:"<<transfernode->id<<"     "<<"transitnode id:"<<transitnode->id<<endl;
					
				if(!transitnode)
				{
					cout<<"\tFailed to create a new transit node for shape "<<shp->m_id<<" in sequence of "<<nseq<<" at stop "<<stop->m_id<<endl;
					return 1;
				}
				transitnode->SetTransitNodeType(PTNode::ENROUTE);
				transitnode->SetStopPtr(stop);  
				transitnode->SetShapePtr(shp);
				transitnode->SetRoutePtr(shp->m_routePtr);		
				if (stop->AddNode(shp->m_id+";ENROUTE",transitnode))
				{
					//No transit node for the stop have ever created, we can create /transit/board/alight links
					if (nseq<numofstops-1)// last stop cannot aborad
					{
						lid ++;  
						PTLink* boardlink = new PTLink(lid,transfernode0,transitnode);	 
						boardlink->SetTransitLinkType(PTLink::ABOARD);
						boardlink->m_hwmean = timescaler / freq;//set the mean headway time to the line!!!!!!
						linkVector.push_back(boardlink);		
						if(boardlink == NULL)
						{
							cout<<"Error: Fail to create a boarding link for "<<stop->m_id<<" in pattern "<<shp->m_id<<endl;
							return 7;
						}
						else
						{
							//boardlink->pars.push_back(freq);
							boardlink->m_shape=shp;
							boardlink->seq = nseq;
							shp->m_boardlinks.push_back(boardlink);
							boardlink->pars.push_back(beta_3);
							boardlink->pars.push_back(gama_3);
							boardlink->pars.push_back(power);
							boardlink->pars.push_back(cap);
							boardlink->cap = cap;//计算容量
							boardlink->freq = freq/timescaler;//折算频率
						}			
					}
						
					if(nseq>0)//first stop cannot alight
					{
						lid ++;   
						//========**initial setting for alghting link**======
						PTLink* alghtinglink = new PTLink(lid,transitnode,transfernode1);	 
						alghtinglink->SetTransitLinkType(PTLink::ALIGHT);

						alghtinglink->m_shape=shp;
						alghtinglink->seq = nseq - 1;
						shp->m_alightlinks.push_back(alghtinglink);

						alghtinglink->pars.push_back(alpha_3);//alpha_4
						alghtinglink->pars.push_back(m_alightLoss);	
						alghtinglink->cap = cap;
						linkVector.push_back(alghtinglink);	

						//transit link initialize
						lid ++;    
						PTNode* tailnode = pvec[nseq-1]->GetNode(shp->m_id+";ENROUTE");//transit node
						PTLink* transitlink = new PTLink(lid,tailnode,transitnode);	 
						/////===========initial setting for transit link 
						transitlink->SetTransitLinkType(PTLink::ENROUTE);
						transitlink->m_shape=shp;
						transitlink->seq = nseq-1;

						transitlink->pars.push_back(alpha_2);
						transitlink->pars.push_back(beta_2);
						transitlink->pars.push_back(gama_2);
						transitlink->pars.push_back(power);//power
						transitlink->pars.push_back(cap);
						transitlink->cap = cap;
						linkVector.push_back(transitlink);	
					}
						
				}
				else
				{
					//transit node must have been created,we only create transit link

					cout<<"\n  pattern "<<ps->second->m_id<<", stop "<<stop->m_id<<" is visited more than once, please make sure the connection is right!"<<endl;
					nodeVector.pop_back();
					//transit link from laststop dwell node to current stop updated transit node
					lid ++;                   
					PTNode* tail = pvec[nseq-1]->GetNode(shp->m_id+";ENROUTE");
					PTNode* head = stop->GetNode(shp->m_id+";ENROUTE");
					PTLink *transitlink = new PTLink(lid,tail,head);	 
					transitlink->SetTransitLinkType(PTLink::ENROUTE);			
					//===========initial setting for transit link===============
					if(transitlink == NULL)
					{
						cout<<"Error: Fail to create a transit link for "<<stop->m_id<<" to "<<transitnode->GetStopPtr()->m_id<<" in pattern "<<shp->m_id<<endl;						
					}		
					else
					{
						transitlink->m_shape=shp;
						transitlink->pars.push_back(alpha_2);
						transitlink->pars.push_back(beta_2);
						transitlink->pars.push_back(gama_2);
						transitlink->pars.push_back(power);//power
						transitlink->pars.push_back(cap);
						transitlink->cap = cap;
					}	
					linkVector.push_back(transitlink);	
				}
			}				
		}
		else
		{
			cout<<"\n  pattern "<<ps->second->m_id<<" has less than 2 stops"<<endl;
			return 5;
		}
	}
	UpdateNodeNum();
	UpdateLinkNum();
	return 0;
}

void PTNET::GetVicinityNodes(COORDMAP &xmap, COORDMAP &ymap, vector<PTNode *> &nvec, PTNode *node, long threshold)
{
	nvec.clear();
	if(!node) return;
	long xLB = node->xCord - threshold, xUB = node->xCord + threshold, yLB = node->yCord-threshold,
		yUB = node->yCord + threshold;
	COORDMAP_I xLiter, xUiter, yLiter, yUiter, pv;
	if(xLB > xmap.rbegin()->first || xUB < xmap.begin()->first || yLB > ymap.rbegin()->first || yUB < ymap.begin()->first) 
	{
	//	cout<<"no vicinity set found for node "<<node->id<<endl;
		return;
	}

	xLiter = xmap.lower_bound(xLB);
	xUiter = xmap.upper_bound(xUB);
	yLiter = ymap.lower_bound(yLB);
	yUiter = ymap.upper_bound(yUB);

	set<int> neighbor;	
	pv = xLiter;
	do 
	{
		neighbor.insert(pv->second);
		if(pv!= xUiter) pv++;
	}while(pv!= xUiter);
	if(neighbor.empty()) return;
	set<int>::const_iterator ei = neighbor.end();
	pv = yLiter;
	do
	{
		if(neighbor.find(pv->second)!= ei) //so we find it.
		{
			nvec.push_back(nodeVector[pv->second-1]);
		}
		if(pv!= yUiter) pv++;
	}while(pv!=yUiter);

}

void	PTNET::CreateODLink()
{
	int lid = numOfLink;
	int n=0;
	PTDestination* dest;
	srand(10);//随机数
	PTLink* walklink;
	for (int i = 0;i<numOfPTDest; i++)
	{
		dest = PTDestVector[i];
		for (int j=0; j<dest->numOfOrg; j++)
		{		
			PTOrg* org= dest->orgVector[j];
			lid++;

			walklink = CatchLinkPtr(org->org,dest->destination);

			if (!walklink)
			{

				walklink = new PTLink(lid,org->org,dest->destination);
				walklink->SetTransitLinkType(PTLink::WALK);
				if(walklink==NULL)
				{
					cout<<"\tFailed to create a walk link from node "<<org->org->id<<" to "<<dest->destination->id<<endl;
					return ;
				}     
				else
				{
					//walklink->pars.push_back(1.0);//alpha_1

					//double a= rand() % 60 + 60;
					//cout<<a<<endl;
					//walklink->pars.push_back(a);  //using a random min
				}		
				linkVector.push_back(walklink);

				n++;
			}
			else
			{
				cout<<"OD:"<<org->org->id<<"-"<<dest->destination->id<<" already have walk links."<<endl;
			}

		}
	}

	UpdateLinkNum();
	cout<<"\tCreate "<<n<<" walking links"<<endl;
	return;

}

PTDestination*	PTNET::CreatePTDestination(PTNode* rootDest, int noo)
{
	if (noo <0)
	{
		cout<<"\n\\Dest "<<rootDest->id<<" contains none or negative orgs."<<endl;
		return NULL;
	}
	PTDestination *dest = new PTDestination(rootDest, noo);
	if (dest == NULL)
	{
		cout<<"\n\tCannot allocate memory for new dest"<<endl;
		return NULL;
	}
	PTDestVector.push_back(dest);
	numOfPTDest ++;
	numOfPTOD += noo;
	return dest;
}

PTDestination*	PTNET::CreatePTDestination(int nid, int noo)
{
	PTNode *node = nodeVector[nid-1];
	if(node == NULL)
	{
		cout<<"\n\tnode "<<nid<<" is not a valid node object. "<<endl;
		return NULL;
	}
	else 
	{
		if (noo <0)
		{
			cout<<"\n\\Dest "<<node->id<<" contains none or negative orgs."<<endl;
			return NULL;
		}
		PTDestination *dest = new PTDestination(node, noo);
		if (dest == NULL)
		{
			cout<<"\n\tCannot allocate memory for new dest"<<endl;
			return NULL;
		}
		PTDestVector.push_back(dest);
		numOfPTDest ++;
		numOfPTOD += noo;
		return dest;
	}
}

int  PTNET::CreateTransferlink()
{
	PTNode* node;
	int lid=numOfLink;
	for(int i=0;i<numOfNode;i++)
	{
		node=nodeVector[i];
		if(node->GetTransitNodeType()==PTNode::TRANSFER)
		{
			vector<PTNode*> tempvec;
			for(PTRTRACE pv = node->backStar.begin(); pv!= node->backStar.end(); pv++)
			{
				PTNode* tempnode1;
				tempnode1=(*pv)->tail;
				if(tempnode1->GetTransitNodeType()==PTNode::ENROUTE)
				{
					for(PTRTRACE p = node->backStar.begin(); p!= node->backStar.end(); p++)
					{
						PTNode* tempnode2;
						tempnode2=(*p)->tail;
						if(tempnode2->GetTransitNodeType()==PTNode::ENROUTE && tempnode2!=tempnode1)
						{
							lid++;
							PTLink* transferlink = new PTLink(lid,tempnode1,tempnode2);
							transferlink->SetTransitLinkType(PTLink::TRANSFER);
							linkVector.push_back(transferlink);
						}
					}
				}
			}
		}
	}
	return 0;
}

int PTNET::UpdateLabel(TPHYPERPATHELEM* tElem,TPHYPERPATHELEM* pElem)
{
	if(tElem && pElem)
	{
		tElem->cost = pElem->cost;
		tElem->dist = pElem->dist;
		tElem->via = pElem->via;
		tElem->vialink = pElem->vialink;
		tElem->transfers = pElem->transfers;
		tElem->walkcost = pElem->walkcost;
		//tElem->state = pElem->state;
		//tElem->sIndex = pElem->sIndex;
		//tElem->travelStrategy = pElem->travelStrategy;
		tElem->stgLabels = pElem->stgLabels;
		tElem->m_attProb = pElem->m_attProb;
		tElem->m_attStates = pElem->m_attStates;
		tElem->m_wait = pElem->m_wait; //only for bus waiting
		tElem->moneySpent = pElem->moneySpent;
		tElem->trvelTime = pElem->trvelTime;
		tElem->trvelDist = pElem->trvelDist;//distance,km
		tElem->node = pElem->node;//the node with this label
	}
	else
	{
		cout<<"tElem && pElem is NULL!"<<endl;
	}
	return 0;
}

int  PTNET::ReInsertLabelML(TPHYPERPATHELEM* tElem,TPHYPERPATHELEM* pElem,multimap<double, TPHYPERPATHELEM*,less<double>> &Q,double hvalue)
{
	multimap<double, TPHYPERPATHELEM*,less<double>>::iterator lower,upper,piter;
	lower =Q.lower_bound(tElem->cost + hvalue - 1e-10);
	upper = Q.upper_bound(tElem->cost + hvalue + 1e-10);
	piter = lower;
	while(piter!=upper && piter->second!=tElem)
	{
		//cout<<"nodeid:"<<piter->second->id<<",cost:"<<piter->first<<", "<<piter->second->StgElem->cost<<endl;
		piter++;
	}
	if(piter == Q.end()) 
	{
		cout<<"Warning: cannot find tailNode label, it's cost is "<<tElem->cost<<" transfer:"<<tElem->transfers<<" scan status:"<<tElem->scanStatus<<endl;
		cout<<"\tCurrently, Q has the following labels:"<<endl;                
		for(lower = Q.begin(); lower!=Q.end();lower++)
		{
			cout<<"node id:"<<lower ->second->node->id<<
				" cost:"<<lower->first<<" transfers:"<<lower->second->transfers<<endl;
		}
		system("pause");
		return 1;
	}
	if(piter->second!=tElem)
	{
		cout<<"\tError: failed to find tailNode lablel,while it is in the queue."<<endl;
		cout<<"tail node id:"<<tElem->node->id<<
				" cost:"<<tElem->cost<<" transfers:"<<tElem->transfers<<" scan status:"<<tElem->scanStatus<<endl;
		//PrintLSQ(Q,200);
		system("pause");
		
		return 1;
	}
	else
	{
		Q.erase(piter);
		Q.insert(std::pair<double, TPHYPERPATHELEM* >(pElem->cost + hvalue,tElem));
	}
	return 0;
}

int  PTNET::EraseLabelML(TPHYPERPATHELEM* tElem,multimap<double, TPHYPERPATHELEM*,less<double>> &Q,double hvalue)
{
	multimap<double, TPHYPERPATHELEM*,less<double>>::iterator lower,upper,piter;
	lower =Q.lower_bound(tElem->cost + hvalue - 1e-10);
	upper = Q.upper_bound(tElem->cost + hvalue + 1e-10);
	piter = lower;
	while(piter!=upper && piter->second!=tElem)
	{
		//cout<<"nodeid:"<<piter->second->id<<",cost:"<<piter->first<<", "<<piter->second->StgElem->cost<<endl;
		piter++;
	}
	if(piter == Q.end()) 
	{
		cout<<"Warning: cannot find tailNode label, it's cost is "<<tElem->cost<<" transfer:"<<tElem->transfers<<endl;
		cout<<"\tCurrently, Q has the following labels:"<<endl;                
		for(lower = Q.begin(); lower!=Q.end();lower++)
		{
			cout<<"node id:"<<lower ->second->node->id<<
				" cost:"<<lower->first<<" transfers:"<<lower->second->transfers<<endl;
		}
		return 1;
	}
	if(piter->second!=tElem)
	{
		cout<<"\tError: failed to find tailNode lablel,while it is in the queue."<<endl;
		cout<<"tail node id:"<<tElem->node->id<<
				" cost:"<<tElem->cost<<" transfers:"<<tElem->transfers<<endl;
		return 1;
	}
	else
	{
		Q.erase(piter);
	}
	return 0;
}

int PTNET::UpdateLabelLSML(PTNode* tailNode,PTLink* link,TPHYPERPATHELEM* &tElem,TPHYPERPATHELEM* pElem,multimap<double, TPHYPERPATHELEM*,less<double>> &Q_now,multimap<double, TPHYPERPATHELEM*,less<double>> &Q_next,double hvalue)
{
	/*if(link->GetTransitLinkType()==PTLink::WALK)
	{
		pElem->walkcost+=link->cost;
	}*/
	double fvalue = pElem->cost + hvalue;
	string ts;
	/*if(tailNode->id==26)
	{
		cout<<endl;
	}*/
	if(link->GetTransitLinkType() == PTLink::WALK || link->GetTransitLinkType() == PTLink::TRANSFER)
	{
		if(!tElem)//pElem not in tail node LabelsMap//???????????
		{
			pElem->scanStatus = 1;
			Q_next.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,pElem));
			//tElem->PrintStgLabels();
			//pElem->PrintStgLabels();
			//cout<<"+"<<endl;
			//cout<<pElem->node->id<<endl;
			/*if(pElem->node->id==65)
			{
				cout<<endl;
			}*/
			tailNode->InsertLabel(pElem);
			//cout<<"insert pElem: ";
			//pElem->PrintStgLabels();
			pElem->isInserted = true;
		}
		else
		{
			if(tElem->scanStatus == 0)  //tail Label is not in QNext or QNow
			{     		
				if(tElem->transfers == pElem->transfers)
				{
					UpdateLabel(tElem,pElem);
					tElem->scanStatus = 1;
					Q_next.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,tElem));	
					//cout<<"-"<<endl;
				}
				else  
				{
					pElem->scanStatus = 1;
					Q_next.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,pElem));
					//cout<<"*"<<endl;
					/*if(pElem->node->id==65)
					{
						cout<<endl;
					}*/
					tailNode->InsertLabel(pElem);
					//cout<<"insert pElem: ";
					//pElem->PrintStgLabels();
					//cout<<"-"<<endl;
					pElem->isInserted = true;
				}
			}	
			else
			{
				if(tElem->transfers ==  pElem->transfers-1) //tElem in QNow 
				{
					pElem->scanStatus = 1;
					Q_next.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,pElem));
					//cout<<"/"<<endl;
					tailNode->InsertLabel(pElem);
					//cout<<"insert pElem: ";
					//pElem->PrintStgLabels();
					//cout<<"+"<<endl;
					pElem->isInserted = true;
				}
				else if (tElem->transfers ==  pElem->transfers ) //tElem in QNext 
				{
					ReInsertLabelML(tElem,pElem,Q_next,hvalue);
					//cout<<"11111"<<endl;
					/*map<string,TPHYPERPATHELEM*>::iterator iter;
					for(iter=tailNode->m_labels->begin();iter!=tailNode->m_labels->end();iter++)
					{
						if(iter->second->transfers==tElem->transfers)
						{
							tailNode->m_labels->erase(to_string(tElem->transfers)+"-"+to_string(tElem->walkcost));
							tailNode->InsertLabel(pElem);;
						}
					}*/
					UpdateLabel(tElem,pElem);
					//DeleteLabel(pElem);		
				}
				else
				{
					cout<< "The tELem scanStatus is 1, but it's transfers != pElem->transfers-1 or pElem->transfers!"<<endl;
					cout<<tElem->transfers<<","<<pElem->transfers<<endl;
					system("pause"); 
				}
			}
		}
	}
	else
	{
		if(!tElem)
		{
			pElem->scanStatus = 1;
			Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,pElem));
			//cout<<"!"<<endl;
			//cout<<pElem->node->id<<endl;
			/*if(pElem->node->id==65)
			{
				cout<<endl;
			}*/
			tailNode->InsertLabel(pElem);//???????
			//cout<<"insert pElem: ";
			//pElem->PrintStgLabels();
			//cout<<"@"<<endl;
			pElem->isInserted = true;
		}
		else
		{
			if(tElem->scanStatus == 0)  
			{                              
				if(tElem->transfers == pElem->transfers)
				{
					UpdateLabel(tElem,pElem);
					tElem->scanStatus = 1;
					//DeleteLabel(pElem);
					Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,tElem));
					//cout<<pElem->node->id<<endl;
				}
				else  
				{
					pElem->scanStatus = 1;
					Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,pElem));
					//cout<<pElem->node->id<<endl;
					//cout<<"@"<<endl;
					/*if(pElem->node->id==65)
					{
						cout<<endl;
					}*/
					tailNode->InsertLabel(pElem);
					//cout<<"insert pElem: ";
					//pElem->PrintStgLabels();
					//cout<<"#"<<endl;
					pElem->isInserted = true;
				}
			}
			else 
			{
				if(tElem->transfers == pElem->transfers )//??????
				{
					ReInsertLabelML(tElem,pElem,Q_now,hvalue);
					//UpdateLabel(tElem,pElem);
					//cout<<"22222"<<endl;
					//map<string,TPHYPERPATHELEM*>::iterator iter;
					//cout<<tailNode->id<<endl;
					/*if(tailNode->id==66)
					{
						cout<<endl;
					}*/
					/*for(iter=tailNode->m_labels->begin();iter!=tailNode->m_labels->end();iter++)
					{
						if(iter->second->transfers==tElem->transfers)
						{
							tailNode->m_labels->erase(to_string(tElem->transfers)+"-"+to_string(tElem->walkcost));
							tailNode->InsertLabel(pElem);;
						}
					}*/
					UpdateLabel(tElem,pElem);
					//m_label里，把tElem的ts换了
					//DeleteLabel(pElem);
				}
				else if(tElem->transfers > pElem->transfers)
				{
					EraseLabelML(tElem,Q_next,hvalue);
					Q_now.insert(std::pair<double, TPHYPERPATHELEM*>(fvalue,tElem));
					//cout<<pElem->node->id<<endl;
					UpdateLabel(tElem,pElem);
					//DeleteLabel(pElem);
				}
				else
				{
					cout<< "The tELem scanStatus is 1, but it's transfers < pElem->transfers"<<endl;
					cout<<tElem->transfers<<","<<pElem->transfers<<endl;
					system("pause"); 
				}
			}
		}
	}
	return 0;
}

int PTNET::ClearQNow(multimap<double, TPHYPERPATHELEM*,less<double>> &Q_now)
{
	if(!Q_now.empty())
	{
		TPHYPERPATHELEM* label;
		for(multimap<double, TPHYPERPATHELEM*,less<double>>::iterator  it = Q_now.begin(); it != Q_now.end();it++)
		{
			it->second->scanStatus = 0;
		}
		Q_now.clear();
	}
	return 0;
}

int PTNET::GetAttSetAndStateMLMMEAP(PTNode* tnode,map<int,TPHYPERPATHELEM*> links, TPHYPERPATHELEM* tempElem,TPHYPERPATHELEM* hElem)
{
	floatType ec=0.0,ew=0.0,ttfreq = 0.0;
	vector<TPHYPERPATHELEM*> AttractiveSet;
	
	vector<TPHYPERPATHELEM*> candSet;
	typedef vector<TPHYPERPATHELEM*>::iterator LITER;
	typedef map<int, TPHYPERPATHELEM*>::iterator CANDITER;
	PTLink*  link;
	for(CANDITER it = links.begin(); it != links.end();it++)
	{
		 candSet.push_back(it->second);
	} 
	if ( candSet.size()>1)
		sort(candSet.begin(), candSet.end(),comp_label_g_cost);
	
	int curState,nextState,state;
	LITER pv= candSet.begin();
	bool firststate = true;
	do 
    {
		bool updated = true;

		link = CatchLinkPtr(tnode, (*pv)->node);
		if(link->m_hwmean > 0.0)
		{
			if(firststate)
			{
				firststate = false;
				//state = (*pv)->state; 
			}
			AttractiveSet.push_back(*pv);
			
		}
		else updated = false;

		ttfreq = 0.0;
		
		for (LITER ai = AttractiveSet.begin();ai!= AttractiveSet.end();)
		{
			link =CatchLinkPtr(tnode, (*ai)->node);
			if(link->m_hwmean == 0.0)
			{
				AttractiveSet.erase(ai);	
				if(ai== AttractiveSet.end()) break;
			}
			else
			{
				if(updated)
				{
					updated = false;
				}
				ttfreq += 1.0/ link->m_hwmean;	
				ai++;
			}
		}
		ew = 1/ttfreq;
		ec = ew;
		double p =0;
		for (LITER ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			link = CatchLinkPtr(tnode, (*ai)->node);
			link->m_probdata = (1.0/link->m_hwmean)/ttfreq;
			ec += link->m_probdata * (*ai)->tempuse;
		}
		pv++;
	}while(pv!=candSet.end() && (*pv)->tempuse < ec );
	if(!AttractiveSet.empty())
	{
		tempElem->cost =ec;
		tempElem->m_wait = ew;	
		tempElem->node = tnode;
		tempElem->m_attProb.clear();
		tempElem->vialink = CatchLinkPtr(tnode, (*AttractiveSet.begin())->node);
		tempElem->via_m_cost.clear();
		tempElem->stgLabels.clear();
	
		set<string> stgname;
		for (LITER ai = AttractiveSet.begin();ai!= AttractiveSet.end();ai++)
		{
			link = CatchLinkPtr(tnode, (*ai)->node);
			tempElem->stgLabels.push_back(*ai);
			tempElem->m_attProb.push_back(link->m_probdata);
		}
	}
	else return 1;
	return 0;
}

//TPHYPERPATHELEM*  PTNET::GetMaxPreferredLabel(PTNode*tail,TPHYPERPATHELEM* pElem,int curTrans)
//{
//	TPHYPERPATHELEM* tlabel = NULL;
//	map<int, vector<int>, less<int> >* preferredStates = GetPSPtr();
//	map<int, vector<int>, less<int> >::iterator it = preferredStates->find(pElem->state);
//	vector<int> st;
//	if(it!= preferredStates->end()) st = it->second;
//	double tcost = -POS_INF_FLOAT;
//	//PrintCurNodeLable(pElem);
//	for(LabelsMapIter  it = tail->m_labels->begin();it !=tail->m_labels->end();it++)
//	{
//		if(find(st.begin(),st.end(),it->second->state) != st.end())
//		{ 
//			if(it->second->cost > tcost && it->second->transfers<=curTrans)
//			{
//				tcost = it->second->cost;
//				tlabel= it->second;
//				//tail->PrintNodeMapLables();
//			}
//		}	
//	}
//	return tlabel;
//}

void PTNET::PrintCurNodeLable(TPHYPERPATHELEM* curLabel)
{
	if(curLabel)
	{
		
		cout<<"Label Cost:"<<curLabel->cost<<", time:"<<curLabel->trvelTime<<", distance:"<<curLabel->trvelDist<<", money:"<<curLabel->moneySpent
			<<", Transfers:"<<curLabel->transfers
			<<" scan status:"<<curLabel->scanStatus<<" Node Id:"<<curLabel->node->id<<endl;
	}
}

int	PTNET::AllocateLinkBuffer(int size)
{
	PTLink *link;
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

int	PTNET::AllocateNetBuffer(int size)
{
	if (size <=0)
	{
		cout<<"\tInvalid size of network buffer array"<<endl;
		return 1;
    }

	//if (buffer) delete [] buffer;
	buffer = new floatType[size];
	if(buffer == NULL)
	{
		cout<<"\tCannot allocate memory for network buffer!"<<endl;
		return 1;
	}
	for (int j = 0;j<size;j++) buffer[j] = 0.0;
	return 0;

}

void PTNET::ReleaseMLElemMap()
{
	map<string,TPHYPERPATHELEM*>::iterator pv;
	for(int i = 0;i<numOfNode;i++)
	{
		PTNode *node = nodeVector[i];
		/*cout<<node->id<<endl;
		if(node->id ==26)
		{
			cout<<endl;
		}*/
		if(node->m_labels)
		{
			for(pv = node->m_labels->begin(); pv!=node->m_labels->end();pv++)
			{
				delete pv->second;
				pv->second=NULL;
			}
			delete node->m_labels;
			node->m_labels = NULL;
		}
	}
}

int PTNET::DominanceCheck(PTNode* tailNode,TPHYPERPATHELEM* &tElem,TPHYPERPATHELEM* pElem,int curTrans,PTLink* link,floatType curwalkcost)
{	
	string ts =to_string(pElem->transfers);//+"-"+to_string(pElem->walkcost)
	if(pElem->walkcost>ttwalktime)
	{
		return 0;
	}

	if(!tailNode->GetMinLabeForFindSHP(curTrans,curwalkcost)) //the tial node never be visited
	{
		//cout<<"4"<<endl;
		tElem = NULL;
		return 1;
	}
	
	//tElem = tailNode->GetLabel(ts);
	else
	//if(true)
	{		
		tElem = tailNode->GetLabel(ts);
		//tElem = tailNode->GetMaxLabelOnState(curTrans,walklimit);
		//tElem->PrintStgLabels();
		if(!tElem)
		{
			//tElem = tailNode->GetLabel(ts);
			tElem = tailNode->GetMaxLabelOnState(curTrans,curwalkcost);
			/*TPHYPERPATHELEM* tempElem;
			tempElem=tailNode->GetMaxLabelOnState(curTrans,walklimit);*/
			//tElem = tailNode->GetMinLabeForFindSHP(curTrans,walklimit);
			if(!tElem || tElem->cost > pElem->cost)
			{
				return 1;
			}
			/*else if(tElem->walkcost>pElem->walkcost)
			{

			}*/
			else return 0;
			/*tElem=tailNode->GetMaxLabelOnState(curTrans,walklimit);
			if(!tElem || tElem->cost > pElem->cost)
			{
				return 1;
			}
			else return 0;*/
		
		}
		else if(tElem->cost > pElem->cost)
		{
			//cout<<"2"<<endl;
			
			return 1;
		}
		//????else if(tElem->walkcost>pElem->walkcost)
		
		else 
		{//cout<<"3"<<endl;
		return 0;}
	}
	//else if(!tailNode->GetMinLabeForFindSHP(curTrans,walklimit)) //the tial node never be visited
	//{
	//	//cout<<"4"<<endl;
	//	tElem = NULL;
	//	return 1;
	//}
	return 0;
}

int	PTNET::AllocateNodeBuffer(int size)
{
	PTNode *node;
	if (size <=0)
	{
		cout<<"\tInvalid size of network buffer array"<<endl;
		return 1;
    }
	else
	{
		nodeBufferSize = size;
	}

	for (int i= 0;i<numOfNode;i++)
	{
		node = nodeVector[i];
		if(node->buffer) delete [] node->buffer;
		node->buffer = new floatType[size];
		if(node->buffer == NULL)
		{
			cout<<"\tCannot allocate memory for link buffer!"<<endl;
			return 1;
		}
		for (int j = 0;j<size;j++)
			node->buffer[j] = 0.0;
	
	}
	return 0;
}

void PTNET::UpatePTNetworkLinkCost_for_test()
{
	PTLink *link;
	for(int i = 0; i < numOfLink; i++)
	{
		link = linkVector[i];
		
		if(link->GetTransitLinkType() == PTLink::ABOARD)
		{
			link->cost = 0.5;
			link->pfdcost = 0.0;
			link->rpfdcost = 0.0;
		}

		if(link->GetTransitLinkType() == PTLink::ENROUTE)
		{
			link->cost = 35;
			link->pfdcost = 0.0;
			link->rpfdcost = 0.0;
			transitlinkVector.push_back(link);
		}

		if(link->GetTransitLinkType() == PTLink::WALK)
		{
			link->cost = 45;
			link->pfdcost = 0.0;
			link->rpfdcost = 0.0;
		}

		if(link->GetTransitLinkType() == PTLink::ALIGHT)
		{
			link->cost = 0.5;
			link->pfdcost = 0.0;
			link->rpfdcost = 0.0;
			link->cap = 1000000;
		}

		link->freq = link->freq * 60.0;

		cout<<link->id<<" "<<link->GetTransitLinkTypeName()<<" "<<link->cost<<" "<<link->freq<<" "<<link->cap<<endl;
	}
}

void PTNET::UpatePTNetworkLinkCost()
{
	PTLink *link,*rlink;
	for(int i = 0; i < numOfLink; i++)
	{
		link = linkVector[i];
		
		if(link->volume < -1e-10)
		{
			cout<<endl;
		}
		//link->UpdatePTLinkCost();
		//link->UpdatePTDerLinkCost();

		/*常数*/
		link->UpdatePTLinkCost_const();
		link->UpdatePTDerLinkCost_const();

		if (link->rLink && link->GetTransitLinkSymmetry()==PTLink::ASYMMTRIC)
		{
			if(link->rLink->volume < -1e-10)
			{
				cout<<endl;
			}
			//link->rLink->UpdatePTLinkCost();
			//link->rLink->UpdatePTDerLinkCost();

			/*常数*/
			link->rLink->UpdatePTLinkCost_const();
			link->rLink->UpdatePTDerLinkCost_const();
		}

		if (link->GetTransitLinkType() == PTLink::ENROUTE)
		{
			transitlinkVector.push_back(link);
		}
	}
}

floatType	PTLink::GetPTLinkCost()
{
	floatType tc = 0.0, tmpcost;
	tmpcost = cost;
	UpdatePTLinkCost();
	tc = cost;
	cost = tmpcost;
	//switch(GetTransitLinkType())
 //       {
	//		case PTLink::WALK:
	//			double alpha_1,wt;
	//			alpha_1=pars[0];
	//			wt=	pars[1];
	//			tc = alpha_1 * wt;
	//			break;
	//		case PTLink::ABOARD:
	//			double alpha_2,beta_2,n1,cap1;
	//			alpha_2	=	pars[0];
	//			beta_2	=	pars[1];
	//			n1		=	pars[2];//power
	//			cap1		=	cap;
	//			tc = alpha_2 * pow (((1 - beta_2) * volume + beta_2 * rLink ->volume) / cap1 , n1);
	//			break;
	//		case PTLink::ALIGHT:
	//			double alpha_4,tloss;
	//			alpha_4=	pars[0];
	//			tloss =		pars[1];
	//			tc  = alpha_4 * tloss;
	//			break;
	//		case PTLink::ENROUTE:
	//			double alpha_3,beta_3,gamma_3,n2,cap2,fft;
	//			alpha_3	=	pars[0];
	//			beta_3	=	pars[1];
	//			gamma_3	=	pars[2];
	//			n2		=	pars[3];//power
	//			cap2	=	cap;
	//			fft		=	pars[5];
	//			tc = alpha_3 * fft + beta_3 * pow ((volume + (gamma_3 - 1) * rLink ->volume) / cap2, n2);
	//			break;
 //       }

	return tc;

}

floatType	PTLink::GetPTLinkDerCost()
{
	floatType tc = 0.0, tmpcost;
	tmpcost = pfdcost;
	UpdatePTDerLinkCost();
	tc = pfdcost;
	pfdcost = tmpcost;

	//switch(GetTransitLinkType())
	//{
	//	case PTLink::WALK:
	//		tc = 0.0;
	//		break;
	//	case PTLink::ABOARD:
	//		double alpha_2,beta_2,n1,cap1;
	//		alpha_2	=	pars[0];
	//		beta_2	=	pars[1];
	//		n1		=	pars[2];//power
	//		cap1		=	cap;
	//		tc = alpha_2 * n1 * pow (((1 - beta_2) * volume + beta_2 * rLink ->volume) / cap1 , n1 - 1) * ( (1 - beta_2) / cap1);
	//		break;
	//	case PTLink::ALIGHT:
	//		tc = 0.0;
	//		break;
	//	case PTLink::ENROUTE:
	//		double alpha_3,beta_3,gamma_3,n2,cap2,fft;
	//		alpha_3	=	pars[0];
	//		beta_3	=	pars[1];
	//		gamma_3	=	pars[2];
	//		n2		=	pars[3];//power
	//		cap2	=	cap;
	//		fft		=	pars[5];
	//		tc  =   beta_3 * n2 * pow ((volume + (gamma_3 - 1) * rLink ->volume) / cap2, n2 - 1) * (1 / cap2);
	//		break;
	//}

	return tc;

}

void PTLink::UpdatePTLinkCost_const_TEST()
{
	//floatType tvol,rtvol = 0.0;
	//if(volume<0.0) 
	//{
	//	tvol = 0.0;
	//	if (fabs(volume)>1e-10)
	//	{
	//		cout<<"link id :"<<id<<" have negative flow:"<<volume<<endl;
	//	}
	//	else
	//	{
	//		volume = 0.0;
	//	}
	//}
	//else tvol = volume;
	//if (rLink)
	//{
	//	if (rLink->volume<0.0) 
	//	{
	//		//cout<<"link id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
	//		rtvol = 0.0;
	//		if (fabs(rLink->volume)>1e-10)
	//		{
	//			cout<<"link id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
	//		}
	//		else
	//		{
	//			rLink->volume = 0.0;
	//		}
	//	}
	//	else  rtvol = rLink ->volume;
	//}

	if (volume<0 && volume>-1e-10) volume = 0.0;
	if (volume < -1e-10)
	{
		cout << "link id :" << id << " have negative flow:" << volume << endl;
	}

	switch (GetTransitLinkType())
	{
	case PTLink::FAILWALK: //add for failing walk link
		double alpha_11, wt1;
		alpha_11 = pars[0];
		wt1 = pars[1];
		cost = alpha_11 * wt1;
		break;
	case PTLink::WALK:
		double alpha_1, wt;
		alpha_1 = pars[0];
		wt = pars[1];
		cost = alpha_1 * wt;
		break;
	case PTLink::TRANSFER:
		cost = 0.0;
		break;
	case PTLink::ABOARD:
		//cost = m_hwmean * 0.5;
		cost = 0.0;
		break;
	case PTLink::ALIGHT:
		//cost = 0.1;
		cost = 0.0;
		break;
	case PTLink::ENROUTE:
		double alpha_3, beta_3, gamma_3, n2, cap2, fft;
		alpha_3 = pars[0];
		beta_3 = pars[1];
		gamma_3 = pars[2];
		n2 = pars[3];//power
		cap2 = cap;
		fft = pars[5];

		cost = alpha_3 * fft;

		if (m_tlSym == PTLink::ASYMMTRIC)
		{
			if (rLink->volume<0 && rLink->volume>-1e-10) rLink->volume = 0.0;
			cost = alpha_3 * fft + beta_3 * pow((volume + (gamma_3 - 1) * rLink->volume) / cap2, n2);
		}
		else
		{
			cost = alpha_3 * fft + beta_3 * pow(volume / cap2, n2);
		}
		//cout<<alpha_3<<","<<fft<<","<<beta_3<<","<<gamma_3<<","<<cap2<<endl;
		//cost = alpha_3 * fft + beta_3 * pow ((volume + (gamma_3 - 1) * rLink ->volume) / cap2, n2);

		break;
	}
	if (fabs(cost) < 1e-10) cost = 0.0;

	//if (id == 1)
	//{
	//	cost = 5.0;
	//}

}

void PTLink::UpdatePTLinkCost_const()
{
	//floatType tvol,rtvol = 0.0;
	//if(volume<0.0) 
	//{
	//	tvol = 0.0;
	//	if (fabs(volume)>1e-10)
	//	{
	//		cout<<"link id :"<<id<<" have negative flow:"<<volume<<endl;
	//	}
	//	else
	//	{
	//		volume = 0.0;
	//	}
	//}
	//else tvol = volume;
	//if (rLink)
	//{
	//	if (rLink->volume<0.0) 
	//	{
	//		//cout<<"link id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
	//		rtvol = 0.0;
	//		if (fabs(rLink->volume)>1e-10)
	//		{
	//			cout<<"link id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
	//		}
	//		else
	//		{
	//			rLink->volume = 0.0;
	//		}
	//	}
	//	else  rtvol = rLink ->volume;
	//}
	
	if (volume<0&&volume>-1e-10) volume = 0.0;
	if (volume<-1e-10)
	{
		cout<<"link id :"<<id<<" have negative flow:"<<volume<<endl;
	}

	switch(GetTransitLinkType())
    {
		case PTLink::FAILWALK: //add for failing walk link
			double alpha_11,wt1;
			alpha_11=pars[0];
			wt1=	pars[1];
			cost = alpha_11 * wt1;
			break;
		case PTLink::WALK:
			double alpha_1,wt;
			alpha_1 = pars[0];
			wt = pars[1];
			cost = alpha_1 * wt;
			break;
		case PTLink::TRANSFER:
			cost = 0.0;
			break;
		case PTLink::ABOARD:
			cost = m_hwmean * 0.5;//winnipeg
			//cost = 0.0;//other
			break;
		case PTLink::ALIGHT:
			cost = 0.1;//winnipeg
			//cost = 0.0;//other
			break;
		case PTLink::ENROUTE:
			cost = fft;
			break;
    }
	if (fabs(cost)<1e-10) cost = 0.0;

}

void PTLink::UpdatePTLinkCost()
{
	//floatType tvol,rtvol = 0.0;
	//if(volume<0.0) 
	//{
	//	tvol = 0.0;
	//	if (fabs(volume)>1e-10)
	//	{
	//		cout<<"link id :"<<id<<" have negative flow:"<<volume<<endl;
	//	}
	//	else
	//	{
	//		volume = 0.0;
	//	}
	//}
	//else tvol = volume;
	//if (rLink)
	//{
	//	if (rLink->volume<0.0) 
	//	{
	//		//cout<<"link id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
	//		rtvol = 0.0;
	//		if (fabs(rLink->volume)>1e-10)
	//		{
	//			cout<<"link id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
	//		}
	//		else
	//		{
	//			rLink->volume = 0.0;
	//		}
	//	}
	//	else  rtvol = rLink ->volume;
	//}
	
	if (volume<0&&volume>-1e-10) volume = 0.0;
	if (volume<-1e-10)
	{
		cout<<"link id :"<<id<<" have negative flow:"<<volume<<endl;
	}
	if (rLink)
	{
		if (rLink->volume<0&&rLink->volume>-1e-10) rLink->volume = 0.0;
		if (rLink->volume<-1e-10)
		{
			cout<<"rlink id :"<<rLink->id<<" have negative flow:"<<rLink->volume<<endl;
		}
	
	}

	switch(GetTransitLinkType())
    {
		case PTLink::FAILWALK: //add for failing walk link
			double alpha_11,wt1;
			alpha_11=pars[0];
			wt1=	pars[1];
			cost = alpha_11 * wt1;
			break;
		case PTLink::WALK:
			double alpha_1,wt;
			alpha_1=pars[0];
			wt=	pars[1];
			cost = alpha_1 * wt;
			//cost=fft;
			break;
		case PTLink::TRANSFER:
			cost=0.0;
			break;
		case PTLink::ABOARD:
			double alpha_2,beta_2,n1,cap1;
			alpha_2	=	pars[0];
			beta_2	=	pars[1];
			n1		=	pars[2];//power
			cap1		=	cap;				
			//cost = alpha_2 * pow (((1 - beta_2) * volume + beta_2 * rLink ->volume) / cap1 , n1);

			//cost=0.0;

			if(m_tlSym==PTLink::ASYMMTRIC)//非对称
			{
				//if (rLink->volume<0&&rLink->volume>-1e-10) rLink->volume = 0.0;
				cost = alpha_2 * pow (((1 - beta_2) * volume + beta_2 * rLink->volume) / cap1 , n1);
			}
			else
			{
				cost = alpha_2 * pow ( volume / cap1 , n1);
			}
			break;
		case PTLink::ALIGHT:
			double alpha_4,tloss;
			alpha_4=	pars[0];
			tloss =		pars[1];
			cost  = alpha_4 * tloss;
			cost = 0.1;
			break;
		case PTLink::ENROUTE:
			double alpha_3,beta_3,gamma_3,n2,cap2,fft;
			alpha_3	=	pars[0];
			beta_3	=	pars[1];
			gamma_3	=	pars[2];
			n2		=	pars[3];//power
			cap2	=	cap;
			fft		=	pars[5];

			cost=alpha_3 * fft;

			if(m_tlSym==PTLink::ASYMMTRIC)
			{
				if (rLink->volume<0&&rLink->volume>-1e-10) rLink->volume = 0.0;
				cost = alpha_3 * fft + beta_3 * pow ((volume + (gamma_3 - 1) * rLink->volume) / cap2, n2);
			}
			else
			{
				cost =  alpha_3 * fft + beta_3 * pow (volume/ cap2, n2);
			}
			//cout<<alpha_3<<","<<fft<<","<<beta_3<<","<<gamma_3<<","<<cap2<<endl;
			//cost = alpha_3 * fft + beta_3 * pow ((volume + (gamma_3 - 1) * rLink ->volume) / cap2, n2);
			
			break;
    }
	if (fabs(cost)<1e-10) cost = 0.0;
}

void PTLink::UpdatePTDerLinkCost_const()
{
	switch(GetTransitLinkType())
	{
		case PTLink::FAILWALK:
			pfdcost = 0.0;
			break;
		case PTLink::WALK:
			pfdcost = 0.0;
			//rpfdcost = 0.0;
			break;
		case PTLink::TRANSFER:
			pfdcost = 0.0;
			break;
		case PTLink::ABOARD:
			pfdcost = 0.0;
			rpfdcost = 0.0;
			break;
		case PTLink::ALIGHT:
			pfdcost = 0.0;
			break;
		case PTLink::ENROUTE:
			pfdcost = 0.0;
			rpfdcost = 0.0;
			break;
	}
}

void PTLink::UpdatePTDerLinkCost()
{
	//floatType tvol,rtvol = 0.0;
	//if(volume<0.0) tvol = 0.0;
	//else tvol = volume;
	////cout<<rtvol<<endl;
	//if (rLink)
	//{
	//	if (rLink->volume<0.0) rtvol = 0.0;
	//	else  rtvol = rLink ->volume;
	//}

	switch(GetTransitLinkType())
	{
		case PTLink::FAILWALK:
			pfdcost = 0.0;
			break;
		case PTLink::WALK:
			pfdcost = 0.0;
			//rpfdcost = 0.0;
			break;
		case PTLink::TRANSFER:
			pfdcost = 0.0;
			break;
		case PTLink::ABOARD:
			double alpha_2,beta_2,n1,cap1;
			alpha_2	=	pars[0];
			beta_2	=	pars[1];
			n1		=	pars[2];//power
			cap1	=	cap;
			if(m_tlSym == PTLink::ASYMMTRIC)
			{
				pfdcost = alpha_2 * n1 * pow (((1 - beta_2) * volume + beta_2 * rLink->volume) / cap1 , n1 - 1) * ( (1 - beta_2) / cap1);
				rpfdcost= alpha_2 * n1 * pow (((1 - beta_2) * volume + beta_2 * rLink->volume) / cap1 , n1 - 1) * (beta_2 / cap1);
			}
			else
			{
				pfdcost =  alpha_2 * n1 * pow( volume / cap1 , n1-1) * (1/cap1);
				rpfdcost = 0.0;
			}
			break;
		case PTLink::ALIGHT:
			pfdcost = 0.0;
			//rpfdcost = 0.0;
			break;
		case PTLink::ENROUTE:
			double alpha_3,beta_3,gamma_3,n2,cap2,fft;
			alpha_3	=	pars[0];
			beta_3	=	pars[1];
			gamma_3	=	pars[2];
			n2		=	pars[3];//power
			cap2	=	cap;
			fft		=	pars[5];
			if(m_tlSym==PTLink::ASYMMTRIC)
			{
				pfdcost  = beta_3 * n2 * pow ((volume + (gamma_3 - 1) * rLink->volume) / cap2, n2 - 1) * (1 / cap2);
				rpfdcost = beta_3 * n2 * pow ((volume + (gamma_3 - 1) * rLink->volume) / cap2, n2 - 1) * ((gamma_3 - 1) / cap2);
			}
			else
			{
				pfdcost  =  beta_3 * n2 * pow(volume/ cap2,n2-1)*(1/cap2);
				rpfdcost = 0.0;
			}
			break;
	}
}

void PTNET::SetLinksAttribute(bool isSym)
{
	for(int i=0;i<numOfLink;i++)
	{
		PTLink* link = linkVector[i];
		if (isSym)
			link->SetTransitLinkSym(PTLink::SYMMTRIC);//设置该link是否对称
		else
			link->SetTransitLinkSym(PTLink::ASYMMTRIC);//默认为不对称
		
		if (PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_B_BL||PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_P_GP_BL || PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_P_Greedy_BL)
			link->SetCapType(PTLink::BL);
		
		if (PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_B_TL||PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_P_GP_TL || PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_P_Greedy_TL || PCTAE_ALG==PTNET::PCTAE_algorithm::CAP_PCTAE_P_GP_eff_TL)
			link->SetCapType(PTLink::TL);//（根据算法类型，设置link的Cap类型，而BL为boarding_link；TL为transit_link）
	}

}

void PTNET::ConnectAsymmetricLinks()
{
	PTLink *link,*rlink;
	string sid;
	PTNode* node;
	for(int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		rlink=NULL;
		
		switch(link->GetTransitLinkType())
        {
			case PTLink::ABOARD:
				sid = link->m_shape->m_id;
				node = link->head->GetStopPtr()->GetNode(sid+";ENROUTE");
				for (vector<PTLink*>::iterator it=node->forwStar.begin();it!=node->forwStar.end();++it)//找到虚拟站节点出去的行驶弧
				{
					if ((*it)->GetTransitLinkType() == PTLink::ENROUTE) rlink= (*it);			
				}			
				if (!rlink)
				{
					cout<<"shpid:"<<sid<<" have wrong connection without outgoing transit links!"<<endl;
					return;
				}
				else link->rLink = rlink;
				break;
			case PTLink::ENROUTE:
				node = link->tail->GetStopPtr()->GetTransferNode();
				for (vector<PTLink*>::iterator it=node->forwStar.begin();it!=node->forwStar.end();++it)
				{
					//cout<<sid<< (*it)->m_shape->m_id;
					if ( (*it)->m_shape == link->m_shape) rlink= (*it);		
					//rlink = link->m_shape->m_boardlinks[link->seq];
				}
				if (!rlink)
				{
					cout<<"shpid:"<<sid<<" have no outgoing incoming board links!"<<endl;
					return;
				}
				else link->rLink = rlink;
				

				break;
		}
	}

}

int	PTNode::NumOfBushOutLink()
{
	int n =0;
	for(PTRTRACE pv = forwStar.begin(); pv!= forwStar.end(); pv++)
	{
		if((*pv)->m_stglink) n++;
	}
	return n;

}
int	PTNode::NumOfDestOutLink()
{
	int n =0;
	for(PTRTRACE pv = forwStar.begin(); pv!= forwStar.end(); pv++)
	{
		if((*pv)->markStatus > 0) n++;
	}
	return n;
}

void	PTNET::ComputeConvGap()
{
	floatType gap, tt =0.0, tmd = 0.0, minimumTcost = 0.0, assflowdiff = 0.0;//tt:total time
	floatType tt2 = 0;
	numaOfHyperpath = 0;
	PTDestination* dest;
	PTOrg* org;
	PTLink *link;

	//不包含waitcost（只有link_cost * link_volume）
	for (int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		tt += link->volume * link->cost;//?????
		//cout<<link->id<<" , "<<link->volume * link->cost<<endl;
	}

	//特殊处理，针对Simplicial decomposition algorithm
	if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_SD) //store the extreme link flow when implementing SPP
	{
		SCnetTTwaitcost = 0.0;
		for (int i = 0;i<numOfLink;i++)
		{
			link = linkVector[i];
			link->volume = 0.0;
		}
	}

	//针对path-based alg
	if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy	|| PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP||
		PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP ||	PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP ||//path-based alg, recompute netTTwaitcost 
		PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_TL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_TL||
		PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_BL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_BL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy
		)
	{
		netTTwaitcost = 0;//初始化
	}
	//cout<<"minpath"<<endl;
	for (int i = 0;i<numOfPTDest;i++)
	{
		dest = PTDestVector[i];
		//cout<<endl<<endl;
		{InitializeHyperpathLS(dest->destination);}
	
		{
			for (int j=0; j<dest->numOfOrg; j++)
			{
				TNM_HyperPath* path = new TNM_HyperPath();
				
				PTOrg* org= dest->orgVector[j];
				org= dest->orgVector[j];
				if(org->state)
				{
					/*cout<<"od:"<<org->org->id<<"->"<<dest->destination->id<<endl;
					path->InitializeHP(org->org,dest->destination);
					path->UpdateWaitcost();
					path->UpdateHyperpathCost();
					path->print();*/

					//cout<<dest->destination->id<<" , "<<org->org->id<<"  "<<org->org->StgElem->cost<<"   "<<org->assDemand<<"  "<<org->org->StgElem->cost * org->assDemand<<endl;
					minimumTcost += org->org->StgElem->cost * org->assDemand;//最短费用 * dmd
					/*if(org->org->StgElem->cost>100000000)
					{cout<<""<<endl;}*/
					//cout<<"cost"<<org->org->StgElem->cost<<endl;
					if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_SD)
					{
						TNM_HyperPath* path = new TNM_HyperPath();
						if (path->InitializeHP(org->org,dest->destination))
						{
							double dmd = org->assDemand;
							SCnetTTwaitcost += path->WaitCost * dmd;
							vector<GLINK*> glinks=path->GetGlinks();
							for(int i=0;i<glinks.size();++i)
							{
								glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
							}
						}
					}

					if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy	|| PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy_TP||
						PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP	|| 	PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP_TP	||//path-based alg, recompute netTTwaitcost
						PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_TL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_TL||
						PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_BL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_BL|| PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy 
					)
					{
						int k = 0;
						floatType rdemand = 0.0;
						for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();) 
						{
							TNM_HyperPath* path = *it;
							rdemand += path->flow;
							if (path->flow == 0.0) // column dropping
							{
								it = org->pathSet.erase(it);			
							}
							else
							{		
								tt2 += path->cost * path->flow;
								netTTwaitcost += path->WaitCost * path->flow;//叠加path的总等待费用
								it++;
								k++;
							}
						}
						assflowdiff += fabs(org->assDemand - rdemand);
						numaOfHyperpath += k;
						org->currentRelativeGap = 1.1;
					}
				}
			}
		}
	}
	if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)//
	{
		tt += max_w;
	}
	else
	{tt += netTTwaitcost;}
	//cout<<netTTwaitcost<<endl;

	//if(PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)//
	//{
	//	tt = 0.0;
	//	for(int i = 0;i<numOfPTDest;i++)
	//	{
	//		PTDestination* dest = PTDestVector[i];
	//		for(int j = 0;j<dest->numOfOrg;j++)
	//		{
	//			PTOrg* org = dest->orgVector[j];
	//			for(int k = 0;k<org->pathSet.size();k++)
	//			{
	//				tt += org->pathSet[k]->flow * org->pathSet[k]->cost;
	//			}
	//		}
	//	}
	//	
	//}

	if (tt==0)
	{
		GapIndicator=0;
		RGapIndicator=0;
	}
	else
	{
		if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_SD) 
		{netTTwaitcost = SCnetTTwaitcost;}
		//cout<<tt<<","<<minimumTcost<<endl;
		//cout<<"assflowdiff:"<<assflowdiff<<endl;
		gap = tt - minimumTcost;
		//if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_GP_eff)//
		//{cout<<"tt: "<<tt<<" mini: "<<minimumTcost<<" max_wait: "<<max_w<<endl;}
		//else
		//{cout<<"tt: "<<tt<<" tt2: "<<tt2<<" mini: "<<minimumTcost<<" wait: "<<netTTwaitcost<<endl;}
		GapIndicator = fabs(gap);
		RGapIndicator = fabs(gap/tt);
	}

	
}

//void	PTNET::ComputeConvGap()
//{
//	floatType gap, tt =0.0, tmd = 0.0, minimumTcost = 0.0, assflowdiff = 0.0;//tt:total time
//	numaOfHyperpath = 0;
//	PTDestination* dest;
//	PTOrg* org;
//	PTLink *link;
//	for (int i = 0;i<numOfLink;i++)
//	{
//		link = linkVector[i];
//		tt += link->volume * link->cost;
//	}
//
//	if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_SD) //store the extreme link flow when implementing SPP
//	{
//		SCnetTTwaitcost = 0.0;
//		for (int i = 0;i<numOfLink;i++)
//		{
//			link = linkVector[i];
//			link->volume = 0.0;
//		}
//	}
//
//	if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy	||
//		PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP ||	//path-based alg, recompute netTTwaitcost 
//		PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_TL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_TL||
//		PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_BL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_BL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy
//		)
//	{
//		netTTwaitcost = 0;
//	}
//
//	for (int i = 0;i<numOfPTDest;i++)
//	{
//		dest = PTDestVector[i];
//		//cout<<endl<<endl;
//		InitializeHyperpathLS(dest->destination);
//		for (int j=0; j<dest->numOfOrg; j++)
//		{
//			org= dest->orgVector[j];
//			minimumTcost += org->org->StgElem->cost * org->assDemand;
//			
//			if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_SD)
//			{
//				TNM_HyperPath* path = new TNM_HyperPath();
//				if (path->InitializeHP(org->org,dest->destination))
//				{
//					double dmd = org->assDemand;
//					SCnetTTwaitcost += path->WaitCost * dmd;
//					vector<GLINK*> glinks=path->GetGlinks();
//					for(int i=0;i<glinks.size();++i)
//					{
//						glinks[i]->m_linkPtr->volume += glinks[i]->m_data * dmd;
//					}
//				}
//			}
//
//			if (PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iGreedy || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_Greedy	||
//				PCTAE_ALG == PCTAE_algorithm::PCTAE_P_NGP || PCTAE_ALG == PCTAE_algorithm::PCTAE_P_iNGP	||	//path-based alg, recompute netTTwaitcost
//				PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_TL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_TL||
//				PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_GP_BL || PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy_BL|| PCTAE_ALG == PCTAE_algorithm::CAP_PCTAE_P_Greedy
//			)
//			{
//				int k = 0;
//				floatType rdemand = 0.0;
//				for (vector<TNM_HyperPath*>::iterator it = org->pathSet.begin(); it!=org->pathSet.end();) 
//				{
//					TNM_HyperPath* path = *it;
//					rdemand += path->flow;
//					if (path->flow == 0.0) // column dropping
//					{
//						it = org->pathSet.erase(it);			
//					}
//					else
//					{			
//						netTTwaitcost += path->WaitCost * path->flow;	
//						it++;
//						k++;
//					}
//				}
//				assflowdiff += fabs(org->assDemand - rdemand);
//				numaOfHyperpath += k;
//				org->currentRelativeGap = 1.1;
//			}
//		}
//	}
//
//	tt += netTTwaitcost;
//	if (PCTAE_ALG == PCTAE_algorithm::PCTAE_A_SD) 
//		netTTwaitcost = SCnetTTwaitcost;
//	//cout<<tt<<","<<minimumTcost<<endl;
//	//cout<<"assflowdiff:"<<assflowdiff<<endl;
//	gap = tt - minimumTcost;
//	GapIndicator = fabs(gap);
//	RGapIndicator = fabs(gap/tt);
//}

void	PTNET::RecordTEAPCurrentIter()
{
	PTITERELEM *iterElem = new PTITERELEM;
	iterElem->iter  = curIter;
	iterElem->convGap  = GapIndicator;
	iterElem->convRGap = RGapIndicator;
	iterElem->time  = 1.0 * (clock() - m_startRunTime)/CLOCKS_PER_SEC;
	iterElem->numof_infeasible_arcs = number_of_infeasible_arcs;
	//iterElem->mainlooptime = IterMainlooptime ;
	//iterElem->innerlooptime = IterInnerlooptime;
	//iterElem->numberofhyperpaths = numaOfHyperpath;
	//iterElem ->innerIters = InnerIters;
	iterRecord.push_back(iterElem); 
	//return iterElem;
}

string PTNET::GetAlgorithmName()
{
	string alg = "";
	switch(PCTAE_ALG)
	{
		case PCTAE_algorithm::PCTAE_A_GFW:
			alg = "fw";
			break;
		case PCTAE_algorithm::PCTAE_P_Greedy:
			alg = "greedy";
			break;
		case PCTAE_algorithm::PCTAE_P_NGP:
			alg = "gp";
			break;
		case PCTAE_algorithm::PCTAE_A_SD:
			alg = "sd";
			break;
		case PCTAE_algorithm::PCTAE_P_iGreedy:
			alg = "igreedy";
			break;
		case PCTAE_algorithm::PCTAE_P_iNGP:
			alg = "igp";
			break;
		case PCTAE_algorithm::PCTAE_B_DSB:
			alg = "dsb";
			break;
		case PCTAE_algorithm::CAP_PCTAE_B_TL:
			alg = "c_dsb_tl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_P_GP_TL:
			alg = "c_gp_tl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_P_Greedy_TL:
			alg = "c_greedy_tl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_B_BL:
			alg = "c_dsb_bl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_P_GP_BL:
			alg = "c_gp_bl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_P_Greedy_BL:
			alg = "c_greedy_bl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_P_GP_eff_TL:
			alg = "c_gp_eff_tl";
			break;
		case PCTAE_algorithm::CAP_PCTAE_A_MSA_eff_TL:
			alg = "c_msa_eff_tl";
			break;
		case PCTAE_algorithm::PCTAE_P_GP_eff:
			alg = "c_gp_eff";
			break;
		case PCTAE_algorithm::PCTAE_A_MSA_eff:
			alg = "c_msa_eff";
			break;
		case PCTAE_algorithm::CAP_PCTAE_A_MSA_DCL_eff_TL:
			alg = "c_msa_dcl_eff_tl";
			break;
		case PCTAE_algorithm::PCTAE_P_AON:
			alg = "AON";
			break;
		default:
			cout << "No alg defined in the solver, please check solve!" << endl;
			return "";
			break;
	}
	return alg;
}

void PTOrg::PathFlowConservation()
{
	floatType ttdemand = 0.0, maxdemand = 0.0;
	TNM_HyperPath* maxpath;
	for (vector<TNM_HyperPath*>::iterator it = pathSet.begin(); it!=pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
		ttdemand += path->flow;//计算分配到hyperpath中的总流量
		if ( path->flow >= maxdemand)//找出最大流量hyperpath
		{
			maxdemand = path->flow;
			maxpath = path;
		}
	}
	floatType dflow = assDemand -  ttdemand;
	if (dflow != 0)//如果有差异，分配到最大流量路径中
	{
		maxpath->flow += dflow;
		vector<GLINK*> glinks = maxpath->GetGlinks();
		for(int i=0; i < glinks.size(); ++i)
		{
			PTLink* plink = glinks[i]->m_linkPtr;
			double lprob = glinks[i]->m_data;
			plink->volume += lprob * dflow;
			if (abs(plink->volume) < 1e-8)
			{
				plink->volume = 0.0;
			}
			//plink ->UpdatePTLinkCost();
			//plink ->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			plink->UpdatePTLinkCost_const();
			plink->UpdatePTDerLinkCost_const();

			if (plink ->rLink)
			{
				//plink ->rLink ->UpdatePTLinkCost();
				//plink ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
				/*不考虑拥挤*/
				plink->rLink->UpdatePTLinkCost_const();
				plink->rLink->UpdatePTDerLinkCost_const();
			}
		}			
	
	}

}

void PTOrg::PathFlowConservationTP()
{
	floatType ttdemand = 0.0, maxdemand = 0.0;
	TNM_HyperPath* maxpath;
	for (vector<TNM_HyperPath*>::iterator it = pathSet.begin(); it!=pathSet.end();it++) 
	{
		TNM_HyperPath* path = *it;
		ttdemand += path->flow;
		if ( path->flow > maxdemand)
		{
			maxdemand = path->flow;
			maxpath = path;
		}
	}
	floatType dflow = assDemand -  ttdemand;
	if (dflow!=0)
	{
		maxpath->flow += dflow;
		vector<GLINK*> glinks=maxpath->GetGlinks();
		for(int i=0;i<glinks.size();++i)
		{
			PTLink* plink= glinks[i]->m_linkPtr;
			double lprob= glinks[i]->m_data;
			plink ->volume += lprob*dflow;
			if (abs(plink->volume) < 1e-8)
			{
				plink->volume = 0.0;
			}
			plink ->UpdatePTLinkCost();
			plink ->UpdatePTDerLinkCost();
			PTLink* rlink = plink ->rLink;
			if (rlink)
			{
				rlink->UpdatePTLinkCost();
				rlink->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
			}
			glinks[i]->m_gflow += glinks[i]->m_data * dflow;
			if (abs(glinks[i]->m_gflow) < 1e-8) glinks[i]->m_gflow  = 0.0;
			glinks[i]->UpdateGLinkCost();
		}
		maxpath->UpdateGLinksCost();
	}
}

//void PTOrg::PathFlowConservationTP()
//{
//	floatType ttdemand = 0.0, maxdemand = 0.0;
//	TNM_HyperPathTP* maxpath;
//	for (vector<TNM_HyperPathTP*>::iterator it = pathSetTP.begin(); it!=pathSetTP.end();it++) 
//	{
//		TNM_HyperPathTP* path = *it;
//		ttdemand += path->flow;
//		if ( path->flow >= maxdemand)
//		{
//			maxdemand = path->flow;
//			maxpath = path;
//		}
//	}
//	floatType dflow = assDemand -  ttdemand;
//	if (dflow!=0)
//	{
//		maxpath->flow += dflow;
//		vector<GLINK*> glinks=maxpath->GetGlinks();
//		for(int i=0;i<glinks.size();++i)
//		{
//			PTLink* plink= glinks[i]->m_linkPtr;
//			double lprob= glinks[i]->m_data;
//			plink ->volume += lprob*dflow;
//			if (abs(plink->volume) < 1e-8)
//			{
//				plink->volume = 0.0;
//			}
//			plink ->UpdatePTLinkCost();
//			plink ->UpdatePTDerLinkCost();
//			if (plink ->rLink)
//			{
//				plink ->rLink ->UpdatePTLinkCost();
//				plink ->rLink ->UpdatePTDerLinkCost();//current link flow variation influnce the related link cost and derivative cost
//			}
//		}			
//	
//	}
//
//
//}

void PTOrg::UpdatePathSetCost()
{
	double maxCost = - 1.0 , minCost = POS_INF_FLOAT;
	currentTotalCost = 0.0;

	for (int i =0; i < pathSet.size();i++)
	{		
		TNM_HyperPath* path= pathSet[i];
		//path->UpdateWaitcost();
		path->UpdateHyperpathCost();//每一条path上的概率并不会更新？
		if (path->cost > maxCost)	maxCost =  path->cost;

		if (path->cost < minCost)    
		{
			minCost = path->cost; 
			minIx = i; 
		}
		currentTotalCost += path->cost * path->flow;
	}

	maxPathGap = maxCost - minCost;
	currentRelativeGap = fabs( 1 - minCost * assDemand / currentTotalCost);
}

void PTOrg::ClearFlow()
{

}

void PTOrg::UpdatePathSetCostTP()
{
	double maxCost= - 1.0 , minCost=POS_INF_FLOAT;
	currentTotalCost = 0.0;

	for (int i =0; i < pathSetTP.size();i++)
	{		
		TNM_HyperPathTP* path= pathSetTP[i];
		path->UpdateHyperpathCost();
		if (path->cost> maxCost)	maxCost =   path->cost;

		if (path->cost< minCost)    
		{
			minCost = path->cost; 
			minIx = i; 
		}
		currentTotalCost += path->cost * path->flow;
	}

	maxPathGap = maxCost - minCost;
	currentRelativeGap = fabs( 1 - minCost * assDemand / currentTotalCost);
}

void PTOrg::UpdatePathSetCost_TP()
{
	double maxCost= - 1.0 , minCost=POS_INF_FLOAT;
	currentTotalCost = 0.0;

	for (int i =0; i < pathSet.size();i++)
	{		
		TNM_HyperPathTP* path= (TNM_HyperPathTP*)pathSet[i];
		path->UpdateHyperpathCost();
		if (path->cost> maxCost)	maxCost =   path->cost;

		if (path->cost< minCost)    
		{
			minCost = path->cost; 
			minIx = i; 
		}
		currentTotalCost += path->cost * path->flow;
	}

	maxPathGap = maxCost - minCost;
	currentRelativeGap = fabs( 1 - minCost * assDemand / currentTotalCost);
}

int	PTNET::GenerateRandGridAN(GRIDPTNETPAR par,string filepath)
{
	ofstream outfile,toutfile;
	srand(par.seed);
	string routefilename = filepath + networkName + "_route.txt";
	cout<<"\tWriting route information into "<<routefilename<<endl;
    if(!TNM_OpenOutFile(outfile, routefilename))
    {
        cout<<"\tCannot open file "<<routefilename<<" to write"<<endl;
        return 1;
    }
	outfile<<"route_id,route_short_name,route_long_name,route_type"<<endl;
	int rid = 1;
	for(int i = 1;i<=par.nx;i++)
	{
		outfile<<""<<rid<<","<<rid<<","<<rid<<","<<"3"<<endl;
		rid++;
		outfile<<""<<rid<<","<<rid<<","<<rid<<","<<"3"<<endl;
		rid++;
	}
	for(int i = 1;i<=par.ny;i++)
	{
		outfile<<""<<rid<<","<<rid<<","<<rid<<","<<"3"<<endl;
		rid++;
		outfile<<""<<rid<<","<<rid<<","<<rid<<","<<"3"<<endl;
		rid++;
	}
	outfile.close();

	string stopfilename = filepath + networkName + "_stop.txt";
	cout<<"\tWriting stop information into "<<stopfilename<<endl;
    if(!TNM_OpenOutFile(outfile, stopfilename))
    {
        cout<<"\tCannot open file "<<stopfilename<<" to write"<<endl;
        return 1;
    }
	outfile<<"STOPID,STOPNAME,X,Y"<<endl;
	double dist = par.gridLen;
	for(int i = 1;i<=par.nx;i++)
	{
		for (int j = 1;j<=par.ny;j++)
		{
			outfile<<""<<(i-1) * par.nx   + j <<","<<(i-1) * par.nx   + j <<","<<dist * i<<","<<dist * j<<endl;
		}
	}
	outfile.close();

	string shapefilename = filepath + networkName + "_shape.txt";
	cout<<"\tWriting shape information into "<<shapefilename<<endl;
	if(!TNM_OpenOutFile(outfile, shapefilename))
    {
        cout<<"\tCannot open file "<<shapefilename<<" to write"<<endl;
        return 1;
    }
	outfile<<"shape_id,route_id,frequency,capacity,stops"<<endl;

	string transitfilename = filepath + networkName + "_transit.txt";
	cout<<"\tWriting transit information into "<<transitfilename<<endl;
    if(!TNM_OpenOutFile(toutfile, transitfilename))
    {
        cout<<"\tCannot open file "<<routefilename<<" to write"<<endl;
        return 1;
    }
	toutfile<<"shapeid,fromstop,tostop,fft,length"<<endl;

	int a = 45, b = 5;
	// generate horizonal lines
	for(int i = 1;i<=par.nx;i++)
	{
		for (int rj = 1; rj<=2; rj++)
		{
			int routeid = 2 * i - 2 + rj;
			for (int k = 1; k<=par.commonLines;k++)
			{
				int frq = rand() % a + b;
				int cap = m_capacity * frq;
				string shapeid = to_string(routeid) + "-" + to_string(k);
				if (rj == 1)
				{
					outfile<<""<<shapeid<<","<<routeid<<","<<frq<<","<<cap<<",";
					for (int j = 1;j<par.nx;j++)
					{
						outfile<<""<<(i-1) * par.nx + j<<",";	
						double t = dist / ( 20 + rand() % 20 ) * 60.0 ;//conver to min
						//cout<<dist<<","<<t<<endl;
						toutfile<<""<<shapeid<<","<<(i-1) * par.nx + j<<","<<(i-1) * par.nx + j + 1<<","<<TNM_FloatFormat(t,4,2)<<","<<dist<<endl;
					}
					outfile<<""<<(i-1) * par.nx + par.nx<<endl;
				}
				else
				{
					outfile<<""<<shapeid<<","<<routeid<<","<<frq<<","<<cap<<",";
					for (int j = par.nx;j > 1;j--)
					{
						outfile<<""<<(i-1) * par.nx + j<<",";
						double t = dist / ( 20 + rand() % 20 ) * 60.0 ;//conver to min
						toutfile<<""<<shapeid<<","<<(i-1) * par.nx + j<<","<<(i-1) * par.nx + j - 1<<","<<TNM_FloatFormat(t,4,2)<<","<<dist<<endl;
					}
					outfile<<""<<(i-1) * par.nx + 1<<endl;
				}
			}
		}
	}
	// generate vertical lines
	for(int i = 1;i<=par.ny;i++)
	{
		for (int rj = 1; rj<=2; rj++)
		{
			int routeid = 2 * i - 2 + rj + 2 * par.nx;
			for (int k = 1; k<=par.commonLines;k++)
			{
				int frq = rand() % a + b;
				int cap = m_capacity * frq;
				string shapeid = to_string(routeid) + "-" + to_string(k);
				if (rj == 1)
				{
					outfile<<""<<shapeid<<","<<routeid<<","<<frq<<","<<cap<<",";
					for (int j = 1;j<par.ny;j++)
					{
						outfile<<""<<(j-1) * par.nx + i<<",";		
						double t = dist / ( 20 + rand() % 20 ) * 60.0 ;//conver to min
						toutfile<<""<<shapeid<<","<<(j-1) * par.nx + i<<","<<j * par.nx + i<<","<<TNM_FloatFormat(t,4,2)<<","<<dist<<endl;
					}
					outfile<<""<<(par.ny - 1) * par.nx + i<<endl;
				}
				else
				{
					outfile<<""<<shapeid<<","<<routeid<<","<<frq<<","<<cap<<",";
					for (int j = par.ny;j > 1;j--)
					{
						outfile<<""<<(j-1) * par.nx + i<<",";	
						double t = dist / ( 20 + rand() % 20 ) * 60.0 ;//conver to min
						toutfile<<""<<shapeid<<","<<(j-1) * par.nx + i<<","<<(j-2) * par.nx + i<<","<<TNM_FloatFormat(t,4,2)<<","<<dist<<endl;
					}
					outfile<<""<<i<<endl;
				}
			}
		}
	}
	outfile.close();
	toutfile.close();

	string tripfilename = filepath + networkName + "_trip.txt";
	cout<<"\tWriting trip information into "<<tripfilename<<endl;
    if(!TNM_OpenOutFile(outfile, tripfilename))
    {
        cout<<"\tCannot open file "<<tripfilename<<" to write"<<endl;
        return 1;
    }
	outfile<<"=================destination,number of origins. org id:demand==========="<<endl;
	outfile.close();

	string walkfilename = filepath + networkName + "_walks.txt";
	cout<<"\tWriting walk information into "<<tripfilename<<endl;
    if(!TNM_OpenOutFile(outfile, walkfilename))
    {
        cout<<"\tCannot open file "<<walkfilename<<" to write"<<endl;
        return 1;
    }
	outfile<<"fromstop,tostop,fft,length,capacity"<<endl;
	outfile.close();
	return 0;

}

void PTNET::GenerateRandGripTrip(GRIDPTNETPAR par)
{
	ofstream outfile;
	string tripfilename = networkName + "_trip.txt";
	floatType ttdemand = 0.0;
    if(!TNM_OpenOutFile(outfile, tripfilename))
    {
        cout<<"\tCannot open file "<<tripfilename<<" to write"<<endl;
        return;
    }
	outfile<<"=================destination,number of origins. org id:demand==========="<<endl;
	int nzones = par.nx * par.ny * par.zRatio;
	//cout<<par.zRatio<<endl;
	if (nzones > 2)
	{
		vector<PTStop*> stops, zonestop;
		for(PTStopMapIter ps = m_stops.begin(); ps!=m_stops.end();ps++)
		{
			stops.push_back(ps->second);
		}

		while (zonestop.size() < nzones)
		{
			int id = (rand() % ((stops.size() - 1) - 0 +1))+ 0;
			PTStop* sstop = stops[id];
			vector<PTStop*>::iterator fit = find(zonestop.begin(),zonestop.end(),sstop);		
			if (fit ==zonestop.end() )   zonestop.push_back(sstop);			
		}

		
		//floatType sz = 0.28 ;
		floatType sz = 1.0 / pow(nzones,0.36);

		for(int i=0;i<zonestop.size();i++)
		{
			PTStop* dstop = stops[i];
			floatType zdmd =0.0;
			for(PTShapeMapIter it = m_shapes.begin();it!= m_shapes.end();it++)
			{
				PTShape* shp = it->second;
				vector<PTStop*>  ms = shp->m_stops;
				vector<PTStop*>::iterator fit = find(ms.begin(),ms.end(),dstop);		
				if (fit !=ms.end() ) 
				{
					//cout<<shp->m_id<<","<<shp->m_cap<<endl;
					zdmd += shp->m_cap;	
				}
			}
			//cout<<sz<<","<<zdmd<<endl;
			zdmd  *= par.dLevel*sz;
			
			ttdemand += zdmd;
			floatType tdist = 0.0;
			for(int j=0;j<zonestop.size();j++)
			{
				if(j!=i)
				{
					PTStop* ostop = stops[j];
					tdist += 1.0 / sqrt ( pow(ostop->GetLat() - dstop->GetLat() , 2) + pow(ostop->GetLon() - dstop->GetLon() , 2) ) ;
				}	
			}

			outfile<<"Destination\t"<<dstop->m_id<<"\t"<<zonestop.size() - 1<<endl;
			int k = 0;
			for(int j=0;j<zonestop.size();j++)
			{
				if(j!=i)
				{
					k++;
					PTStop* ostop = stops[j];
					double pdist = 1.0 / sqrt ( pow(ostop->GetLat() - dstop->GetLat() , 2) + pow(ostop->GetLon() - dstop->GetLon() , 2));
					double demand = zdmd * pdist / tdist;

					if (k == zonestop.size() - 1)
					{
						outfile<<ostop->m_id<<":"<<demand<<endl;
					}
					else outfile<<ostop->m_id<<":"<<demand<<",";
				}	
			}
		}
	}
	cout<<"Generate total demand:"<<ttdemand<<endl;
	outfile.close();

}

TPHYPERPATHELEM::TPHYPERPATHELEM():HYPERPATHELEM()
{
	scanStatus   = 0;
	m_wait       = 0.0; 
	moneySpent   = 0.0;
	trvelTime    = 0.0;
	trvelDist    = 0.0;
	transfers    = 0;
	walkcost =0;
	curModeTravelDist = 0.0;
	tempuse      = 0.0;
	isInserted   = false;
	viaNodeLabel = NULL; 
	stgNodeLabel = NULL;
	node = NULL;
}

void TPHYPERPATHELEM::ResetLabel()
{
	scanStatus   = 0;
	m_wait       = 0.0; 
	moneySpent   = 0.0;
	trvelTime    = 0.0;
	trvelDist    = 0.0;
	curModeTravelDist = 0.0;
	tempuse      = 0.0;
	isInserted   = false;
	viaNodeLabel = NULL; 
	stgNodeLabel = NULL;
	node = NULL;
	walkcost=0;
	m_attProb.clear();
	m_attStates.clear();
	via_m_cost.clear();
	stgLabels.clear();
}

void  TPHYPERPATHELEM::PrintStgLabels()
{
	typedef vector<TPHYPERPATHELEM*>::iterator LITER;
	int ix = 0;
	cout<<endl;
	cout<<"cost: "<<cost<<endl;
	/*cout<<"tail label transfers:"<<transfers<<endl;
	for (LITER ai = stgLabels.begin();ai!= stgLabels.end();ai++)
	{
		cout<<" head label transfers:"<<(*ai)->transfers<<"   ,  "
			<<(*ai)->cost<<" link app:"<<m_attProb[ix]<<endl;
		ix ++;
	}*/
}

TPHYPERPATHELEM* PTNode::GetMinLabel()
{
	TPHYPERPATHELEM* tlabel = NULL;
	double tcost = POS_INF_FLOAT;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		if(it->second->cost<tcost)
		{
			tcost=it->second->cost;
			tlabel = it->second;
		}
	}	
	return tlabel;
}
TPHYPERPATHELEM* PTNode::GetMaxLabelOnState(int curTrans,floatType walklimit)
{
	TPHYPERPATHELEM* tlabel = NULL;
	double tcost = -POS_INF_FLOAT;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		if(it->second->transfers<=curTrans  )//&& it->second->walkcost<=walklimit
		{
			if(it->second->cost>tcost)
			{
				tcost=it->second->cost;
				tlabel = it->second;
			}
		}
	}	
	return tlabel;
}

TPHYPERPATHELEM* PTNode::GetMinLabelOnTransfers(int k,floatType walklimit)
{
	TPHYPERPATHELEM* tlabel = NULL;
	double tcost = POS_INF_FLOAT;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		if(it->second->transfers <= k && it->second->walkcost<=walklimit)
		{
			if(it->second->cost<tcost)
			{
				tcost=it->second->cost;
				tlabel = it->second;
			}
		}
	}	
	return tlabel;
}

TPHYPERPATHELEM* PTNode::GetMinLabeForFindSHPTP(int curTrans)
{
	TPHYPERPATHELEM* tlabel = NULL;
	double tcost = POS_INF_FLOAT;
	//double twalk = POS_INF_FLOAT;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		if(it->second->cost<tcost && it->second->transfers <= curTrans )
		{
			tcost=it->second->cost;
			//twalk=it->second->walkcost;
			tlabel = it->second;
		}
	}	
	return tlabel;
}

TPHYPERPATHELEM* PTNode::GetMinLabeForFindSHP(int curTrans,floatType walklimit)
{
	TPHYPERPATHELEM* tlabel = NULL;
	double tcost = POS_INF_FLOAT;
	//double twalk = POS_INF_FLOAT;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		if(it->second->cost<tcost && it->second->transfers <= curTrans   )//&& it->second->walkcost<=walklimit 
		{
			tcost=it->second->cost;
			//twalk=it->second->walkcost;
			tlabel = it->second;
		}
	}	
	return tlabel;
}

TPHYPERPATHELEM* PTNode::GetMinAcyclicLabel()
{
	TPHYPERPATHELEM* tlabel = NULL;
	double tcost = POS_INF_FLOAT;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		//GetFWLink((it)->node);
		if(it->second->vialink)
		{
			if(it->second->vialink->GetTransitLinkType()== PTLink::ENROUTE)
			{
				if(it->second->cost<tcost)
				{
					tcost=it->second->cost;
					tlabel = it->second;
				}
			}
		}
		else
		{
			/*it->second->PrintStgLabels();
			system("pause");*/
		}
	}	
	return tlabel;
}

TPHYPERPATHELEM* PTNode::GetLabel(const string &ts)
{
	LabelsMapIter  it = m_labels->find(ts);
	if(it!=m_labels->end())
	{
		return (it->second);
	}
	else
	{
		//cout<<"not find the label with transfer-state piar:"<<ts<<endl;
		return NULL;
	}
}

void PTNode::PrintNodeMapLables()
{
	TPHYPERPATHELEM* curLabel;
	int n =0;
	cout<<"Node Id:"<<id<<endl;
	for(LabelsMapIter  it = m_labels->begin();it !=m_labels->end();it++)
	{
		n++;
		curLabel = it->second;
		cout<<n<<"-th label, Cost:"<<curLabel->cost<<", time:"<<curLabel->trvelTime<<", distance:"<<curLabel->trvelDist
			<<", money:"<<curLabel->moneySpent<<", Transfers:"<<curLabel->transfers<<endl;
		//curLabel->PrintStgLabels();
	}	
	cout<<endl;
}

int PTNode::InsertLabel(TPHYPERPATHELEM* pElem)
{

	string ts =to_string(pElem->transfers);//+"-"+to_string(pElem->walkcost)
	
	pair<map<string,TPHYPERPATHELEM*>::iterator,bool> ret = m_labels->insert(pair<string,TPHYPERPATHELEM*>(ts,pElem));
	if(!ret.second)
	{
		cout<<"The label not be inserted the tail labelMaps！ Node id:"<<pElem->node->id<<
			" transfers:"<<pElem->transfers <<" label cost:"<<pElem->cost<<"label walkcost: "<<pElem->walkcost<<endl;
		cout<<"The tail have following labels:"<<endl;
		//PrintNodeMapLables();
		system("pause"); 
	}
	return 0;
}

void PTOrg::UpdatePathMax_w()
{
	for (int i = 0; i < pathSet.size();i++)
	{		
		TNM_HyperPath* path= pathSet[i];
		path->Max_w = 0.0;
		for(int j = 0;j<path->m_nodes.size();j++)
		{
			PTNode* node = path->m_nodes[j]->m_ptnodePtr;
			if(node->GetTransitNodeType() == PTNode::TRANSFER)
			{
				floatType temp = 0.0;
				for(int k = 0;k<path->m_nodes[j]->node_link.size();k++)
				{
					PTLink* link = path->m_nodes[j]->node_link[k]->m_linkPtr;
					if(link->GetTransitLinkType() == PTLink::ABOARD)
					{
						temp = max(temp,path->flow * path->m_nodes[j]->node_link[k]->m_data/link->efffreq);//????????
					}
				}
				path->Max_w += temp;
			}
		}
	}
}

void PTOrg::Pretreatment()
{
	/*double dflow = pathSet[0]->flow/2.0;*/
	vector<TNM_HyperPath*> temp;
	int index = 0;
	floatType tempflow = 0.0;
	for(int i = 0;i<pathSet.size();i++)
	{
		TNM_HyperPath* temppath = pathSet[i];
		if(temppath->flow == 0)
		{
			temp.push_back(temppath);
		}
		else if(temppath->flow > tempflow)
		{
			index = i;
			tempflow = temppath->flow;
		}
	}

	TNM_HyperPath* MaxFlowpath = pathSet[index];
	
	MaxFlowpath->Preflow = MaxFlowpath->flow;
	//TNM_HyperPath* path = pathSet[0];
	double dflow = MaxFlowpath->flow/(temp.size()+1);

	vector<GLINK*> mglinks=MaxFlowpath->GetGlinks();
	
	PTLink *link,*rlink;

	for(int i = 0;i<temp.size();i++)
	{
		TNM_HyperPath* path = temp[i];

		path->Preflow = path->flow;

		vector<GLINK*> glinks= path->GetGlinks();

		MaxFlowpath->Preflow = MaxFlowpath->flow;

		MaxFlowpath->flow = MaxFlowpath->flow - dflow; 	

		path->flow = path->flow + dflow; 	

		if (path->flow<0||MaxFlowpath->flow<0)
		{
			//cout<<"wrong!!! "<<path->flow<<","<<MinCostpath->flow<<","<<dflow<<","<<DerSum<<","<<path->ComputePdfWaitcost()<<","<<MinCostpath->ComputePdfWaitcost()<<endl;
			system("PAUSE");
		}
		
		MaxFlowpath->loadingflow();
		path->loadingflow();
		
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
	

	PathFlowConservation_eff();
}

//int PTNET::ComputePdfWaitcost()
//{
//	
//}

//int	GNODE::SolveFixedPoint()
//{
//	if(node_link.size() == 1)
//	{
//		node_link[0]->prevolume = flow;
//		node_link[0]->volume = flow;
//		node_link[0]->m_linkPtr->volume += flow;
//		if(node_link[0]->m_linkPtr->rLink)
//		{
//			node_link[0]->m_linkPtr->rLink->volume += flow;
//		}
//
//		node_link[0]->m_head->flow += flow;
//
//		node_link[0]->m_data = m_data;
//		node_link[0]->m_head->m_data += node_link[0]->m_data;
//
//		//node_link[0]->m_linkPtr->UpdatePTLinkCost();
//		node_link[0]->m_linkPtr->UpdateEffectiveFreq();
//		if(node_link[0]->m_linkPtr->rLink)
//		{
//			node_link[0]->m_linkPtr->rLink->volume -= flow;
//			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
//		}
//
//
//
//		return 0;
//	}
//
//	//initial
//	floatType ttf = 0.0;
//	for(int i = 0;i<node_link.size();i++)
//	{
//		GLINK* glink =  node_link[i];
//		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -0.0001)
//		{
//			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
//			glink->m_linkPtr->volume = 0;
//		}
//		glink->m_linkPtr->UpdateEffectiveFreq();
//		if(glink->m_linkPtr->efffreq < 0.0001)
//		{
//			//cout<<"slove wrong"<<endl;
//		}
//		ttf += glink->m_linkPtr->efffreq;
//	}
//
//	for(int i = 0;i<node_link.size();i++)
//	{
//		GLINK* glink =  node_link[i];
//		glink->prevolume = glink->m_linkPtr->efffreq / ttf * this->flow;
//		glink->volume = glink->m_linkPtr->efffreq / ttf * this->flow;
//		glink->m_linkPtr->volume += glink->prevolume;
//		if(glink->m_linkPtr->rLink)
//		{
//			glink->m_linkPtr->rLink->volume += glink->prevolume;
//		}
//		glink->m_data = glink->m_linkPtr->efffreq / ttf;
//		glink->m_head->flow += glink->prevolume;
//	}
//
//	//loop
//	int count = 0;
//	while(true)
//	{
//		for(int i = 0;i<node_link.size();i++)
//		{
//			GLINK* glink =  node_link[i];
//			glink->m_linkPtr->UpdateEffectiveFreq();
//		}
//
//		//gap
//		count++;
//		GLINK* glink =  node_link[0];
//		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
//		floatType gap = 0.0;
//		for(int i = 0;i<node_link.size();i++)
//		{
//			GLINK* glink =  node_link[i];
//			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
//			gap += pow(aa-temp,2);
//		}
//
//		if(gap < 0.000001 || count > 100)
//		{
//			for(int i = 0;i<node_link.size();i++)
//			{
//				GLINK* glink =  node_link[i];
//				//glink->m_head->flow = glink->volume;//?????
//				//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
//				/*if(glink->m_linkPtr->rLink)
//				{
//					glink->m_linkPtr->rLink->volume -= glink->volume;
//				}*/
//				if(glink->volume < 0 && glink->volume > -0.000000001)
//				{
//					glink->m_linkPtr->volume -= glink->volume;
//
//					glink->volume = 0;
//				}
//
//				glink->m_data = m_data * glink->m_data;
//				glink->m_head->m_data += glink->m_data;
//
//				//glink->m_linkPtr->UpdatePTLinkCost();
//				glink->m_linkPtr->UpdateEffectiveFreq();
//				if(glink->m_linkPtr->rLink)
//				{
//					glink->m_linkPtr->rLink->volume -= glink->volume;
//					//glink->m_linkPtr->rLink->UpdatePTLinkCost();
//				}
//			}
//
//			//cout<<count<<endl;
//			return 0;
//		}
//
//		//update m_data
//		floatType ttfreq = 0.0;
//		for(int i = 0;i<node_link.size();i++)
//		{
//			GLINK* glink =  node_link[i];
//			ttfreq += glink->m_linkPtr->efffreq;
//		}
//		for(int i = 0;i<node_link.size();i++)
//		{
//			GLINK* glink =  node_link[i];
//			glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
//		}
//
//		//line search
//
//
//		//loading
//		for(int i = 0;i<node_link.size();i++)
//		{
//			GLINK* glink =  node_link[i];
//
//			glink->volume = glink->m_data * this->flow;
//
//			glink->m_linkPtr->volume += glink->volume - glink->prevolume;
//			/*if(glink->m_linkPtr->volume < 0)
//			{
//				cout<<endl;
//			}*/
//			if(glink->m_linkPtr->rLink)
//			{
//				glink->m_linkPtr->rLink->volume += glink->volume - glink->prevolume;
//			}
//
//			glink->m_head->flow += glink->volume - glink->prevolume;
//
//			glink->prevolume = glink->volume;
//		}
//
//
//	}
//}

int	GNODE::SolveFPwithMSA_gcost(floatType lambda)
{
	/*如果只有一条上车弧*/
	if(node_link.size() == 1)
	{
		//加载流量更新概率
		node_link[0]->prevolume = node_link[0]->volume;
		node_link[0]->volume = flow;
		node_link[0]->m_linkPtr->volume += node_link[0]->volume - node_link[0]->prevolume;
		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += node_link[0]->volume - node_link[0]->prevolume;
		}

		node_link[0]->m_head->flow += node_link[0]->volume - node_link[0]->prevolume;

		node_link[0]->m_data = m_data;
		node_link[0]->m_head->m_data += node_link[0]->m_data;

		node_link[0]->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//这里与原版本代码不同（其实可加可不加）
		node_link[0]->m_linkPtr->UpdateEffectiveFreq();
		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume -= node_link[0]->volume - node_link[0]->prevolume;
			node_link[0]->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//这里与原版本代码不同
		}

		return 0;
	}

	/*如果有两条及两条以上的上车弧*/
	floatType ttf = 0.0;
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -0.0001)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
		glink->m_linkPtr->UpdateEffectiveFreq();
		if(glink->m_linkPtr->efffreq < 0.0001)
		{
			//cout<<endl;
		}
		ttf += glink->m_linkPtr->efffreq;
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		glink->prevolume = glink->volume;//glink->m_linkPtr->efffreq / ttf * this->flow;
		glink->volume = glink->m_linkPtr->efffreq / ttf * this->flow;
		glink->m_linkPtr->volume += glink->volume - glink->prevolume;
		if(glink->m_linkPtr->rLink)
		{
			glink->m_linkPtr->rLink->volume += glink->volume - glink->prevolume;
		}
		glink->m_data = glink->m_linkPtr->efffreq / ttf;
		glink->m_head->flow += glink->volume - glink->prevolume;
	}

	//主循环
	int count = 0;
	double alaph;//MSA中的参数alpha
	while(true)
	{
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			glink->m_linkPtr->UpdateEffectiveFreq();
		}

		//计算误差
		count++;
		GLINK* glink =  node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		floatType gap = 0.0;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			gap += pow(aa - temp,2);
		}

		//满足收敛条件，退出循环
		if(gap < 0.000001 || count > 100)
		{
			for(int i = 0; i < node_link.size(); i++)
			{
				GLINK* glink =  node_link[i];
				//glink->m_head->flow = glink->volume;//?????
				//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
				/*if(glink->m_linkPtr->rLink)
				{
					glink->m_linkPtr->rLink->volume -= glink->volume;
				}*/
				if(glink->volume < 0 && glink->volume > -0.000000001)
				{
					glink->m_linkPtr->volume -= glink->volume;

					glink->volume = 0;
				}

				glink->m_data = m_data * glink->m_data;
				glink->m_head->m_data += glink->m_data;

				glink->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//这里与原版本代码不同（其实可加可不加）
				glink->m_linkPtr->UpdateEffectiveFreq();
				if(glink->m_linkPtr->rLink)
				{
					glink->m_linkPtr->rLink->volume -= glink->volume;
					glink->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//这里与原版本代码不同（其实可加可不加）
				}
				//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
			}

			//cout<<count<<endl;
			return 0;
		}

		//更新link在站点处的使用概率
		floatType ttfreq = 0.0;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			ttfreq += glink->m_linkPtr->efffreq;
		}
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
		}

		//加载流量（link，相关link，link的头节点）
		alaph = 1.0/count;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];

			glink->prevolume = glink->volume;
			glink->volume = (1 - alaph) * glink->volume + alaph * glink->m_data * this->flow;//（1-a） * va^k-1 + a * va^s

			glink->m_linkPtr->volume += glink->volume - glink->prevolume;
			/*if(glink->m_linkPtr->volume < 0)
			{
				cout<<endl;
			}*/
			if(glink->m_linkPtr->rLink)
			{
				glink->m_linkPtr->rLink->volume += glink->volume - glink->prevolume;
			}

			glink->m_head->flow += glink->volume - glink->prevolume;

			//glink->prevolume = glink->volume;
		}
	}
}

int	GNODE::SolveFixedPoint()
{
	if(node_link.size() == 1)
	{
		node_link[0]->prevolume = node_link[0]->volume;
		node_link[0]->volume = flow;
		node_link[0]->m_linkPtr->volume += node_link[0]->volume - node_link[0]->prevolume;
		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += node_link[0]->volume - node_link[0]->prevolume;
		}

		node_link[0]->m_head->flow += node_link[0]->volume - node_link[0]->prevolume;

		node_link[0]->m_data = m_data;
		node_link[0]->m_head->m_data += node_link[0]->m_data;

		//node_link[0]->m_linkPtr->UpdatePTLinkCost();
		node_link[0]->m_linkPtr->UpdateEffectiveFreq();
		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume -= node_link[0]->volume - node_link[0]->prevolume;
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
		}

		return 0;
	}

	//initial
	floatType ttf = 0.0;
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -0.0001)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
		glink->m_linkPtr->UpdateEffectiveFreq();
		if(glink->m_linkPtr->efffreq < 0.0001)
		{
			//cout<<endl;
		}
		ttf += glink->m_linkPtr->efffreq;
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		glink->prevolume = glink->volume;//glink->m_linkPtr->efffreq / ttf * this->flow;
		glink->volume = glink->m_linkPtr->efffreq / ttf * this->flow;
		glink->m_linkPtr->volume += glink->volume - glink->prevolume;
		if(glink->m_linkPtr->rLink)
		{
			glink->m_linkPtr->rLink->volume += glink->volume - glink->prevolume;
		}
		glink->m_data = glink->m_linkPtr->efffreq / ttf;
		glink->m_head->flow += glink->volume - glink->prevolume;
	}

	//loop
	int count = 0;
	double alaph;//MSA中的参数alpha
	while(true)
	{
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			glink->m_linkPtr->UpdateEffectiveFreq();
		}

		//gap
		count++;
		GLINK* glink =  node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		floatType gap = 0.0;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			gap += pow(aa-temp,2);
		}

		//满足收敛条件，退出循环
		if(gap < 0.000001 || count > 100)
		{
			for(int i = 0;i<node_link.size();i++)
			{
				GLINK* glink =  node_link[i];
				//glink->m_head->flow = glink->volume;//?????
				//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
				/*if(glink->m_linkPtr->rLink)
				{
					glink->m_linkPtr->rLink->volume -= glink->volume;
				}*/
				if(glink->volume < 0 && glink->volume > -0.000000001)
				{
					glink->m_linkPtr->volume -= glink->volume;

					glink->volume = 0;
				}

				glink->m_data = m_data * glink->m_data;
				glink->m_head->m_data += glink->m_data;

				//glink->m_linkPtr->UpdatePTLinkCost();
				glink->m_linkPtr->UpdateEffectiveFreq();
				if(glink->m_linkPtr->rLink)
				{
					glink->m_linkPtr->rLink->volume -= glink->volume;
					//glink->m_linkPtr->rLink->UpdatePTLinkCost();
				}
				//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
			}

			//cout<<count<<endl;
			return 0;
		}

		//update m_data
		floatType ttfreq = 0.0;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			ttfreq += glink->m_linkPtr->efffreq;
		}
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
		}

		//line search

		//loading
		alaph = 1.0/count;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];

			glink->prevolume = glink->volume;
			glink->volume = (1-alaph) * glink->volume + alaph * glink->m_data * this->flow;//（1-a） * va^k-1 + a * va^s

			glink->m_linkPtr->volume += glink->volume - glink->prevolume;
			/*if(glink->m_linkPtr->volume < 0)
			{
				cout<<endl;
			}*/
			if(glink->m_linkPtr->rLink)
			{
				glink->m_linkPtr->rLink->volume += glink->volume - glink->prevolume;
			}

			glink->m_head->flow += glink->volume - glink->prevolume;

			//glink->prevolume = glink->volume;
		}

	}
}

int	GNODE::SolveFPwithGP_gcost(floatType lambda)
{
	/*如果只有一条上车弧*/
	if(node_link.size() == 1)
	{
		node_link[0]->prevolume = flow;//由节点的流量（ = hyperpath到达该节点的流量） 加载到对应的glink中
		node_link[0]->volume = flow; //加载hyperpath的流量(flow)
		node_link[0]->m_linkPtr->volume += flow;//由glink加载到ptlink中

		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
		}

		node_link[0]->m_head->flow += flow;//更新头节点的进入流量

		node_link[0]->m_data = m_data;//更新link的使用概率（因为只有一条boarding_link）
		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

		if(node_link[0]->m_linkPtr->volume < -1e-10)
		{cout<<endl;}
		//node_link[0]->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//更新弧的general_cost，以及general_cost的一阶导
		
		/*cost为常数*/
		node_link[0]->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost_const(lambda);//更新弧的general_cost，以及general_cost的一阶导

		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

		if(node_link[0]->m_linkPtr->rLink)
		{
			if(node_link[0]->m_linkPtr->rLink->volume < -1e-10)
			{cout<<endl;}
			node_link[0]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
			//node_link[0]->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);
			
			/*cost为常数*/
			node_link[0]->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost_const(lambda);//更新弧的general_cost，以及general_cost的一阶导
			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		return 0;
	}

	/*如果有两条及以上的上车弧*/
	//初始化
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink = node_link[i];
		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -1e-11)//如果boarding_link上有数值，更新当前link和rlink(当前link初始化为0；rlink增加对应的数值)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}

	//均分站点流量，加载至各条link和link的头节点中
	for(int i = 0;i<node_link.size();i++)
	{
		node_link[i]->volume = flow / node_link.size();
		node_link[i]->prevolume = flow / node_link.size();
		node_link[i]->m_head->flow += node_link[i]->prevolume;
		node_link[i]->m_linkPtr->volume += flow / node_link.size();
		if(node_link[i]->m_linkPtr->rLink)
		{
			node_link[i]->m_linkPtr->rLink->volume += flow / node_link.size();
		}
	}

	//计算各条link的“费用”
	for(int i = 0;i<node_link.size();i++)
	{
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr（VI问题的cost）
	}
	
	//主循环
	floatType gap = 0.0;
	int count = 0;
	do
	{
		//更新link的“费用”
		for(int i = 0;i<node_link.size();i++)
		{
			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
		}
		//找出最小cost的link
		floatType mincost = 1e15;
		int minindex;
		for(int i = 0;i<node_link.size();i++)
		{
			if(node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

		//GP（GP法转移流量）
		for(int i = 0;i<node_link.size();i++)
		{
			if(i != minindex)
			{
				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
				node_link[i]->m_data = node_link[i]->volume / flow;

				node_link[minindex]->prevolume = node_link[minindex]->volume;
				node_link[i]->prevolume = node_link[i]->volume;

				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
				node_link[i]->UpdateDer();

				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq
				if (Der == 0)	Der = 1e-8;
				double dev = node_link[i]->cost - node_link[minindex]->cost;
				double dflow;
				if (dev >= 0) 
				{
					dflow = __min(1.0 * dev /Der, node_link[i]->volume);
					//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
				}
				else
				{
					dflow = __max(1.0 * dev /Der, -node_link[minindex]->volume);	
					//cout<<1.0 * dev /DerSum<<","<< MinCostpath->flow<<",dflow"<<dflow<<endl;
				}

				node_link[i]->volume = node_link[i]->prevolume - dflow;
				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

				if (node_link[i]->volume < 0||node_link[minindex]->volume < 0)
				{
					cout<<node_link[i]->prevolume<<","<<node_link[minindex]->prevolume<<","<<node_link[i]->der<<","<<node_link[minindex]->der<<","<<node_link[i]->cost<<","<<node_link[minindex]->cost<<endl;
					system("PAUSE");
				}

				//load（加载流量，包括相关link，以为计算有效发车频率）
				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
				if(node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->volume - node_link[i]->prevolume;
				}

				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
				if(node_link[minindex]->m_linkPtr->rLink)
				{
					node_link[minindex]->m_linkPtr->rLink->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
				}

				//加载至link的头节点中
				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

				//更新有效发车频率
				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/

			}
		}

		//compute gap
		count++;
		GLINK* glink =  node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		gap = 0.0;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			gap += pow(aa - temp, 2);
		}

	}while(gap > 0.00001 && count < 50);//1000(100)(正常选的50？100) SF(1.2):1000!!!

	//更新这条path上所有节点和link的概率
	floatType ttfreq = 0.0;
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
	}

	//更新link在站点处的使用概率
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	//更新link和m_head在hpath中的使用概率
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];

		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		if(glink->m_linkPtr->volume < -1e-10)
		{cout<<endl;}
		//glink->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//更新link的general_cost

		/*cost为常数*/
		glink->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost_const(lambda);

		glink->m_linkPtr->UpdateEffectiveFreq();
		
		if(glink->m_linkPtr->rLink)
		{
			glink->m_linkPtr->rLink->volume -= glink->volume;//为什么？？？-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数

			if(glink->m_linkPtr->rLink->volume < -1e-10)
			{cout<<endl;}

			//glink->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);

			/*cost为常数*/
			glink->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost_const(lambda);
		}
	}

	return 0;
}

//int	GNODE::SolveFPwithGP()
//{
//	if (node_link.size() == 1)
//	{
//		node_link[0]->prevolume = flow;//由节点的流量（ = hyperpath到达该节点的流量） 加载到对应的glink中
//		node_link[0]->volume = flow; //加载hyperpath的流量(flow)
//		node_link[0]->m_linkPtr->volume += flow;//由glink加载到ptlink中
//		if (node_link[0]->m_linkPtr->rLink)
//		{
//			node_link[0]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
//		}
//
//		node_link[0]->m_head->flow += flow;//更新头节点的进入流量
//
//		node_link[0]->m_data = m_data;//更新link的使用概率（因为只有一条boarding_link）
//		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）
//
//		if (node_link[0]->m_linkPtr->volume < -1e-10)
//		{
//			cout << endl;
//		}
//		//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
//		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导
//
//		/*不考虑拥挤*/
//		//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
//		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();
//
//		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率
//
//		if (node_link[0]->m_linkPtr->rLink)
//		{
//			if (node_link[0]->m_linkPtr->rLink->volume < -1e-10)
//			{
//				cout << endl;
//			}
//			node_link[0]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
//			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
//			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();
//
//			/*不考虑拥挤*/
//			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
//			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();
//
//			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
//		}
//		return 0;
//	}
//
//
//
//	//init
//
//	for (int i = 0; i < node_link.size(); i++)
//	{
//		GLINK* glink = node_link[i];
//		if (glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -1e-11)//如果boarding_link上有数值，更新当前link和rlink(当前link初始化为0；rlink增加对应的数值)
//		{
//			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
//			glink->m_linkPtr->volume = 0;
//		}
//	}
//
//
//	//floatType RATIO = 1.0 / 20.0;
//	for (int i = 0; i < node_link.size(); i++)
//	{
//		node_link[i]->volume = flow / node_link.size();
//		node_link[i]->prevolume = flow / node_link.size();
//
//		/*just ceshi*/
//		//if (i == 0)
//		//{
//		//	node_link[i]->volume = flow * RATIO;
//		//	node_link[i]->prevolume = flow * RATIO;
//		//}
//
//		//if (i == 1)
//		//{
//		//	node_link[i]->volume = flow * (1.0 - RATIO);
//		//	node_link[i]->prevolume = flow * (1.0 - RATIO);
//		//}
//
//		//node_link[i]->volume = flow * (2.0/3.0);
//		//node_link[i]->prevolume = flow / (1.0 / 3.0);
//
//		node_link[i]->m_head->flow += node_link[i]->prevolume;
//		node_link[i]->m_linkPtr->volume += flow / node_link.size();
//		if (node_link[i]->m_linkPtr->rLink)
//		{
//			node_link[i]->m_linkPtr->rLink->volume += flow / node_link.size();
//		}
//
//		/*just ceshi*/
//		//if (i == 0)
//		//{
//		//	node_link[i]->m_linkPtr->volume += flow * (RATIO);
//		//	if(node_link[i]->m_linkPtr->rLink)
//		//	{
//		//		node_link[i]->m_linkPtr->rLink->volume += flow * (RATIO);
//		//	}
//		//}
//
//		//if (i == 1)
//		//{
//		//	node_link[i]->m_linkPtr->volume += flow * (1.0 - RATIO);
//		//	if (node_link[i]->m_linkPtr->rLink)
//		//	{
//		//		node_link[i]->m_linkPtr->rLink->volume += flow * (1.0 - RATIO);
//		//	}
//		//}
//
//
//	}
//
//	//cout << "初始: " << endl;
//	for (int i = 0; i < node_link.size(); i++)
//	{
//		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
//		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr（VI问题的cost）
//
//		//GLINK* glink = node_link[i];
//
//		//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << "flow: "<< glink->m_linkPtr->volume<<" efffreq: " << glink->m_linkPtr->efffreq << " cost: " << glink->cost << endl;
//	}
//	//cout << "-----------------------------------------------" << endl;
//
//	//main loop
//	floatType gap = 0.0;
//	int count = 0;
//	do
//	{
//		for (int i = 0; i < node_link.size(); i++)
//		{
//			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
//		}
//		//update cost（找出最小cost的b_link）
//		floatType mincost = 1e25;
//		int minindex;
//		for (int i = 0; i < node_link.size(); i++)
//		{
//			if (node_link[i]->cost < mincost)
//			{
//				mincost = node_link[i]->cost;
//				minindex = i;
//			}
//		}
//
//		//cout << "Iter: " << count << "----------------------------------" << endl;
//		//GP（GP法转移流量）
//		for (int i = 0; i < node_link.size(); i++)
//		{
//
//			if (i != minindex)
//			{
//				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
//				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
//
//				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
//				node_link[i]->m_data = node_link[i]->volume / flow;
//
//				node_link[minindex]->prevolume = node_link[minindex]->volume;
//				node_link[i]->prevolume = node_link[i]->volume;
//
//				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
//				node_link[i]->UpdateDer();
//
//
//				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq
//				if (Der == 0)	Der = 1e-8;
//				double dev = node_link[i]->cost - node_link[minindex]->cost;
//				double dflow;
//				if (dev >= 0)
//				{
//					dflow = __min(1.0 * dev / Der, node_link[i]->volume);
//					//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
//				}
//				else
//				{
//					dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
//					//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
//				}
//
//				node_link[i]->volume = node_link[i]->prevolume - dflow;
//				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;
//
//				if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
//				{
//					//cout<<"yes!!!!!!!"<<endl;
//					//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;
//
//
//					//cout<<"-------------------"<<endl;
//					//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;
//
//					//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
//					//cout<<"CESHI: "<<CESHI1<<endl;
//
//
//					////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)
//
//					//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
//					//cout<<"-------------------"<<endl;
//
//					//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;
//
//					//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);
//
//					//cout<<"ceshi: "<<ceshi<<endl;
//
//					cout << node_link[i]->prevolume << "," << node_link[minindex]->prevolume << "," << node_link[i]->der << "," << node_link[minindex]->der << "," << node_link[i]->cost << "," << node_link[minindex]->cost << endl;
//					system("PAUSE");
//				}
//
//				//load（加载流量）
//				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
//				if (node_link[i]->m_linkPtr->rLink)
//				{
//					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->volume - node_link[i]->prevolume;
//				}
//
//				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
//				if (node_link[minindex]->m_linkPtr->rLink)
//				{
//					node_link[minindex]->m_linkPtr->rLink->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
//				}
//
//
//				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
//				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;
//
//				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
//				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
//
//				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
//				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
//				//GLINK* glink = node_link[i];
//				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
//			}
//			//GLINK* glink = node_link[i];
//
//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
//
//		}
//
//		//compute gap
//		count++;
//		GLINK* glink = node_link[0];
//		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
//		glink->gap = aa;
//		gap = 0.0;
//		for (int i = 0; i < node_link.size(); i++)
//		{
//			GLINK* glink = node_link[i];
//			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
//			glink->gap = temp;
//
//			gap += pow(aa - temp, 2);
//
//			//GLINK* glink = node_link[i];
//
//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->m_linkPtr->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
//		}
//		//cout <<"GAP: "<< gap << endl;
//
//	} while (gap > 0.00001 && count < 100);//count = 1000; gap > 0.00001 (best parameter: 100)  常规是50 SF（1.8）：100    ****(SF(1.2/1.4): gap > 0.0000001 && count < 100)****
//
//	//if (gap > 0.0000001)
//	//{
//	//	//cout << "gap is not satisfied!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << endl;
//	//	for (int i = 0; i < node_link.size(); i++)
//	//	{
//	//		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
//	//	}
//	//	//update cost（找出最小cost的b_link）
//	//	floatType mincost = 1e25;
//	//	int minindex;
//	//	for (int i = 0; i < node_link.size(); i++)
//	//	{
//	//		if (node_link[i]->cost < mincost)
//	//		{
//	//			mincost = node_link[i]->cost;
//	//			minindex = i;
//	//		}
//	//	}
//
//	//	for (int i = 0; i < node_link.size(); i++)
//	//	{
//
//	//		if (i != minindex)
//	//		{
//	//			node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
//	//			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
//
//	//			node_link[minindex]->m_data = node_link[minindex]->volume / flow;
//	//			node_link[i]->m_data = node_link[i]->volume / flow;
//
//	//			node_link[minindex]->prevolume = node_link[minindex]->volume;
//	//			node_link[i]->prevolume = node_link[i]->volume;
//
//	//			node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
//	//			node_link[i]->UpdateDer();
//
//
//	//			floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq
//	//			if (Der == 0)	Der = 1e-8;
//	//			double dev = node_link[i]->cost - node_link[minindex]->cost;
//	//			double dflow;
//	//			if (dev >= 0)
//	//			{
//	//				dflow = __min(1.0 * dev / Der, node_link[i]->volume);
//	//				//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
//	//			}
//	//			else
//	//			{
//	//				dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
//	//				//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
//	//			}
//
//	//			//cout << "dflow: " << dflow << endl;
//
//	//			node_link[i]->volume = node_link[i]->prevolume - dflow;
//	//			node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;
//
//
//	//			//load（加载流量）
//	//			node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
//	//			if (node_link[i]->m_linkPtr->rLink)
//	//			{
//	//				node_link[i]->m_linkPtr->rLink->volume += node_link[i]->volume - node_link[i]->prevolume;
//	//			}
//
//	//			node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
//	//			if (node_link[minindex]->m_linkPtr->rLink)
//	//			{
//	//				node_link[minindex]->m_linkPtr->rLink->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
//	//			}
//
//
//	//			node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
//	//			node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;
//
//	//			node_link[i]->m_linkPtr->UpdateEffectiveFreq();
//	//			node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
//	//		}
//	//		
//	//	}
//	//}
//
//	//update m_data（更新概率）
//	floatType ttfreq = 0.0;
//	for (int i = 0; i < node_link.size(); i++)
//	{
//		GLINK* glink = node_link[i];
//		ttfreq += glink->m_linkPtr->efffreq;
//	}
//
//	for (int i = 0; i < node_link.size(); i++)
//	{
//		GLINK* glink = node_link[i];
//		glink->effective_freq = glink->m_linkPtr->efffreq;
//		glink->temp_value = glink->m_linkPtr->efffreq / ttfreq;
//		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
//	}
//
//
//	for (int i = 0; i < node_link.size(); i++)
//	{
//		GLINK* glink = node_link[i];
//		//glink->m_head->flow = glink->volume;//?????
//		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
//		//glink->m_head->flow += glink->volume - glink->prevolume;
//
//
//
//
//		glink->m_data = m_data * glink->m_data;
//		glink->m_head->m_data += glink->m_data;
//
//		if (glink->m_linkPtr->volume < -1e-10)
//		{
//			cout << endl;
//		}
//		//glink->m_linkPtr->UpdatePTLinkCost();
//		//glink->m_linkPtr->UpdatePTDerLinkCost();
//
//		/*不考虑拥挤*/
//		//glink->m_linkPtr->UpdatePTLinkCost_const();
//		//glink->m_linkPtr->UpdatePTDerLinkCost_const();
//
//		glink->m_linkPtr->UpdateEffectiveFreq();
//
//		if (glink->m_linkPtr->rLink)
//		{
//			//cout<<glink->m_linkPtr->rLink->volume<<endl;
//			glink->m_linkPtr->rLink->volume -= glink->volume;//为什么？？？-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
//
//			if (glink->m_linkPtr->rLink->volume < -1e-10)
//			{
//				cout << endl;
//			}
//
//			//glink->m_linkPtr->rLink->UpdatePTLinkCost();
//			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost();
//
//			/*不考虑拥挤*/
//			//glink->m_linkPtr->rLink->UpdatePTLinkCost_const();
//			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost_const();
//
//			//glink->m_linkPtr->rLink->UpdateEffectiveFreq();
//		}
//		//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
//	}
//
//	return 0;
//}

int	GNODE::SolveFPwithGP_test_shuchu()
{
	//if (flow == 0 || flow < 1e-15)
	if (flow == 0 || ((flow < 1e-15) && (flow > 0)))
	{
		floatType ttfreq = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->m_linkPtr->UpdateEffectiveFreq();
			ttfreq += node_link[i]->m_linkPtr->efffreq;
		}

		for (int i = 0; i < node_link.size(); i++)
		{

			node_link[i]->volume = 0.0;
			node_link[i]->prevolume = 0.0;
			node_link[i]->m_linkPtr->volume += flow;//由glink加载到ptlink中
			if (node_link[i]->m_linkPtr->rLink)
			{
				node_link[i]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
			}

			node_link[i]->m_head->flow += flow;//更新头节点的进入流量

			node_link[i]->m_data = m_data * (node_link[i]->m_linkPtr->efffreq / ttfreq);//更新link的使用概率（因为只有一条boarding_link）
			node_link[i]->m_head->m_data += node_link[i]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

			if (node_link[i]->m_linkPtr->volume < -1e-8)
			{
				cout << "flow = 0.0: " << node_link[i]->m_linkPtr->id << " 's volume: " << node_link[0]->m_linkPtr->volume << " less than -1e-8" << endl;
				cout << endl;
			}
			//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
			//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导

			/*不考虑拥挤*/
			//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
			//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();

			//node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

			if (node_link[i]->m_linkPtr->rLink)
			{
				if (node_link[i]->m_linkPtr->rLink->volume < -1e-8)
				{
					cout << "flow = 0.0(rlink): " << node_link[i]->m_linkPtr->rLink->id << " 's volume: " << node_link[i]->m_linkPtr->rLink->volume << "less than -1e-8" << endl;
					cout << endl;
				}
				node_link[i]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
				//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
				//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();

				/*不考虑拥挤*/
				//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
				//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

				//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
			}
		}

		return 0;
	}

	if (node_link.size() == 1)
	{
		node_link[0]->prevolume = flow;//由节点的流量（ = hyperpath到达该节点的流量） 加载到对应的glink中
		node_link[0]->volume = flow; //加载hyperpath的流量(flow)

		node_link[0]->m_linkPtr->temp_temp = node_link[0]->m_linkPtr->volume;
		node_link[0]->m_linkPtr->volume += flow;//由glink加载到ptlink中

		if (node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
		}

		node_link[0]->m_head->flow += flow;//更新头节点的进入流量

		node_link[0]->m_data = m_data;//更新link的使用概率（因为只有一条boarding_link）
		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

		if (node_link[0]->m_linkPtr->volume < -1e-8)
		{

			cout << "ttflow: " << flow << endl;
			cout << "node_link[0]->m_linkPtr->before_volume: " << node_link[0]->m_linkPtr->temp_temp << endl;
			cout << "node_link.size() == 1: " << node_link[0]->m_linkPtr->id << " 's volume: " << node_link[0]->m_linkPtr->volume << " less than -1e-10" << endl;
			system("PAUSE");
		}
		//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导

		/*不考虑拥挤*/
		//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();

		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

		if (node_link[0]->m_linkPtr->rLink)
		{
			if (node_link[0]->m_linkPtr->rLink->volume < -1e-8)
			{
				cout << "node_link.size()(rlink) == 1: " << node_link[0]->m_linkPtr->rLink->id << " 's volume: " << node_link[0]->m_linkPtr->rLink->volume << " less than -1e-10" << endl;
				cout << endl;
			}
			node_link[0]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		return 0;
	}

	//init
	floatType fuyuanzhi = 0.0;
	floatType MAX_CAP = 0.0;
	bool if_already_exceed = false;
	bool must_exceed_state = false;
	bool all_exceed_state = false;
	//bool rlink_already_inflow = false;
	int already_exceed_num = 0;
	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];

		glink->m_linkPtr->UpdateEffectiveFreq();
		glink->rlink_already_inflow = false;

		if (glink->m_head->flow > 0)
		{
			//cout << glink->m_linkPtr->id << "'s rlink: " << glink->m_linkPtr->rLink->id << " already have inflow!" << endl;
			glink->m_linkPtr->rLink->volume += glink->m_head->flow;
			glink->rlink_already_inflow = true;
			fuyuanzhi = glink->m_head->flow;
		}

		glink->if_exceed = false;

		//cout << "id: "<< glink->m_linkPtr->rLink->id << " eff_freq: " << glink->m_linkPtr->efffreq << " glink->m_linkPtr->rLink->volume: " << glink->m_linkPtr->rLink->volume << " cap: " << glink->m_linkPtr->rLink->cap << endl;

		if (glink->m_linkPtr->rLink->volume < glink->m_linkPtr->rLink->cap)
		{
			glink->max_blink_flow = glink->m_linkPtr->rLink->cap - glink->m_linkPtr->rLink->volume;

			MAX_CAP += glink->max_blink_flow;
		}

		if (glink->m_linkPtr->rLink->volume >= glink->m_linkPtr->rLink->cap)
		{
			glink->volume = 0.0;
			already_exceed_num += 1;
			if_already_exceed = true;
		}

		if (glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -1e-10)//如果boarding_link上有数值，更新当前link和rlink(当前link初始化为0；rlink增加对应的数值)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}
	//cout <<"---------------------"<< flow << " " << MAX_CAP << endl;
	/////增加警告！！！！！！

	if (!if_already_exceed)
	{
		must_exceed_state = false;

		if (MAX_CAP < flow)
		{
			//cout << "Error! The station is required to allocate a flow exceeding its capacity." << endl;
			//cout << flow << " " << MAX_CAP << endl;
			must_exceed_state = true;
			//exceed_flow = (flow - MAX_CAP) * 1.05;
			//return 2;
			//system("PAUSE");
		}

		//floatType RATIO = 1.0 / 20.0;

		if (must_exceed_state)
		{
			//cout << "yesss!!!!!!!!" << endl;
			floatType ttflow = 0.0;
			for (int i = 0; i < node_link.size(); i++)
			{
				node_link[i]->volume = node_link[i]->max_blink_flow;
				node_link[i]->prevolume = node_link[i]->max_blink_flow;

				ttflow += node_link[i]->max_blink_flow;
			}

			for (int i = 0; i < node_link.size(); i++)
			{
				node_link[i]->volume += (flow - ttflow) / node_link.size();
				node_link[i]->prevolume += (flow - ttflow) / node_link.size();

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					//cout << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->prevolume << endl;
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
					//cout << node_link[i]->m_linkPtr->rLink->volume << endl;
					//cout << "--------------------------------" << endl;
				}
			}

		}
		else
		{
			floatType shift_flow = 0.0;
			int exceed_blink_num = 0;
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				if (node_link[i]->volume > node_link[i]->max_blink_flow)
				{

					shift_flow += (node_link[i]->volume - node_link[i]->max_blink_flow * 0.99);

					node_link[i]->volume = node_link[i]->max_blink_flow * 0.99;
					node_link[i]->prevolume = node_link[i]->max_blink_flow * 0.99;

					exceed_blink_num += 1;

					node_link[i]->if_exceed = true;
				}
			}

			//cout << shift_flow << " " << (node_link.size() - exceed_blink_num) << endl;
			//floatType shift_flow = 0.0;
			for (int i = 0; i < node_link.size(); i++)
			{
				if (!node_link[i]->if_exceed)
				{
					node_link[i]->volume += (shift_flow / (node_link.size() - exceed_blink_num));

					node_link[i]->prevolume += (shift_flow / (node_link.size() - exceed_blink_num));
				}

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					//cout << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->prevolume << endl;
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
					//cout << node_link[i]->m_linkPtr->rLink->volume << endl;
					//cout << "--------------------------------" << endl;
				}

				node_link[i]->if_exceed = false;
			}

		}
	}
	else
	{
		all_exceed_state = false;
		if (already_exceed_num == node_link.size())
		{
			//cout << "Error! All lines at this stop exceed their capacity!" << endl;
			all_exceed_state = true;
			//system("PAUSE");
		}

		if (all_exceed_state)
		{
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}

		}
		else
		{
			for (int i = 0; i < node_link.size(); i++)
			{
				if (node_link[i]->m_linkPtr->rLink->volume < node_link[i]->m_linkPtr->rLink->cap)
				{

					node_link[i]->volume = (flow - already_exceed_num * 0.0) / (node_link.size() - already_exceed_num);
				}

				node_link[i]->prevolume = node_link[i]->volume;

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}
		}
	}

	//cout << "初始: " << endl;
	//floatType ttflow = 0.0;
	for (int i = 0; i < node_link.size(); i++)
	{
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr（VI问题的cost）

		GLINK* glink = node_link[i];

		//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << "flow: " << glink->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << glink->cost << " rlink->id: " << glink->m_linkPtr->rLink->id << " rlink->volume: " << glink->m_linkPtr->rLink->volume << endl;
		//cout << glink->prevolume << endl;
		//cout << "-------------------------------" << endl;
		//cout << "node_link[i]->m_linkPtr->volume : " << node_link[i]->m_linkPtr->volume << endl;
		//ttflow += glink->volume;

	}
	//cout << flow << endl;
	//cout << "-----------------------------------------------" << endl;

	//main loop
	floatType gap = 0.0;
	int count = 0;
	floatType zhejian = 0.8;
	int maxLineSearchIter = 100000;
	floatType lineSearchAccuracy = 0.0000001;
	floatType DIFF;
	//vector<floatType> gap_vector;
	do
	{
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
		}
		//update cost（找出最小cost的b_link）
		floatType mincost = 1e25;
		int minindex;
		for (int i = 0; i < node_link.size(); i++)
		{
			if (node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

		//cout << "Iter: " << count << "----------------------------------" << endl;
		//GP（GP法转移流量）
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->prevolume = node_link[i]->volume;
			node_link[minindex]->prevolume = node_link[minindex]->volume;
			if (must_exceed_state)
			{
				if (i != minindex)
				{
					//以volume/eff作为boarding_link的cost
					node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
					node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
					int iter = 0;
					floatType dev = node_link[i]->cost - node_link[minindex]->cost;
					floatType ldflow = 0.0, rdflow = 0.0;
					if (dev > 0) //非参考路径 > 参考路径
					{
						rdflow = node_link[i]->volume;
					}
					else // //参考路径 < 非参考路径
					{
						ldflow = -node_link[minindex]->volume;
					}
					floatType dflow = 0.0, lastdflow = 0.0;

					DIFF = pow(dev, 2.0);
					//两条路径（参考路径 vs 非参考路径）间通过二分法进行流量转移（之前应当判断一下最大转移量是否满足收敛条件？）
					while (iter < maxLineSearchIter && abs(DIFF) > lineSearchAccuracy )
					{
						//cout << "iter: " << iter << endl;
						iter++;
						dflow = (ldflow + rdflow) / 2.0;//二分法确定转移量
						node_link[minindex]->volume = node_link[minindex]->volume + dflow - lastdflow; //这里的加减lastdflow是为了重置volume
						node_link[i]->volume = node_link[i]->volume - dflow + lastdflow;

						//cout << "dflow: " << dflow << endl;

						//更新头节点与plink的流量
						node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;
						node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;

						node_link[minindex]->m_linkPtr->volume += dflow - lastdflow;
						node_link[i]->m_linkPtr->volume += -dflow + lastdflow;

						//更新行驶弧的流量
						if (node_link[i]->m_linkPtr->rLink)
						{
							node_link[i]->m_linkPtr->rLink->volume += -dflow + lastdflow;
						}
						if (node_link[minindex]->m_linkPtr->rLink)
						{
							node_link[minindex]->m_linkPtr->rLink->volume += dflow - lastdflow;
						}

						//如果上车弧的流量为负，报错
						if ((abs(node_link[i]->volume) > 1e-10 && node_link[i]->volume < 0) || (abs(node_link[minindex]->volume) > 1e-10 && node_link[minindex]->volume < 0))
						{
							cout << node_link[i]->volume << "," << node_link[minindex]->volume << endl;
							system("PAUSE");
						}

						/*if(node_link[i]->volume < 0 && node_link[i]->volume > -0.0000000001)
						{
							node_link[i]->m_linkPtr->volume -= node_link[i]->volume;
							node_link[i]->m_linkPtr->rLink->volume -= node_link[i]->volume;
							node_link[i]->volume = 0;
							node_link[minindex]->volume += node_link[i]->volume;
						}

						if(node_link[minindex]->volume < 0 && node_link[minindex]->volume > -0.0000000001)
						{
							node_link[minindex]->m_linkPtr->volume -= node_link[minindex]->volume;
							node_link[minindex]->m_linkPtr->rLink->volume -= node_link[minindex]->volume;
							node_link[minindex]->volume = 0;
							node_link[i]->volume += node_link[minindex]->volume;
						}*/

						node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
						node_link[i]->m_linkPtr->UpdateEffectiveFreq();

						node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
						node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;

						dev = node_link[i]->cost - node_link[minindex]->cost;
						lastdflow = dflow;
						node_link[minindex]->prevolume = node_link[minindex]->volume;
						node_link[i]->prevolume = node_link[i]->volume;

						if (dev > 0)
						{
							ldflow = dflow;
						}
						else
						{
							rdflow = dflow;
						}

						DIFF = pow(dev, 2.0);
						//floatType tt = 0.0;
						//for (int i = 0; i < node_link.size(); i++)
						//{
						//	GLINK* glink = node_link[i];

						//	tt += node_link[i]->m_linkPtr->efffreq;
						//	cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " volume: " << glink->m_linkPtr->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
						//}
						//cout << iter<<" "<<"wait_cost: " << 1.0 / tt << endl;
						//cout << dev << endl;
						//cout << "-------------------------" << endl;

					}

					node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
					node_link[i]->m_linkPtr->UpdateEffectiveFreq();
					node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
					node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
				}

			}

			else
			{
				if (i != minindex)
				{
					//node_link[minindex]->m_linkPtr->temptemp = node_link[minindex]->m_linkPtr->volume;
					//node_link[i]->m_linkPtr->temptemp = node_link[i]->m_linkPtr->volume;

					//if (node_link[i]->m_linkPtr->temptemp < 0 || node_link[minindex]->m_linkPtr->temptemp < 0)
					//{

					//	cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;

					//	system("PAUSE");
					//}

					node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
					node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

					node_link[minindex]->m_data = node_link[minindex]->volume / flow;
					node_link[i]->m_data = node_link[i]->volume / flow;

					node_link[minindex]->prevolume = node_link[minindex]->volume;
					node_link[i]->prevolume = node_link[i]->volume;

					node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
					node_link[i]->UpdateDer();

					node_link[i]->max_blink_flow = node_link[i]->m_linkPtr->rLink->cap - node_link[i]->m_linkPtr->rLink->volume;//更新最大转移量
					node_link[minindex]->max_blink_flow = node_link[minindex]->m_linkPtr->rLink->cap - node_link[minindex]->m_linkPtr->rLink->volume;

					//if (node_link[i]->m_linkPtr->rLink->volume > node_link[i]->m_linkPtr->rLink->cap)
					//{
					//	node_link[i]->if_exceed = true;
					//}
					//else
					//{
					//	node_link[i]->if_exceed = false;
					//}

					//if (node_link[minindex]->m_linkPtr->rLink->volume > node_link[minindex]->m_linkPtr->rLink->cap)
					//{
					//	node_link[minindex]->if_exceed = true;
					//}
					//else
					//{
					//	node_link[minindex]->if_exceed = false;
					//}

					//cout << "node_link[i]->max_blink_flow: " << node_link[i]->max_blink_flow << " node_link[minindex]->max_blink_flow: " << node_link[minindex]->max_blink_flow << endl;

					floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq

					//cout << "node_link[i]->der: " << node_link[i]->der << " node_link[minindex]->der: " << node_link[minindex]->der << endl;

					if (Der == 0)	Der = 1e-8;
					double dev = node_link[i]->cost - node_link[minindex]->cost;
					double dflow;

					if (!if_already_exceed)
					{
						if (must_exceed_state)
						{

							if (dev >= 0)
							{
								dflow = __min(0.0001 * dev / Der, node_link[i]->volume);

								//<< "dev >= 0" << endl;
								//if (node_link[minindex]->if_exceed)
								//{
								//	dflow = __min(1.0 * dev / Der, node_link[i]->volume);
								//}
								//else
								//{
								//	dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
								//}
							}
							else
							{
								dflow = __max(0.0001 * dev / Der, -node_link[minindex]->volume);

								//if (node_link[i]->if_exceed)
								//{
								//	dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
								//}
								//else
								//{
								//	dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
								//}
							}
						}

						else
						{
							if (dev >= 0)
							{
								//dflow = __min(1.0 * dev / Der, node_link[i]->volume);

								//zhejian = 0.1;

								//if ( (dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
								//{
								//	zhejian = 0.01;
								//}
								if (zhejian > 0.01)
								{
									if ((dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
									{
										zhejian = 0.01;
									}
									//zhejian = 0.01;

									else
									{
										zhejian = 0.8;
									}

								}

								//dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);

								dflow = __min(zhejian * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
								//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
							}
							else
							{
								//dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);

								//zhejian = 0.1;

								//if ((-1.0 * dev / Der) > node_link[i]->max_blink_flow * 0.99)//调整下比例？改成0.5？？
								//{
								//	zhejian = 0.01;
								//}

								if (zhejian > 0.01)
								{
									if ((-dev / Der) > node_link[i]->max_blink_flow * 0.99)
									{
										zhejian = 0.01;
									}
									else
									{
										zhejian = 0.8;
									}

								}

								//dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
								dflow = __max(zhejian * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);

								//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
							}

						}
					}
					else
					{
						if (dev >= 0)
						{
							dflow = __min(0.0001 * dev / Der, node_link[i]->volume);
						}
						else
						{
							dflow = __max(0.0001 * dev / Der, -node_link[minindex]->volume);
						}
					}

					//cout << "dev: " << dev << " Der: " << Der << " dev / Der: " << dev / Der << " dflow: " << dflow << endl;

					node_link[i]->volume = node_link[i]->prevolume - dflow;
					node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

					if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
					{
						//cout<<"yes!!!!!!!"<<endl;
						//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;

						//cout<<"-------------------"<<endl;
						//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;

						//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
						//cout<<"CESHI: "<<CESHI1<<endl;

						////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)

						//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
						//cout<<"-------------------"<<endl;

						//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;

						//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);

						//cout<<"ceshi: "<<ceshi<<endl;

						if (flow = 0.0)
						{

							cout << "wei 0!" << endl;
						}

						cout << "ttflow: " << flow << endl;

						cout << "minindex: " << node_link[minindex]->m_linkPtr->id << " minindex->prevolume: " << node_link[minindex]->prevolume << endl;

						cout << "minindex->eff: " << node_link[minindex]->m_linkPtr->efffreq << " minindex->type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << endl;

						cout << "i: " << node_link[i]->m_linkPtr->id << " i->prevolume: " << node_link[i]->prevolume << endl;

						cout << "i->eff: " << node_link[i]->m_linkPtr->efffreq << " i->type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << " " << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << endl;

						cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;
						node_link[i]->m_linkPtr->UpdateEffectiveFreq_TEST();

						cout << "再来一遍i->eff: " << node_link[i]->m_linkPtr->efffreq << endl;

						cout << "dev: " << dev << " Der: " << Der << " 1.0 * dev / Der: " << 1.0 * dev / Der << " shift_flow: " << dflow << ",i->aftervolume: " << node_link[i]->volume << ",minindex->aftervolume: " << node_link[minindex]->volume << endl;
						system("PAUSE");
					}

					//load（加载流量）
					node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
					if (node_link[i]->m_linkPtr->rLink)
					{
						node_link[i]->m_linkPtr->rLink->volume += (node_link[i]->volume - node_link[i]->prevolume);
					}

					//cout << node_link[i]->m_linkPtr->rLink->id<<" "<<node_link[i]->prevolume << " " << node_link[i]->volume << " " << node_link[i]->m_linkPtr->rLink->volume << endl;

					node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
					if (node_link[minindex]->m_linkPtr->rLink)
					{
						node_link[minindex]->m_linkPtr->rLink->volume += (node_link[minindex]->volume - node_link[minindex]->prevolume);
					}
					//cout << node_link[minindex]->m_linkPtr->rLink->id<<" "<<node_link[minindex]->prevolume << " " << node_link[minindex]->volume << " " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

					node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
					node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

					node_link[i]->m_linkPtr->UpdateEffectiveFreq();
					node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

					//cout << "link->id: " << node_link[i]->m_linkPtr->id << " type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[i]->volume << " efffreq: " << node_link[i]->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << " rlink->id: " << node_link[i]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[i]->m_linkPtr->rLink->volume << endl;

					//cout << "link->id: " << node_link[minindex]->m_linkPtr->id << " type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[minindex]->volume << " efffreq: " << node_link[minindex]->m_linkPtr->efffreq << " cost: " << node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq << " rlink->id: " << node_link[minindex]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

					//cout << "-------------------------------------------------------" << endl;
					//cout << node_link[i]->m_linkPtr->id<<" "<<node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << " "<< node_link[i]->m_linkPtr->efffreq<<endl;

					//cout << node_link[minindex]->m_linkPtr->id <<" "<<node_link[minindex]->m_linkPtr->rLink->volume << " " << node_link[minindex]->m_linkPtr->cap << " "<<node_link[minindex]->m_linkPtr->efffreq << endl;

					/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
					tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
					//GLINK* glink = node_link[i];
					//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
				}
				//GLINK* glink = node_link[i];

				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;

			}

			

		}

		//compute gap
		count++;
		GLINK* glink = node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		glink->gap = aa;
		gap = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			GLINK* glink = node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			glink->gap = temp;

			gap += pow(aa - temp, 2);

			//GLINK* glink = node_link[i];
			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
		}
		//cout << "GAP: " << gap << endl;

	} while (gap > 0.0000001 && count < 150000);//count = 1000; gap > 0.00001 (best parameter: 100)  常规是50 SF（1.8）：100    ****(SF(1.2/1.4): gap > 0.0000001 && count < 100)****

	//if (count == 1000 && flow < 0.00001)
	//{
	//	cout << "gap: " << gap << endl;
	//	cout << "count: " << count << endl;
	//	cout << "flow: "<<flow << endl;

	//	int iter = 0;

	//	do
	//	{
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
	//		}
	//		//update cost（找出最小cost的b_link）
	//		floatType mincost = 1e25;
	//		int minindex;
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			if (node_link[i]->cost < mincost)
	//			{
	//				mincost = node_link[i]->cost;
	//				minindex = i;
	//			}
	//		}

	//		cout << "Iter: " << iter << "----------------------------------" << endl;
	//		//GP（GP法转移流量）
	//		for (int i = 0; i < node_link.size(); i++)
	//		{

	//			if (i != minindex)
	//			{
	//				//node_link[minindex]->m_linkPtr->temptemp = node_link[minindex]->m_linkPtr->volume;
	//				//node_link[i]->m_linkPtr->temptemp = node_link[i]->m_linkPtr->volume;

	//				//if (node_link[i]->m_linkPtr->temptemp < 0 || node_link[minindex]->m_linkPtr->temptemp < 0)
	//				//{

	//				//	cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;

	//				//	system("PAUSE");
	//				//}

	//				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
	//				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

	//				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
	//				node_link[i]->m_data = node_link[i]->volume / flow;

	//				node_link[minindex]->prevolume = node_link[minindex]->volume;
	//				node_link[i]->prevolume = node_link[i]->volume;

	//				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
	//				node_link[i]->UpdateDer();

	//				node_link[i]->max_blink_flow = node_link[i]->m_linkPtr->rLink->cap - node_link[i]->m_linkPtr->rLink->volume;//更新最大转移量
	//				node_link[minindex]->max_blink_flow = node_link[minindex]->m_linkPtr->rLink->cap - node_link[minindex]->m_linkPtr->rLink->volume;

	//				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq

	//				//cout << "node_link[i]->der: " << node_link[i]->der << " node_link[minindex]->der: " << node_link[minindex]->der << endl;

	//				if (Der == 0)	Der = 1e-8;
	//				double dev = node_link[i]->cost - node_link[minindex]->cost;
	//				double dflow;
	//				floatType zhejian = 1.0;
	//				if (!if_already_exceed)
	//				{
	//					if (dev >= 0)
	//					{
	//						//dflow = __min(1.0 * dev / Der, node_link[i]->volume);

	//						zhejian = 0.1;

	//						if ( (dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
	//						{
	//							zhejian = 0.01;
	//						}

	//						//dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);

	//						dflow = __min(zhejian * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
	//						//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
	//					}
	//					else
	//					{
	//						//dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);

	//						zhejian = 0.1;

	//						if ((-1.0 * dev / Der) > node_link[i]->max_blink_flow * 0.99)//调整下比例？改成0.5？？
	//						{
	//							zhejian = 0.01;
	//						}

	//						//dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
	//						dflow = __max(zhejian * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);

	//						//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
	//					}

	//				}
	//				else
	//				{
	//					if (dev >= 0)
	//					{
	//						dflow = __min(1.0 * dev / Der, node_link[i]->volume);
	//					}
	//					else
	//					{
	//						dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
	//					}

	//				}

	//				cout << "dev: " << dev << " Der: " << Der << " dev / Der: " << dev / Der << " dflow: " << dflow << endl;

	//				node_link[i]->volume = node_link[i]->prevolume - dflow;
	//				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

	//				if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
	//				{
	//					//cout<<"yes!!!!!!!"<<endl;
	//					//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;

	//					//cout<<"-------------------"<<endl;
	//					//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;

	//					//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
	//					//cout<<"CESHI: "<<CESHI1<<endl;

	//					////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)

	//					//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
	//					//cout<<"-------------------"<<endl;

	//					//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;

	//					//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);

	//					//cout<<"ceshi: "<<ceshi<<endl;

	//					if (flow = 0.0)
	//					{

	//						cout << "wei 0!" << endl;
	//					}

	//					cout << "ttflow: " << flow << endl;

	//					cout << "minindex: " << node_link[minindex]->m_linkPtr->id << " minindex->prevolume: " << node_link[minindex]->prevolume << endl;

	//					cout << "minindex->eff: " << node_link[minindex]->m_linkPtr->efffreq << " minindex->type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << endl;

	//					cout << "i: " << node_link[i]->m_linkPtr->id << " i->prevolume: " << node_link[i]->prevolume << endl;

	//					cout << "i->eff: " << node_link[i]->m_linkPtr->efffreq << " i->type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << " " << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << endl;

	//					cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;
	//					node_link[i]->m_linkPtr->UpdateEffectiveFreq_TEST();

	//					cout << "再来一遍i->eff: " << node_link[i]->m_linkPtr->efffreq << endl;

	//					cout << "dev: " << dev << " Der: " << Der << " 1.0 * dev / Der: " << 1.0 * dev / Der << " shift_flow: " << dflow << ",i->aftervolume: " << node_link[i]->volume << ",minindex->aftervolume: " << node_link[minindex]->volume << endl;
	//					system("PAUSE");
	//				}

	//				//load（加载流量）
	//				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
	//				if (node_link[i]->m_linkPtr->rLink)
	//				{
	//					node_link[i]->m_linkPtr->rLink->volume += (node_link[i]->volume - node_link[i]->prevolume);
	//				}

	//				//cout << node_link[i]->m_linkPtr->rLink->id<<" "<<node_link[i]->prevolume << " " << node_link[i]->volume << " " << node_link[i]->m_linkPtr->rLink->volume << endl;

	//				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
	//				if (node_link[minindex]->m_linkPtr->rLink)
	//				{
	//					node_link[minindex]->m_linkPtr->rLink->volume += (node_link[minindex]->volume - node_link[minindex]->prevolume);
	//				}
	//				//cout << node_link[minindex]->m_linkPtr->rLink->id<<" "<<node_link[minindex]->prevolume << " " << node_link[minindex]->volume << " " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

	//				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
	//				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

	//				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
	//				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

	//				cout << "link->id: " << node_link[i]->m_linkPtr->id << " type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[i]->volume << " efffreq: " << node_link[i]->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << " rlink->id: " << node_link[i]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[i]->m_linkPtr->rLink->volume << endl;

	//				cout << "link->id: " << node_link[minindex]->m_linkPtr->id << " type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[minindex]->volume << " efffreq: " << node_link[minindex]->m_linkPtr->efffreq << " cost: " << node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq << " rlink->id: " << node_link[minindex]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

	//				//cout << "-------------------------------------------------------" << endl;
	//				//cout << node_link[i]->m_linkPtr->id<<" "<<node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << " "<< node_link[i]->m_linkPtr->efffreq<<endl;

	//				//cout << node_link[minindex]->m_linkPtr->id <<" "<<node_link[minindex]->m_linkPtr->rLink->volume << " " << node_link[minindex]->m_linkPtr->cap << " "<<node_link[minindex]->m_linkPtr->efffreq << endl;

	//				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
	//				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
	//				//GLINK* glink = node_link[i];
	//				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
	//			}
	//			//GLINK* glink = node_link[i];

	//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;

	//		}

	//		//compute gap
	//		iter++;
	//		GLINK* glink = node_link[0];
	//		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
	//		glink->gap = aa;
	//		gap = 0.0;
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			GLINK* glink = node_link[i];
	//			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
	//			glink->gap = temp;

	//			gap += pow(aa - temp, 2);

	//			//GLINK* glink = node_link[i];
	//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
	//		}

	//		cout << "GAP: " << gap << endl;

	//	} while (gap > 0.0000001 && iter < 1000);
	//	system("pause");
	//}

	//update m_data（更新概率）
	floatType ttfreq = 0.0;
	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
		//cout << glink->m_linkPtr->id << " " << glink->m_linkPtr->efffreq << endl;
	}

	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];

		glink->effective_freq = glink->m_linkPtr->efffreq;

		glink->temp_value = glink->m_linkPtr->efffreq / ttfreq;
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];
		//glink->m_head->flow = glink->volume;//?????
		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
		//glink->m_head->flow += glink->volume - glink->prevolume;

		//glink->effective_freq = glink->m_linkPtr->efffreq;

		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		if (glink->m_linkPtr->volume < -1e-8)
		{
			cout << "正常计算： " << glink->m_linkPtr->id << " 's volume: " << glink->m_linkPtr->volume << " less than -1e-8" << endl;
			//cout << endl;
		}
		//glink->m_linkPtr->UpdatePTLinkCost();
		//glink->m_linkPtr->UpdatePTDerLinkCost();

		/*不考虑拥挤*/
		//glink->m_linkPtr->UpdatePTLinkCost_const();
		//glink->m_linkPtr->UpdatePTDerLinkCost_const();

		glink->m_linkPtr->UpdateEffectiveFreq();

		if (glink->m_linkPtr->rLink)
		{
			//cout<<glink->m_linkPtr->rLink->volume<<endl;
			glink->m_linkPtr->rLink->temptemp = glink->m_linkPtr->rLink->volume;
			glink->m_linkPtr->rLink->volume -= glink->volume;//为什么？？？-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数

			if (glink->rlink_already_inflow)
			{
				glink->m_linkPtr->rLink->volume -= fuyuanzhi;
			}

			if (glink->m_linkPtr->rLink->volume < -1e-8)
			{
				cout << glink->m_linkPtr->rLink->id << " 's volume: " << glink->m_linkPtr->rLink->volume << " is less than -1e-8" << endl;
				cout << "pre_volume is: " << glink->m_linkPtr->rLink->temptemp << " glink->volume is: " << glink->volume << endl;

				cout << "--------------------------------------------" << endl;

				system("PAUSE");
				//cout << endl;
			}

			//glink->m_linkPtr->rLink->UpdatePTLinkCost();
			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			//glink->m_linkPtr->rLink->UpdatePTLinkCost_const();
			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

			//glink->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
	}

	return 0;
}

int	GNODE::SolveFPwithGP_test()
{
	//if (flow == 0 || flow < 1e-15)
	if (flow == 0 || ((flow < 1e-15) && (flow > 0)))
	{
		floatType ttfreq = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->m_linkPtr->UpdateEffectiveFreq();
			ttfreq += node_link[i]->m_linkPtr->efffreq;
		}

		for (int i = 0; i < node_link.size(); i++)
		{

			node_link[i]->volume = 0.0;
			node_link[i]->prevolume = 0.0;
			node_link[i]->m_linkPtr->volume += flow;//由glink加载到ptlink中
			if (node_link[i]->m_linkPtr->rLink)
			{
				node_link[i]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
			}

			node_link[i]->m_head->flow += flow;//更新头节点的进入流量

			node_link[i]->m_data = m_data * (node_link[i]->m_linkPtr->efffreq / ttfreq);//更新link的使用概率（因为只有一条boarding_link）
			node_link[i]->m_head->m_data += node_link[i]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

			if (node_link[i]->m_linkPtr->volume < -1e-5)
			{
				cout << "flow = 0.0: " << node_link[i]->m_linkPtr->id << " 's volume: " << node_link[0]->m_linkPtr->volume << " less than -1e-8" << endl;
				cout << endl;
			}
			//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
			//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导

			/*不考虑拥挤*/
			//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
			//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();

			//node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

			if (node_link[i]->m_linkPtr->rLink)
			{
				if (node_link[i]->m_linkPtr->rLink->volume < -1e-8)
				{
					cout << "flow = 0.0(rlink): " << node_link[i]->m_linkPtr->rLink->id << " 's volume: " << node_link[i]->m_linkPtr->rLink->volume << "less than -1e-8" << endl;
					cout << endl;
				}
				node_link[i]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
				//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
				//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();

				/*不考虑拥挤*/
				//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
				//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

				//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
			}
		}

		return 0;
	}

	if (node_link.size() == 1)
	{
		node_link[0]->prevolume = flow;//由节点的流量（ = hyperpath到达该节点的流量） 加载到对应的glink中
		node_link[0]->volume = flow; //加载hyperpath的流量(flow)

		node_link[0]->m_linkPtr->temp_temp = node_link[0]->m_linkPtr->volume;
		node_link[0]->m_linkPtr->volume += flow;//由glink加载到ptlink中

		if (node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
		}

		node_link[0]->m_head->flow += flow;//更新头节点的进入流量

		node_link[0]->m_data = m_data;//更新link的使用概率（因为只有一条boarding_link）
		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

		if (node_link[0]->m_linkPtr->volume < -1e-5)
		{

			cout << "ttflow: " << flow << endl;
			cout << "node_link[0]->m_linkPtr->before_volume: " << node_link[0]->m_linkPtr->temp_temp << endl;
			cout << "node_link.size() == 1: " << node_link[0]->m_linkPtr->id << " 's volume: " << node_link[0]->m_linkPtr->volume << " less than -1e-10" << endl;
			system("PAUSE");
		}
		//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导

		/*不考虑拥挤*/
		//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();

		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

		if (node_link[0]->m_linkPtr->rLink)
		{
			if (node_link[0]->m_linkPtr->rLink->volume < -1e-5)
			{
				cout << "node_link.size()(rlink) == 1: " << node_link[0]->m_linkPtr->rLink->id << " 's volume: " << node_link[0]->m_linkPtr->rLink->volume << " less than -1e-10" << endl;
				cout << endl;
			}
			node_link[0]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		return 0;
	}

	//init
	floatType fuyuanzhi = 0.0;
	floatType MAX_CAP = 0.0;
	bool if_already_exceed = false;
	bool must_exceed_state = false;
	bool all_exceed_state = false;
	int already_exceed_num = 0;
	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];

		glink->if_exceed = false;

		glink->rlink_already_inflow = false;
		if (glink->m_head->flow > 0)
		{
			//cout << glink->m_linkPtr->id << "'s rlink: " << glink->m_linkPtr->rLink->id << " already have inflow!" << endl;
			glink->m_linkPtr->rLink->volume += glink->m_head->flow;
			glink->rlink_already_inflow = true;
			fuyuanzhi = glink->m_head->flow;
		}

		//cout << "id: " << "eff_freq: " << glink->m_linkPtr->efffreq << " " << glink->m_linkPtr->rLink->id << " glink->m_linkPtr->rLink->volume: " << glink->m_linkPtr->rLink->volume << " " << glink->m_linkPtr->rLink->cap << endl;

		if (glink->m_linkPtr->rLink->volume < glink->m_linkPtr->rLink->cap)
		{
			glink->max_blink_flow = glink->m_linkPtr->rLink->cap - glink->m_linkPtr->rLink->volume;

			MAX_CAP += glink->max_blink_flow;
		}

		if (glink->m_linkPtr->rLink->volume >= glink->m_linkPtr->rLink->cap)
		{
			glink->volume = 0.0;
			already_exceed_num += 1;
			if_already_exceed = true;
		}

		if (glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -1e-10)//如果boarding_link上有数值，更新当前link和rlink(当前link初始化为0；rlink增加对应的数值)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}
	//cout <<"---------------------"<< flow << " " << MAX_CAP << endl;
	/////增加警告！！！！！！

	if (!if_already_exceed)
	{
		must_exceed_state = false;

		if (MAX_CAP < flow)
		{
			//cout << "Error! The station is required to allocate a flow exceeding its capacity." << endl;
			//cout << flow << " " << MAX_CAP << endl;
			must_exceed_state = true;
			//exceed_flow = (flow - MAX_CAP) * 1.05;
			//return 2;
			//system("PAUSE");
		}

		//floatType RATIO = 1.0 / 20.0;

		if (must_exceed_state)
		{
			//cout << "yesss!!!!!!!!" << endl;
			floatType ttflow = 0.0;
			for (int i = 0; i < node_link.size(); i++)
			{
				node_link[i]->volume = node_link[i]->max_blink_flow;
				node_link[i]->prevolume = node_link[i]->max_blink_flow;

				ttflow += node_link[i]->max_blink_flow;
			}

			for (int i = 0; i < node_link.size(); i++)
			{
				node_link[i]->volume += (flow - ttflow) / node_link.size();
				node_link[i]->prevolume += (flow - ttflow) / node_link.size();

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					//cout << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->prevolume << endl;
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
					//cout << node_link[i]->m_linkPtr->rLink->volume << endl;
					//cout << "--------------------------------" << endl;
				}
			}

		}
		else
		{
			floatType shift_flow = 0.0;
			int exceed_blink_num = 0;
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				if (node_link[i]->volume > node_link[i]->max_blink_flow)
				{

					shift_flow += (node_link[i]->volume - node_link[i]->max_blink_flow * 0.99);

					node_link[i]->volume = node_link[i]->max_blink_flow * 0.99;
					node_link[i]->prevolume = node_link[i]->max_blink_flow * 0.99;

					exceed_blink_num += 1;

					node_link[i]->if_exceed = true;
				}
			}

			//cout << shift_flow << " " << (node_link.size() - exceed_blink_num) << endl;
			//floatType shift_flow = 0.0;
			for (int i = 0; i < node_link.size(); i++)
			{
				if (!node_link[i]->if_exceed)
				{
					node_link[i]->volume += (shift_flow / (node_link.size() - exceed_blink_num));

					node_link[i]->prevolume += (shift_flow / (node_link.size() - exceed_blink_num));
				}

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					//cout << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->prevolume << endl;
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
					//cout << node_link[i]->m_linkPtr->rLink->volume << endl;
					//cout << "--------------------------------" << endl;
				}

				node_link[i]->if_exceed = false;
			}

		}
	}
	else
	{
		all_exceed_state = false;
		if (already_exceed_num == node_link.size())
		{
			//cout << "Error! All lines at this stop exceed their capacity!" << endl;
			all_exceed_state = true;
			//system("PAUSE");
		}

		if (all_exceed_state)
		{
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}

		}
		else
		{
			for (int i = 0; i < node_link.size(); i++)
			{
				if (node_link[i]->m_linkPtr->rLink->volume < node_link[i]->m_linkPtr->rLink->cap)
				{

					node_link[i]->volume = (flow - already_exceed_num * 0.0) / (node_link.size() - already_exceed_num);
				}

				node_link[i]->prevolume = node_link[i]->volume;

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}
		}
	}

	//cout << "初始: " << endl;
	//floatType ttflow = 0.0;
	for (int i = 0; i < node_link.size(); i++)
	{
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr（VI问题的cost）

		//GLINK* glink = node_link[i];

		//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << "flow: " << glink->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << glink->cost << " rlink->id: " << glink->m_linkPtr->rLink->id << " rlink->volume: " << glink->m_linkPtr->rLink->volume << endl;
		//cout << glink->prevolume << endl;
		//cout << "-------------------------------" << endl;
		//cout << "node_link[i]->m_linkPtr->volume : " << node_link[i]->m_linkPtr->volume << endl;
		//ttflow += glink->volume;

	}
	//cout << flow << endl;
	//cout << "-----------------------------------------------" << endl;

	//main loop
	floatType gap = 0.0;
	int count = 0;
	floatType zhejian = 0.8;
	//vector<floatType> gap_vector;
	do
	{
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
		}
		//update cost（找出最小cost的b_link）
		floatType mincost = 1e25;
		int minindex;
		for (int i = 0; i < node_link.size(); i++)
		{
			if (node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

		//cout << "Iter: " << count << "----------------------------------" << endl;
		//GP（GP法转移流量）
		for (int i = 0; i < node_link.size(); i++)
		{

			if (i != minindex)
			{
				//node_link[minindex]->m_linkPtr->temptemp = node_link[minindex]->m_linkPtr->volume;
				//node_link[i]->m_linkPtr->temptemp = node_link[i]->m_linkPtr->volume;

				//if (node_link[i]->m_linkPtr->temptemp < 0 || node_link[minindex]->m_linkPtr->temptemp < 0)
				//{

				//	cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;

				//	system("PAUSE");
				//}

				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
				node_link[i]->m_data = node_link[i]->volume / flow;

				node_link[minindex]->prevolume = node_link[minindex]->volume;
				node_link[i]->prevolume = node_link[i]->volume;

				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
				node_link[i]->UpdateDer();

				node_link[i]->max_blink_flow = node_link[i]->m_linkPtr->rLink->cap - node_link[i]->m_linkPtr->rLink->volume;//更新最大转移量
				node_link[minindex]->max_blink_flow = node_link[minindex]->m_linkPtr->rLink->cap - node_link[minindex]->m_linkPtr->rLink->volume;

				//if (node_link[i]->m_linkPtr->rLink->volume > node_link[i]->m_linkPtr->rLink->cap)
				//{
				//	node_link[i]->if_exceed = true;
				//}
				//else
				//{
				//	node_link[i]->if_exceed = false;
				//}

				//if (node_link[minindex]->m_linkPtr->rLink->volume > node_link[minindex]->m_linkPtr->rLink->cap)
				//{
				//	node_link[minindex]->if_exceed = true;
				//}
				//else
				//{
				//	node_link[minindex]->if_exceed = false;
				//}

				//cout << "node_link[i]->max_blink_flow: " << node_link[i]->max_blink_flow << " node_link[minindex]->max_blink_flow: " << node_link[minindex]->max_blink_flow << endl;

				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq

				//cout << "node_link[i]->der: " << node_link[i]->der << " node_link[minindex]->der: " << node_link[minindex]->der << endl;

				if (Der == 0)	Der = 1e-8;
				double dev = node_link[i]->cost - node_link[minindex]->cost;
				double dflow;

				if (!if_already_exceed)
				{
					if (must_exceed_state)
					{

						if (dev >= 0)
						{
							dflow = __min(0.0001 * dev / Der, node_link[i]->volume);

							//<< "dev >= 0" << endl;
							//if (node_link[minindex]->if_exceed)
							//{
							//	dflow = __min(1.0 * dev / Der, node_link[i]->volume);
							//}
							//else
							//{
							//	dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
							//}
						}
						else
						{
							dflow = __max(0.0001 * dev / Der, -node_link[minindex]->volume);

							//if (node_link[i]->if_exceed)
							//{
							//	dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
							//}
							//else
							//{
							//	dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
							//}
						}
					}

					else
					{
						if (dev >= 0)
						{
							//dflow = __min(1.0 * dev / Der, node_link[i]->volume);

							//zhejian = 0.1;

							//if ( (dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
							//{
							//	zhejian = 0.01;
							//}
							if (zhejian > 0.01)
							{
								if ((dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
								{
									zhejian = 0.01;
								}
								//zhejian = 0.01;

								else
								{
									zhejian = 0.8;
								}

							}

							//dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);

							dflow = __min(zhejian * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
							//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
						}
						else
						{
							//dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);

							//zhejian = 0.1;

							//if ((-1.0 * dev / Der) > node_link[i]->max_blink_flow * 0.99)//调整下比例？改成0.5？？
							//{
							//	zhejian = 0.01;
							//}

							if (zhejian > 0.01)
							{
								if ((-dev / Der) > node_link[i]->max_blink_flow * 0.99)
								{
									zhejian = 0.01;
								}
								else
								{
									zhejian = 0.8;
								}

							}

							//dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
							dflow = __max(zhejian * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);

							//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
						}

					}
				}
				else
				{
					if (dev >= 0)
					{
						dflow = __min(0.0001 * dev / Der, node_link[i]->volume);
					}
					else
					{
						dflow = __max(0.0001 * dev / Der, -node_link[minindex]->volume);
					}
				}

				//cout << "dev: " << dev << " Der: " << Der << " dev / Der: " << dev / Der << " dflow: " << dflow << endl;

				node_link[i]->volume = node_link[i]->prevolume - dflow;
				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

				if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
				{
					//cout<<"yes!!!!!!!"<<endl;
					//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;

					//cout<<"-------------------"<<endl;
					//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;

					//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
					//cout<<"CESHI: "<<CESHI1<<endl;

					////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)

					//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
					//cout<<"-------------------"<<endl;

					//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;

					//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);

					//cout<<"ceshi: "<<ceshi<<endl;

					if (flow = 0.0)
					{

						cout << "wei 0!" << endl;
					}

					cout << "ttflow: " << flow << endl;

					cout << "minindex: " << node_link[minindex]->m_linkPtr->id << " minindex->prevolume: " << node_link[minindex]->prevolume << endl;

					cout << "minindex->eff: " << node_link[minindex]->m_linkPtr->efffreq << " minindex->type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << endl;

					cout << "i: " << node_link[i]->m_linkPtr->id << " i->prevolume: " << node_link[i]->prevolume << endl;

					cout << "i->eff: " << node_link[i]->m_linkPtr->efffreq << " i->type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << " " << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << endl;

					cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;
					node_link[i]->m_linkPtr->UpdateEffectiveFreq_TEST();

					cout << "再来一遍i->eff: " << node_link[i]->m_linkPtr->efffreq << endl;

					cout << "dev: " << dev << " Der: " << Der << " 1.0 * dev / Der: " << 1.0 * dev / Der << " shift_flow: " << dflow << ",i->aftervolume: " << node_link[i]->volume << ",minindex->aftervolume: " << node_link[minindex]->volume << endl;
					system("PAUSE");
				}

				//load（加载流量）
				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += (node_link[i]->volume - node_link[i]->prevolume);
				}

				//cout << node_link[i]->m_linkPtr->rLink->id<<" "<<node_link[i]->prevolume << " " << node_link[i]->volume << " " << node_link[i]->m_linkPtr->rLink->volume << endl;

				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
				if (node_link[minindex]->m_linkPtr->rLink)
				{
					node_link[minindex]->m_linkPtr->rLink->volume += (node_link[minindex]->volume - node_link[minindex]->prevolume);
				}
				//cout << node_link[minindex]->m_linkPtr->rLink->id<<" "<<node_link[minindex]->prevolume << " " << node_link[minindex]->volume << " " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

				//cout << "link->id: " << node_link[i]->m_linkPtr->id << " type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[i]->volume << " efffreq: " << node_link[i]->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << " rlink->id: " << node_link[i]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[i]->m_linkPtr->rLink->volume << endl;

				//cout << "link->id: " << node_link[minindex]->m_linkPtr->id << " type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[minindex]->volume << " efffreq: " << node_link[minindex]->m_linkPtr->efffreq << " cost: " << node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq << " rlink->id: " << node_link[minindex]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

				//cout << "-------------------------------------------------------" << endl;
				//cout << node_link[i]->m_linkPtr->id<<" "<<node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << " "<< node_link[i]->m_linkPtr->efffreq<<endl;

				//cout << node_link[minindex]->m_linkPtr->id <<" "<<node_link[minindex]->m_linkPtr->rLink->volume << " " << node_link[minindex]->m_linkPtr->cap << " "<<node_link[minindex]->m_linkPtr->efffreq << endl;

				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
				//GLINK* glink = node_link[i];
				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
			}
			//GLINK* glink = node_link[i];

			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;

		}

		//compute gap
		count++;
		GLINK* glink = node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		glink->gap = aa;
		gap = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			GLINK* glink = node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			glink->gap = temp;

			gap += pow(aa - temp, 2);

			//GLINK* glink = node_link[i];
			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
		}
		//cout << "GAP: " << gap << endl;

	} while (gap > 0.0000001 && count < 100000);//count = 1000; gap > 0.00001 (best parameter: 100)  常规是50 SF（1.8）：100    ****(SF(1.2/1.4): gap > 0.0000001 && count < 100)****

	//if (count == 1000 && flow < 0.00001)
	//{
	//	cout << "gap: " << gap << endl;
	//	cout << "count: " << count << endl;
	//	cout << "flow: "<<flow << endl;

	//	int iter = 0;

	//	do
	//	{
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
	//		}
	//		//update cost（找出最小cost的b_link）
	//		floatType mincost = 1e25;
	//		int minindex;
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			if (node_link[i]->cost < mincost)
	//			{
	//				mincost = node_link[i]->cost;
	//				minindex = i;
	//			}
	//		}

	//		cout << "Iter: " << iter << "----------------------------------" << endl;
	//		//GP（GP法转移流量）
	//		for (int i = 0; i < node_link.size(); i++)
	//		{

	//			if (i != minindex)
	//			{
	//				//node_link[minindex]->m_linkPtr->temptemp = node_link[minindex]->m_linkPtr->volume;
	//				//node_link[i]->m_linkPtr->temptemp = node_link[i]->m_linkPtr->volume;

	//				//if (node_link[i]->m_linkPtr->temptemp < 0 || node_link[minindex]->m_linkPtr->temptemp < 0)
	//				//{

	//				//	cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;

	//				//	system("PAUSE");
	//				//}

	//				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
	//				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

	//				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
	//				node_link[i]->m_data = node_link[i]->volume / flow;

	//				node_link[minindex]->prevolume = node_link[minindex]->volume;
	//				node_link[i]->prevolume = node_link[i]->volume;

	//				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
	//				node_link[i]->UpdateDer();

	//				node_link[i]->max_blink_flow = node_link[i]->m_linkPtr->rLink->cap - node_link[i]->m_linkPtr->rLink->volume;//更新最大转移量
	//				node_link[minindex]->max_blink_flow = node_link[minindex]->m_linkPtr->rLink->cap - node_link[minindex]->m_linkPtr->rLink->volume;

	//				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq

	//				//cout << "node_link[i]->der: " << node_link[i]->der << " node_link[minindex]->der: " << node_link[minindex]->der << endl;

	//				if (Der == 0)	Der = 1e-8;
	//				double dev = node_link[i]->cost - node_link[minindex]->cost;
	//				double dflow;
	//				floatType zhejian = 1.0;
	//				if (!if_already_exceed)
	//				{
	//					if (dev >= 0)
	//					{
	//						//dflow = __min(1.0 * dev / Der, node_link[i]->volume);

	//						zhejian = 0.1;

	//						if ( (dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
	//						{
	//							zhejian = 0.01;
	//						}

	//						//dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);

	//						dflow = __min(zhejian * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
	//						//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
	//					}
	//					else
	//					{
	//						//dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);

	//						zhejian = 0.1;

	//						if ((-1.0 * dev / Der) > node_link[i]->max_blink_flow * 0.99)//调整下比例？改成0.5？？
	//						{
	//							zhejian = 0.01;
	//						}

	//						//dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
	//						dflow = __max(zhejian * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);

	//						//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
	//					}

	//				}
	//				else
	//				{
	//					if (dev >= 0)
	//					{
	//						dflow = __min(1.0 * dev / Der, node_link[i]->volume);
	//					}
	//					else
	//					{
	//						dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
	//					}

	//				}

	//				cout << "dev: " << dev << " Der: " << Der << " dev / Der: " << dev / Der << " dflow: " << dflow << endl;

	//				node_link[i]->volume = node_link[i]->prevolume - dflow;
	//				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

	//				if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
	//				{
	//					//cout<<"yes!!!!!!!"<<endl;
	//					//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;

	//					//cout<<"-------------------"<<endl;
	//					//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;

	//					//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
	//					//cout<<"CESHI: "<<CESHI1<<endl;

	//					////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)

	//					//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
	//					//cout<<"-------------------"<<endl;

	//					//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;

	//					//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);

	//					//cout<<"ceshi: "<<ceshi<<endl;

	//					if (flow = 0.0)
	//					{

	//						cout << "wei 0!" << endl;
	//					}

	//					cout << "ttflow: " << flow << endl;

	//					cout << "minindex: " << node_link[minindex]->m_linkPtr->id << " minindex->prevolume: " << node_link[minindex]->prevolume << endl;

	//					cout << "minindex->eff: " << node_link[minindex]->m_linkPtr->efffreq << " minindex->type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << endl;

	//					cout << "i: " << node_link[i]->m_linkPtr->id << " i->prevolume: " << node_link[i]->prevolume << endl;

	//					cout << "i->eff: " << node_link[i]->m_linkPtr->efffreq << " i->type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << " " << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << endl;

	//					cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;
	//					node_link[i]->m_linkPtr->UpdateEffectiveFreq_TEST();

	//					cout << "再来一遍i->eff: " << node_link[i]->m_linkPtr->efffreq << endl;

	//					cout << "dev: " << dev << " Der: " << Der << " 1.0 * dev / Der: " << 1.0 * dev / Der << " shift_flow: " << dflow << ",i->aftervolume: " << node_link[i]->volume << ",minindex->aftervolume: " << node_link[minindex]->volume << endl;
	//					system("PAUSE");
	//				}

	//				//load（加载流量）
	//				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
	//				if (node_link[i]->m_linkPtr->rLink)
	//				{
	//					node_link[i]->m_linkPtr->rLink->volume += (node_link[i]->volume - node_link[i]->prevolume);
	//				}

	//				//cout << node_link[i]->m_linkPtr->rLink->id<<" "<<node_link[i]->prevolume << " " << node_link[i]->volume << " " << node_link[i]->m_linkPtr->rLink->volume << endl;

	//				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
	//				if (node_link[minindex]->m_linkPtr->rLink)
	//				{
	//					node_link[minindex]->m_linkPtr->rLink->volume += (node_link[minindex]->volume - node_link[minindex]->prevolume);
	//				}
	//				//cout << node_link[minindex]->m_linkPtr->rLink->id<<" "<<node_link[minindex]->prevolume << " " << node_link[minindex]->volume << " " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

	//				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
	//				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

	//				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
	//				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

	//				cout << "link->id: " << node_link[i]->m_linkPtr->id << " type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[i]->volume << " efffreq: " << node_link[i]->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << " rlink->id: " << node_link[i]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[i]->m_linkPtr->rLink->volume << endl;

	//				cout << "link->id: " << node_link[minindex]->m_linkPtr->id << " type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[minindex]->volume << " efffreq: " << node_link[minindex]->m_linkPtr->efffreq << " cost: " << node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq << " rlink->id: " << node_link[minindex]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

	//				//cout << "-------------------------------------------------------" << endl;
	//				//cout << node_link[i]->m_linkPtr->id<<" "<<node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << " "<< node_link[i]->m_linkPtr->efffreq<<endl;

	//				//cout << node_link[minindex]->m_linkPtr->id <<" "<<node_link[minindex]->m_linkPtr->rLink->volume << " " << node_link[minindex]->m_linkPtr->cap << " "<<node_link[minindex]->m_linkPtr->efffreq << endl;

	//				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
	//				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
	//				//GLINK* glink = node_link[i];
	//				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
	//			}
	//			//GLINK* glink = node_link[i];

	//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;

	//		}

	//		//compute gap
	//		iter++;
	//		GLINK* glink = node_link[0];
	//		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
	//		glink->gap = aa;
	//		gap = 0.0;
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			GLINK* glink = node_link[i];
	//			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
	//			glink->gap = temp;

	//			gap += pow(aa - temp, 2);

	//			//GLINK* glink = node_link[i];
	//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
	//		}

	//		cout << "GAP: " << gap << endl;

	//	} while (gap > 0.0000001 && iter < 1000);
	//	system("pause");
	//}

	//update m_data（更新概率）
	floatType ttfreq = 0.0;
	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
	}

	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];

		glink->effective_freq = glink->m_linkPtr->efffreq;

		glink->temp_value = glink->m_linkPtr->efffreq / ttfreq;
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];
		//glink->m_head->flow = glink->volume;//?????
		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
		//glink->m_head->flow += glink->volume - glink->prevolume;

		//glink->effective_freq = glink->m_linkPtr->efffreq;

		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		if (glink->m_linkPtr->volume < -1e-5)
		{
			cout << "正常计算： " << glink->m_linkPtr->id << " 's volume: " << glink->m_linkPtr->volume << " less than -1e-8" << endl;
			//cout << endl;
		}
		//glink->m_linkPtr->UpdatePTLinkCost();
		//glink->m_linkPtr->UpdatePTDerLinkCost();

		/*不考虑拥挤*/
		//glink->m_linkPtr->UpdatePTLinkCost_const();
		//glink->m_linkPtr->UpdatePTDerLinkCost_const();

		glink->m_linkPtr->UpdateEffectiveFreq();

		if (glink->m_linkPtr->rLink)
		{
			//cout<<glink->m_linkPtr->rLink->volume<<endl;
			glink->m_linkPtr->rLink->temptemp = glink->m_linkPtr->rLink->volume;
			glink->m_linkPtr->rLink->volume -= glink->volume;//为什么？？？-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数

			if (glink->rlink_already_inflow)
			{
				glink->m_linkPtr->rLink->volume -= fuyuanzhi;
			}

			if (glink->m_linkPtr->rLink->volume < -1e-5)
			{
				cout << glink->m_linkPtr->rLink->id << " 's volume: " << glink->m_linkPtr->rLink->volume << " is less than -1e-8" << endl;
				cout << "pre_volume is: " << glink->m_linkPtr->rLink->temptemp << " glink->volume is: " << glink->volume << endl;

				cout << "--------------------------------------------" << endl;

				system("PAUSE");
				//cout << endl;
			}

			//glink->m_linkPtr->rLink->UpdatePTLinkCost();
			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			//glink->m_linkPtr->rLink->UpdatePTLinkCost_const();
			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

			//glink->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
	}

	return 0;
}

int	GNODE::SolveFPwithGP()
{

	//if (flow == 0 || flow < 1e-15)
	if (flow == 0 || ((flow < 1e-15) && (flow > 0)))
	{
		floatType ttfreq = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->m_linkPtr->UpdateEffectiveFreq();
			ttfreq += node_link[i]->m_linkPtr->efffreq;
		}

		for (int i = 0; i < node_link.size(); i++)
		{

			node_link[i]->volume = 0.0;
			node_link[i]->prevolume = 0.0;
			node_link[i]->m_linkPtr->volume += flow;//由glink加载到ptlink中
			if (node_link[i]->m_linkPtr->rLink)
			{
				node_link[i]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
			}

			node_link[i]->m_head->flow += flow;//更新头节点的进入流量

			node_link[i]->m_data = m_data * (node_link[i]->m_linkPtr->efffreq / ttfreq);//更新link的使用概率（因为只有一条boarding_link）
			node_link[i]->m_head->m_data += node_link[i]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

			if (node_link[i]->m_linkPtr->volume < -1e-8)
			{
				cout << "flow = 0.0: "<<node_link[i]->m_linkPtr->id << " 's volume: " << node_link[0]->m_linkPtr->volume << " less than -1e-8" << endl;
				cout << endl;
			}
			//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
			//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导

			/*不考虑拥挤*/
			//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
			//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();

			//node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

			if (node_link[i]->m_linkPtr->rLink)
			{
				if (node_link[i]->m_linkPtr->rLink->volume < -1e-8)
				{
					cout << "flow = 0.0(rlink): " << node_link[i]->m_linkPtr->rLink->id << " 's volume: " << node_link[i]->m_linkPtr->rLink->volume << "less than -1e-8" << endl;
					cout << endl;
				}
				node_link[i]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
				//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
				//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();

				/*不考虑拥挤*/
				//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
				//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

				//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
			}
		}

		return 0;
	}

	if (node_link.size() == 1)
	{
		node_link[0]->prevolume = flow;//由节点的流量（ = hyperpath到达该节点的流量） 加载到对应的glink中
		node_link[0]->volume = flow; //加载hyperpath的流量(flow)

		node_link[0]->m_linkPtr->temp_temp = node_link[0]->m_linkPtr->volume;
		node_link[0]->m_linkPtr->volume += flow;//由glink加载到ptlink中

		if (node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += flow;//更新rlink的流量，如果只有一条boarding_link，则将全部流量加载到rlink（行驶弧）中
		}

		node_link[0]->m_head->flow += flow;//更新头节点的进入流量

		node_link[0]->m_data = m_data;//更新link的使用概率（因为只有一条boarding_link）
		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新头节点的使用概率（因为只有一条boarding_link）

		if (node_link[0]->m_linkPtr->volume < -1e-8)
		{

			cout << "ttflow: " << flow << endl;
			cout << "node_link[0]->m_linkPtr->before_volume: " << node_link[0]->m_linkPtr->temp_temp << endl;
			cout << "node_link.size() == 1: "<<node_link[0]->m_linkPtr->id << " 's volume: " << node_link[0]->m_linkPtr->volume << " less than -1e-10"<<endl;
			system("PAUSE");
		}
		//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导

		/*不考虑拥挤*/
		//node_link[0]->m_linkPtr->UpdatePTLinkCost_const();
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost_const();

		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

		if (node_link[0]->m_linkPtr->rLink)
		{
			if (node_link[0]->m_linkPtr->rLink->volume < -1e-8)
			{
				cout <<"node_link.size()(rlink) == 1: "<<node_link[0]->m_linkPtr->rLink->id << " 's volume: " << node_link[0]->m_linkPtr->rLink->volume << " less than -1e-10" << endl;
				cout << endl;
			}
			node_link[0]->m_linkPtr->rLink->volume -= flow;//？？？？（为什么加完后又减了等量的flow）-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost_const();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		return 0;
	}

	//init
	floatType MAX_CAP = 0.0;
	bool if_already_exceed = false;
	bool must_exceed_state = false;
	bool all_exceed_state = false;
	int already_exceed_num = 0;
	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];

		glink->if_exceed = false;

		//cout << "id: " << "eff_freq: " << glink->m_linkPtr->efffreq << " " << glink->m_linkPtr->rLink->id << " glink->m_linkPtr->rLink->volume: " << glink->m_linkPtr->rLink->volume << " "<< glink->m_linkPtr->rLink->cap<<endl;

		if (glink->m_linkPtr->rLink->volume < glink->m_linkPtr->rLink->cap)
		{
			glink->max_blink_flow = glink->m_linkPtr->rLink->cap - glink->m_linkPtr->rLink->volume;

			MAX_CAP += glink->max_blink_flow;
		}

		if (glink->m_linkPtr->rLink->volume >= glink->m_linkPtr->rLink->cap)
		{
			glink->volume = 0.0;
			already_exceed_num += 1;
			if_already_exceed = true;
		}

		if (glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -1e-10)//如果boarding_link上有数值，更新当前link和rlink(当前link初始化为0；rlink增加对应的数值)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}

	/////增加警告！！！！！！

	if (!if_already_exceed)
	{
		must_exceed_state = false;
		if (MAX_CAP < flow)
		{
			//cout << "Error! The station is required to allocate a flow exceeding its capacity." << endl;

			//cout << flow << " " << MAX_CAP << endl;
			must_exceed_state = true;
			//system("PAUSE");
		}

		//floatType RATIO = 1.0 / 20.0;

		if (must_exceed_state)
		{
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}

		}
		else
		{
			floatType shift_flow = 0.0;
			int exceed_blink_num = 0;
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				if (node_link[i]->volume > node_link[i]->max_blink_flow)
				{

					shift_flow += (node_link[i]->volume - node_link[i]->max_blink_flow * 0.99);

					node_link[i]->volume = node_link[i]->max_blink_flow * 0.99;
					node_link[i]->prevolume = node_link[i]->max_blink_flow * 0.99;

					exceed_blink_num += 1;

					node_link[i]->if_exceed = true;
				}
			}

			//cout << shift_flow << " " << (node_link.size() - exceed_blink_num) << endl;
			//floatType shift_flow = 0.0;
			for (int i = 0; i < node_link.size(); i++)
			{
				if (!node_link[i]->if_exceed)
				{
					node_link[i]->volume += (shift_flow / (node_link.size() - exceed_blink_num));

					node_link[i]->prevolume += (shift_flow / (node_link.size() - exceed_blink_num));
				}

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}

				node_link[i]->if_exceed = false;
			}

		}
	}
	else
	{
		all_exceed_state = false;
		if (already_exceed_num == node_link.size())
		{
			//cout << "Error! All lines at this stop exceed their capacity!" << endl;
			all_exceed_state = true;
			//system("PAUSE");
		}

		if (all_exceed_state)
		{
			for (int i = 0; i < node_link.size(); i++)
			{

				node_link[i]->volume = flow / node_link.size();
				node_link[i]->prevolume = flow / node_link.size();

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}

		}
		else
		{
			for (int i = 0; i < node_link.size(); i++)
			{
				if (node_link[i]->m_linkPtr->rLink->volume < node_link[i]->m_linkPtr->rLink->cap)
				{

					node_link[i]->volume = (flow - already_exceed_num * 0.0) / (node_link.size() - already_exceed_num);
				}

				node_link[i]->prevolume = node_link[i]->volume;

				node_link[i]->m_head->flow += node_link[i]->prevolume;
				node_link[i]->m_linkPtr->volume += node_link[i]->prevolume;

				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->prevolume;
				}
			}
		}
	}

	//cout << "初始: " << endl;
	//floatType ttflow = 0.0;
	for (int i = 0; i < node_link.size(); i++)
	{
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr（VI问题的cost）

		GLINK* glink = node_link[i];

		//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << "flow: " << glink->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << glink->cost << " rlink->id: " << glink->m_linkPtr->rLink->id << " rlink->volume: " << glink->m_linkPtr->rLink->volume << endl;

		//ttflow += glink->volume;

	}
	//cout << flow << endl;
	//cout << "-----------------------------------------------" << endl;

	//main loop
	floatType gap = 0.0;
	int count = 0;
	floatType zhejian = 0.8;
	//vector<floatType> gap_vector;
	do
	{
		for (int i = 0; i < node_link.size(); i++)
		{
			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
		}
		//update cost（找出最小cost的b_link）
		floatType mincost = 1e25;
		int minindex;
		for (int i = 0; i < node_link.size(); i++)
		{
			if (node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

		//cout << "Iter: " << count << "----------------------------------" << endl;
		//GP（GP法转移流量）
		for (int i = 0; i < node_link.size(); i++)
		{

			if (i != minindex)
			{
				//node_link[minindex]->m_linkPtr->temptemp = node_link[minindex]->m_linkPtr->volume;
				//node_link[i]->m_linkPtr->temptemp = node_link[i]->m_linkPtr->volume;

				//if (node_link[i]->m_linkPtr->temptemp < 0 || node_link[minindex]->m_linkPtr->temptemp < 0)
				//{

				//	cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;

				//	system("PAUSE");
				//}

				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
				node_link[i]->m_data = node_link[i]->volume / flow;

				node_link[minindex]->prevolume = node_link[minindex]->volume;
				node_link[i]->prevolume = node_link[i]->volume;

				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
				node_link[i]->UpdateDer();

				node_link[i]->max_blink_flow = node_link[i]->m_linkPtr->rLink->cap - node_link[i]->m_linkPtr->rLink->volume;//更新最大转移量
				node_link[minindex]->max_blink_flow = node_link[minindex]->m_linkPtr->rLink->cap - node_link[minindex]->m_linkPtr->rLink->volume;

				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq

				//cout << "node_link[i]->der: " << node_link[i]->der << " node_link[minindex]->der: " << node_link[minindex]->der << endl;

				if (Der == 0)	Der = 1e-8;
				double dev = node_link[i]->cost - node_link[minindex]->cost;
				double dflow;
				
				if (!if_already_exceed)
				{
					if (must_exceed_state)
					{
						if (dev >= 0)
						{
							dflow = __min(1.0 * dev / Der, node_link[i]->volume);
						}
						else
						{
							dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
						}
					}

					else
					{
						if (dev >= 0)
						{
							//dflow = __min(1.0 * dev / Der, node_link[i]->volume);

							//zhejian = 0.1;

							//if ( (dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
							//{
							//	zhejian = 0.01;
							//}
							if (zhejian > 0.01)
							{
								if ((dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
								{
									zhejian = 0.01;
								}
								//zhejian = 0.01;

								else
								{
									zhejian = 0.8;
								}

							}

							//dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);

							dflow = __min(zhejian * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
							//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
						}
						else
						{
							//dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);

							//zhejian = 0.1;

							//if ((-1.0 * dev / Der) > node_link[i]->max_blink_flow * 0.99)//调整下比例？改成0.5？？
							//{
							//	zhejian = 0.01;
							//}

							if (zhejian > 0.01)
							{
								if ((-dev / Der) > node_link[i]->max_blink_flow * 0.99)
								{
									zhejian = 0.01;
								}
								else
								{
									zhejian = 0.8;
								}

							}

							//dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
							dflow = __max(zhejian * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);

							//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
						}

					}
				}
				else
				{
					if (dev >= 0)
					{
						dflow = __min(1.0 * dev / Der, node_link[i]->volume);
					}
					else
					{
						dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
					}
				}

				//cout << "dev: " << dev << " Der: " << Der << " dev / Der: " << dev / Der << " dflow: " << dflow << endl;

				node_link[i]->volume = node_link[i]->prevolume - dflow;
				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

				if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
				{
					//cout<<"yes!!!!!!!"<<endl;
					//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;

					//cout<<"-------------------"<<endl;
					//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;

					//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
					//cout<<"CESHI: "<<CESHI1<<endl;

					////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)

					//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
					//cout<<"-------------------"<<endl;

					//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;

					//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);

					//cout<<"ceshi: "<<ceshi<<endl;

					if (flow = 0.0)
					{

						cout << "wei 0!" << endl;
					}

					cout << "ttflow: " << flow << endl;

					cout << "minindex: " << node_link[minindex]->m_linkPtr->id << " minindex->prevolume: " << node_link[minindex]->prevolume << endl;

					cout << "minindex->eff: " << node_link[minindex]->m_linkPtr->efffreq << " minindex->type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << endl;

					cout << "i: " << node_link[i]->m_linkPtr->id << " i->prevolume: " << node_link[i]->prevolume << endl;

					cout << "i->eff: " << node_link[i]->m_linkPtr->efffreq << " i->type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << " " << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << endl;

					
					cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;
					node_link[i]->m_linkPtr->UpdateEffectiveFreq_TEST();

					cout << "再来一遍i->eff: " << node_link[i]->m_linkPtr->efffreq << endl;

					cout <<"dev: "<<dev<<" Der: "<<Der<<" 1.0 * dev / Der: "<< 1.0 * dev / Der<<" shift_flow: " << dflow << ",i->aftervolume: " << node_link[i]->volume << ",minindex->aftervolume: " << node_link[minindex]->volume << endl;
					system("PAUSE");
				}

				//load（加载流量）
				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
				if (node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += (node_link[i]->volume - node_link[i]->prevolume);
				}

				//cout << node_link[i]->m_linkPtr->rLink->id<<" "<<node_link[i]->prevolume << " " << node_link[i]->volume << " " << node_link[i]->m_linkPtr->rLink->volume << endl;

				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
				if (node_link[minindex]->m_linkPtr->rLink)
				{
					node_link[minindex]->m_linkPtr->rLink->volume += (node_link[minindex]->volume - node_link[minindex]->prevolume);
				}
				//cout << node_link[minindex]->m_linkPtr->rLink->id<<" "<<node_link[minindex]->prevolume << " " << node_link[minindex]->volume << " " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

				//cout << "link->id: " << node_link[i]->m_linkPtr->id << " type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[i]->volume << " efffreq: " << node_link[i]->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << " rlink->id: " << node_link[i]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[i]->m_linkPtr->rLink->volume << endl;

				//cout << "link->id: " << node_link[minindex]->m_linkPtr->id << " type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[minindex]->volume << " efffreq: " << node_link[minindex]->m_linkPtr->efffreq << " cost: " << node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq << " rlink->id: " << node_link[minindex]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

				//cout << "-------------------------------------------------------" << endl;
				//cout << node_link[i]->m_linkPtr->id<<" "<<node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << " "<< node_link[i]->m_linkPtr->efffreq<<endl;

				//cout << node_link[minindex]->m_linkPtr->id <<" "<<node_link[minindex]->m_linkPtr->rLink->volume << " " << node_link[minindex]->m_linkPtr->cap << " "<<node_link[minindex]->m_linkPtr->efffreq << endl;

				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
				//GLINK* glink = node_link[i];
				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
			}
			//GLINK* glink = node_link[i];

			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;

		}

		//compute gap
		count++;
		GLINK* glink = node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		glink->gap = aa;
		gap = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			GLINK* glink = node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			glink->gap = temp;

			gap += pow(aa - temp, 2);

			//GLINK* glink = node_link[i];
			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
		}
		//cout << "GAP: " << gap << endl;

	} while (gap > 0.0000001 && count < 1000);//count = 1000; gap > 0.00001 (best parameter: 100)  常规是50 SF（1.8）：100    ****(SF(1.2/1.4): gap > 0.0000001 && count < 100)****

	//if (count == 1000 && flow < 0.00001)
	//{
	//	cout << "gap: " << gap << endl;
	//	cout << "count: " << count << endl;
	//	cout << "flow: "<<flow << endl;

	//	int iter = 0;

	//	do
	//	{
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
	//		}
	//		//update cost（找出最小cost的b_link）
	//		floatType mincost = 1e25;
	//		int minindex;
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			if (node_link[i]->cost < mincost)
	//			{
	//				mincost = node_link[i]->cost;
	//				minindex = i;
	//			}
	//		}

	//		cout << "Iter: " << iter << "----------------------------------" << endl;
	//		//GP（GP法转移流量）
	//		for (int i = 0; i < node_link.size(); i++)
	//		{

	//			if (i != minindex)
	//			{
	//				//node_link[minindex]->m_linkPtr->temptemp = node_link[minindex]->m_linkPtr->volume;
	//				//node_link[i]->m_linkPtr->temptemp = node_link[i]->m_linkPtr->volume;

	//				//if (node_link[i]->m_linkPtr->temptemp < 0 || node_link[minindex]->m_linkPtr->temptemp < 0)
	//				//{

	//				//	cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;

	//				//	system("PAUSE");
	//				//}

	//				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
	//				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;

	//				node_link[minindex]->m_data = node_link[minindex]->volume / flow;
	//				node_link[i]->m_data = node_link[i]->volume / flow;

	//				node_link[minindex]->prevolume = node_link[minindex]->volume;
	//				node_link[i]->prevolume = node_link[i]->volume;

	//				node_link[minindex]->UpdateDer();//这里的计算公式有问题？？？（pow((m_linkPtr->cap * 60 * m_linkPtr->freq + m_linkPtr->volume - m_linkPtr->rLink->volume),-1))）
	//				node_link[i]->UpdateDer();

	//				node_link[i]->max_blink_flow = node_link[i]->m_linkPtr->rLink->cap - node_link[i]->m_linkPtr->rLink->volume;//更新最大转移量
	//				node_link[minindex]->max_blink_flow = node_link[minindex]->m_linkPtr->rLink->cap - node_link[minindex]->m_linkPtr->rLink->volume;

	//				floatType Der = node_link[i]->der + node_link[minindex]->der;//  /node_link[minindex]->m_linkPtr->efffreq

	//				//cout << "node_link[i]->der: " << node_link[i]->der << " node_link[minindex]->der: " << node_link[minindex]->der << endl;

	//				if (Der == 0)	Der = 1e-8;
	//				double dev = node_link[i]->cost - node_link[minindex]->cost;
	//				double dflow;
	//				floatType zhejian = 1.0;
	//				if (!if_already_exceed)
	//				{
	//					if (dev >= 0)
	//					{
	//						//dflow = __min(1.0 * dev / Der, node_link[i]->volume);

	//						zhejian = 0.1;

	//						if ( (dev / Der) > node_link[minindex]->max_blink_flow * 0.99)
	//						{
	//							zhejian = 0.01;
	//						}

	//						//dflow = __min(0.01 * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);

	//						dflow = __min(zhejian * dev / Der, node_link[minindex]->max_blink_flow * 0.99, node_link[i]->volume);
	//						//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
	//					}
	//					else
	//					{
	//						//dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);

	//						zhejian = 0.1;

	//						if ((-1.0 * dev / Der) > node_link[i]->max_blink_flow * 0.99)//调整下比例？改成0.5？？
	//						{
	//							zhejian = 0.01;
	//						}

	//						//dflow = __max(0.01 * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);
	//						dflow = __max(zhejian * dev / Der, -node_link[i]->max_blink_flow * 0.99, -node_link[minindex]->volume);

	//						//cout<<1.0 * dev /Der<<","<< -node_link[minindex]->volume<<",dflow"<<dflow<<endl;
	//					}

	//				}
	//				else
	//				{
	//					if (dev >= 0)
	//					{
	//						dflow = __min(1.0 * dev / Der, node_link[i]->volume);
	//					}
	//					else
	//					{
	//						dflow = __max(1.0 * dev / Der, -node_link[minindex]->volume);
	//					}

	//				}

	//				cout << "dev: " << dev << " Der: " << Der << " dev / Der: " << dev / Der << " dflow: " << dflow << endl;

	//				node_link[i]->volume = node_link[i]->prevolume - dflow;
	//				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

	//				if (node_link[i]->volume < 0 || node_link[minindex]->volume < 0)
	//				{
	//					//cout<<"yes!!!!!!!"<<endl;
	//					//cout<<node_link[i]->volume<<" "<< node_link[minindex]->volume<<endl;

	//					//cout<<"-------------------"<<endl;
	//					//cout<<node_link[i]->m_linkPtr->efffreq<<" "<<node_link[i]->m_linkPtr->volume<<endl;

	//					//floatType CESHI1 =  ((0.2) * pow(node_link[i]->m_linkPtr->volume/(node_link[i]->m_linkPtr->cap + node_link[i]->m_linkPtr->volume - node_link[i]->m_linkPtr->rLink->volume),-0.8)) ;
	//					//cout<<"CESHI: "<<CESHI1<<endl;

	//					////floatType CESHI2 = (node_link[i]->m_linkPtr->cap - m_linkPtr->rLink->volume)/pow((m_linkPtr->cap + m_linkPtr->volume - m_linkPtr->rLink->volume),2)

	//					//cout<<node_link[i]->der<<" "<<node_link[minindex]->der<<endl;
	//					//cout<<"-------------------"<<endl;

	//					//cout<<dev<<" "<<Der<<" "<<1.0 * dev /Der<<" "<<-node_link[minindex]->prevolume<<" "<<node_link[i]->prevolume<<" "<<dflow<<endl;

	//					//floatType ceshi = __max(1.0 * dev /Der, -node_link[minindex]->prevolume);

	//					//cout<<"ceshi: "<<ceshi<<endl;

	//					if (flow = 0.0)
	//					{

	//						cout << "wei 0!" << endl;
	//					}

	//					cout << "ttflow: " << flow << endl;

	//					cout << "minindex: " << node_link[minindex]->m_linkPtr->id << " minindex->prevolume: " << node_link[minindex]->prevolume << endl;

	//					cout << "minindex->eff: " << node_link[minindex]->m_linkPtr->efffreq << " minindex->type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << endl;

	//					cout << "i: " << node_link[i]->m_linkPtr->id << " i->prevolume: " << node_link[i]->prevolume << endl;

	//					cout << "i->eff: " << node_link[i]->m_linkPtr->efffreq << " i->type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << " " << node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << endl;

	//					cout << "node_link[i]->m_linkPtr->prevolume: " << node_link[i]->m_linkPtr->temptemp << " node_link[minindex]->m_linkPtr->prevolume: " << node_link[minindex]->m_linkPtr->temptemp << endl;
	//					node_link[i]->m_linkPtr->UpdateEffectiveFreq_TEST();

	//					cout << "再来一遍i->eff: " << node_link[i]->m_linkPtr->efffreq << endl;

	//					cout << "dev: " << dev << " Der: " << Der << " 1.0 * dev / Der: " << 1.0 * dev / Der << " shift_flow: " << dflow << ",i->aftervolume: " << node_link[i]->volume << ",minindex->aftervolume: " << node_link[minindex]->volume << endl;
	//					system("PAUSE");
	//				}

	//				//load（加载流量）
	//				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
	//				if (node_link[i]->m_linkPtr->rLink)
	//				{
	//					node_link[i]->m_linkPtr->rLink->volume += (node_link[i]->volume - node_link[i]->prevolume);
	//				}

	//				//cout << node_link[i]->m_linkPtr->rLink->id<<" "<<node_link[i]->prevolume << " " << node_link[i]->volume << " " << node_link[i]->m_linkPtr->rLink->volume << endl;

	//				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
	//				if (node_link[minindex]->m_linkPtr->rLink)
	//				{
	//					node_link[minindex]->m_linkPtr->rLink->volume += (node_link[minindex]->volume - node_link[minindex]->prevolume);
	//				}
	//				//cout << node_link[minindex]->m_linkPtr->rLink->id<<" "<<node_link[minindex]->prevolume << " " << node_link[minindex]->volume << " " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

	//				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
	//				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

	//				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
	//				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

	//				cout << "link->id: " << node_link[i]->m_linkPtr->id << " type: " << node_link[i]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[i]->volume << " efffreq: " << node_link[i]->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << " rlink->id: " << node_link[i]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[i]->m_linkPtr->rLink->volume << endl;

	//				cout << "link->id: " << node_link[minindex]->m_linkPtr->id << " type: " << node_link[minindex]->m_linkPtr->GetTransitLinkTypeName() << "flow: " << node_link[minindex]->volume << " efffreq: " << node_link[minindex]->m_linkPtr->efffreq << " cost: " << node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq << " rlink->id: " << node_link[minindex]->m_linkPtr->rLink->id << " rlink->volume: " << node_link[minindex]->m_linkPtr->rLink->volume << endl;

	//				//cout << "-------------------------------------------------------" << endl;
	//				//cout << node_link[i]->m_linkPtr->id<<" "<<node_link[i]->m_linkPtr->rLink->volume << " " << node_link[i]->m_linkPtr->cap << " "<< node_link[i]->m_linkPtr->efffreq<<endl;

	//				//cout << node_link[minindex]->m_linkPtr->id <<" "<<node_link[minindex]->m_linkPtr->rLink->volume << " " << node_link[minindex]->m_linkPtr->cap << " "<<node_link[minindex]->m_linkPtr->efffreq << endl;

	//				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
	//				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/
	//				//GLINK* glink = node_link[i];
	//				//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
	//			}
	//			//GLINK* glink = node_link[i];

	//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;

	//		}

	//		//compute gap
	//		iter++;
	//		GLINK* glink = node_link[0];
	//		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
	//		glink->gap = aa;
	//		gap = 0.0;
	//		for (int i = 0; i < node_link.size(); i++)
	//		{
	//			GLINK* glink = node_link[i];
	//			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
	//			glink->gap = temp;

	//			gap += pow(aa - temp, 2);

	//			//GLINK* glink = node_link[i];
	//			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
	//		}

	//		cout << "GAP: " << gap << endl;

	//	} while (gap > 0.0000001 && iter < 1000);
	//	system("pause");
	//}

	//update m_data（更新概率）
	floatType ttfreq = 0.0;
	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
	}

	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];

		glink->effective_freq = glink->m_linkPtr->efffreq;

		glink->temp_value = glink->m_linkPtr->efffreq / ttfreq;
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	for (int i = 0; i < node_link.size(); i++)
	{
		GLINK* glink = node_link[i];
		//glink->m_head->flow = glink->volume;//?????
		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
		//glink->m_head->flow += glink->volume - glink->prevolume;

		//glink->effective_freq = glink->m_linkPtr->efffreq;

		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		if (glink->m_linkPtr->volume < -1e-8)
		{
			cout <<"正常计算： "<< glink->m_linkPtr->id << " 's volume: " << glink->m_linkPtr->volume << " less than -1e-8" << endl;
			//cout << endl;
		}
		//glink->m_linkPtr->UpdatePTLinkCost();
		//glink->m_linkPtr->UpdatePTDerLinkCost();

		/*不考虑拥挤*/
		//glink->m_linkPtr->UpdatePTLinkCost_const();
		//glink->m_linkPtr->UpdatePTDerLinkCost_const();

		glink->m_linkPtr->UpdateEffectiveFreq();

		if (glink->m_linkPtr->rLink)
		{
			//cout<<glink->m_linkPtr->rLink->volume<<endl;
			glink->m_linkPtr->rLink->temptemp = glink->m_linkPtr->rLink->volume;
			glink->m_linkPtr->rLink->volume -= glink->volume;//为什么？？？-之前是为了计算有效发车频率，这里复原，并重置费用、导数这些参数

			if (glink->m_linkPtr->rLink->volume < -1e-8)
			{
				cout << glink->m_linkPtr->rLink->id << " 's volume: " << glink->m_linkPtr->rLink->volume << " is less than -1e-8" << endl;
				cout << "pre_volume is: " << glink->m_linkPtr->rLink->temptemp << " glink->volume is: "<< glink->volume<<endl;

				cout << "--------------------------------------------" << endl;

				system("PAUSE");
				//cout << endl;
			}

			//glink->m_linkPtr->rLink->UpdatePTLinkCost();
			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost();

			/*不考虑拥挤*/
			//glink->m_linkPtr->rLink->UpdatePTLinkCost_const();
			//glink->m_linkPtr->rLink->UpdatePTDerLinkCost_const();

			//glink->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
	}

	return 0;
}

int	GNODE::SolveFPwithGPln()
{
	if(node_link.size() == 1)
	{
		node_link[0]->prevolume = flow;
		node_link[0]->volume = flow;
		node_link[0]->m_linkPtr->volume += flow;
		node_link[0]->m_linkPtr->rLink->volume += flow;

		node_link[0]->m_head->flow += flow;

		node_link[0]->m_data = m_data;
		node_link[0]->m_head->m_data += node_link[0]->m_data;

		node_link[0]->m_linkPtr->UpdatePTLinkCost();
		node_link[0]->m_linkPtr->UpdatePTDerLinkCost();
		node_link[0]->m_linkPtr->UpdateEffectiveFreq();

		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume -= flow;
			node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
			node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();
		}
		return 0;
	}

	//init

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink = node_link[i];
		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -0.00000001)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}

	for(int i = 0;i<node_link.size();i++)
	{
		node_link[i]->volume = flow / node_link.size();
		node_link[i]->prevolume = flow / node_link.size();
		node_link[i]->m_head->flow += node_link[i]->prevolume;
		node_link[i]->m_linkPtr->volume += flow / node_link.size();
		if(node_link[i]->m_linkPtr->rLink)
		{
			node_link[i]->m_linkPtr->rLink->volume += flow / node_link.size();
		}
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		node_link[i]->cost = log(1.0 / node_link[i]->m_linkPtr->efffreq);
	}

	//main loop
	floatType gap = 0.0;
	int count = 0;
	do
	{
		//update cost
		floatType mincost = 1000000000;
		int minindex;
		for(int i = 0;i<node_link.size();i++)
		{
			node_link[i]->cost = log(1.0 / node_link[i]->m_linkPtr->efffreq);
			if(node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

		//GP
		for(int i = 0;i<node_link.size();i++)
		{
			if(i != minindex)
			{
				node_link[minindex]->cost = log(1.0 / node_link[minindex]->m_linkPtr->efffreq);
				node_link[i]->cost = log(1.0 / node_link[i]->m_linkPtr->efffreq);
				node_link[minindex]->prevolume = node_link[minindex]->volume;
				node_link[i]->prevolume = node_link[i]->volume;
				node_link[minindex]->UpdateDerln();
				node_link[i]->UpdateDerln();

				floatType Der = node_link[i]->der- node_link[minindex]->der;
				if (Der ==0)	Der = 1e-8;
				double dev = node_link[i]->cost - node_link[minindex]->cost;
				double dflow;
				if (dev>=0) 
				{
					dflow =  __min(1.0 * dev /Der, node_link[i]->volume);
					//cout<<1.0 * dev /DerSum<<","<< path->flow<<",dflow"<<dflow<<endl;
				}
				else
				{
					dflow =  __max(1.0 * dev /Der, -node_link[minindex]->volume);	
					//cout<<1.0 * dev /DerSum<<","<< MinCostpath->flow<<",dflow"<<dflow<<endl;
				}

				node_link[i]->volume = node_link[i]->prevolume - dflow;
				node_link[minindex]->volume = node_link[minindex]->prevolume + dflow;

				if (node_link[i]->volume<0||node_link[minindex]->volume<0)
				{
					cout<<node_link[i]->volume<<","<<node_link[minindex]->volume<<endl;
					system("PAUSE");
				}

				//load
				node_link[i]->m_linkPtr->volume += node_link[i]->volume - node_link[i]->prevolume;
				if(node_link[i]->m_linkPtr->rLink)
				{
					node_link[i]->m_linkPtr->rLink->volume += node_link[i]->volume - node_link[i]->prevolume;
				}

				node_link[minindex]->m_linkPtr->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
				if(node_link[minindex]->m_linkPtr->rLink)
				{
					node_link[minindex]->m_linkPtr->rLink->volume += node_link[minindex]->volume - node_link[minindex]->prevolume;
				}

				node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;
				node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;

				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();

				/*tempglinks[minindex]->cost = log(tempglinks[minindex]->m_linkPtr->efffreq);
				tempglinks[i]->cost = log(tempglinks[i]->m_linkPtr->efffreq);*/

			}
		}

		//compute gap
		count++;
		GLINK* glink =  node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		gap = 0.0;
		for(int i = 0;i<node_link.size();i++)
		{
			GLINK* glink =  node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			gap += pow(aa-temp,2);
		}

	}while(gap > 0.00001 && count < 1000);

	//update m_data
	floatType ttfreq = 0.0;
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		//glink->m_head->flow = glink->volume;//?????
		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
		//glink->m_head->flow += glink->volume - glink->prevolume;
		glink->m_linkPtr->rLink->volume -= glink->volume;

		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		glink->m_linkPtr->UpdatePTLinkCost();
		glink->m_linkPtr->UpdatePTDerLinkCost();
		glink->m_linkPtr->UpdateEffectiveFreq();
		if(glink->m_linkPtr->rLink)
		{
			glink->m_linkPtr->rLink->UpdatePTLinkCost();
			glink->m_linkPtr->rLink->UpdatePTDerLinkCost();
			glink->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
	}

	return 0;
}

int	GNODE::SolveFPwithGPlinesearch_gcost(floatType lambda)
{
	/*如果只有一条boarding_link，那就只使用这条上车弧，流量相应加载*/
	if(node_link.size() == 1)
	{
		node_link[0]->prevolume = flow;//由节点的流量加载到上车弧（boarding_link）中
		node_link[0]->volume = flow;
		node_link[0]->m_linkPtr->volume += flow;
		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += flow;//更新行驶弧的流量（为了计算有效发车频率）
		}

		node_link[0]->m_head->flow += flow;//更新下一个节点（link的头节点）的流量

		node_link[0]->m_data = m_data;//更新行驶弧的概率
		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新下一个节点（link的头节点）的流量

		if(node_link[0]->m_linkPtr->volume < -1e-10)
		{cout<<endl;}
		node_link[0]->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//更新弧的general_cost和相应的一阶导
		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

		//重置行驶弧的流量
		if(node_link[0]->m_linkPtr->rLink)
		{
			if(node_link[0]->m_linkPtr->rLink->volume < -1e-10)
			{cout<<endl;}
			node_link[0]->m_linkPtr->rLink->volume -= flow;
			node_link[0]->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//更新弧的general_cost和相应的一阶导
			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		return 0;
	}

	//初始化
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink = node_link[i];
		
		//如果上车弧的流量为负，更新上车弧和行驶弧的流量
		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -0.00000001)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}
	
	//初始化boarding_link的流量（由站点处的流量均分到link中）和有效发车频率
	for(int i = 0;i<node_link.size();i++)
	{
		node_link[i]->volume = flow / node_link.size();
		node_link[i]->prevolume = flow / node_link.size();
		node_link[i]->m_head->flow += node_link[i]->prevolume;
		node_link[i]->m_linkPtr->volume += flow / node_link.size();
		if(node_link[i]->m_linkPtr->rLink)
		{
			node_link[i]->m_linkPtr->rLink->volume += flow / node_link.size();
		}
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		if(node_link[i]->m_linkPtr->efffreq < 0.0001)
		{
			//应该不需要
			//cout<<node_link[i]->m_linkPtr->freq<<","<<node_link[i]->m_linkPtr->volume<<","<<node_link[i]->m_linkPtr->cap<<","<<node_link[i]->m_linkPtr->rLink->volume<<endl;//(freq)*(1-pow(volume/(cap * 60 * freq+volume-rLink->volume),0.2));
		}
		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr
	}

	//main loop
	int count = 0;
	floatType gap = 0.0;
	do
	{
		//找出最短费用的线路（boarding_link）
		floatType mincost = 1000000000;
		int minindex;
		for(int i = 0;i<node_link.size();i++)
		{
			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
			if(node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

	
		for(int i = 0;i<node_link.size();i++)
		{
			node_link[i]->prevolume = node_link[i]->volume;
			node_link[minindex]->prevolume = node_link[minindex]->volume;
			if(i != minindex)
			{
				//以volume/eff作为boarding_link的cost
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
				int iter = 0; 
				int maxLineSearchIter = 100;
				floatType lineSearchAccuracy = 1e-12;
				floatType dev = node_link[i]->cost - node_link[minindex]->cost;
				floatType ldflow = 0.0,rdflow = 0.0;
				if (dev > 0) //非参考路径 > 参考路径
				{
					rdflow =  node_link[i]->volume;
				}
				else // //参考路径 < 非参考路径
				{
					ldflow = -node_link[minindex]->volume;	
				}
				floatType dflow = 0.0 ,lastdflow = 0.0;

				//两条路径（参考路径 vs 非参考路径）间通过二分法进行流量转移（之前应当判断一下最大转移量是否满足收敛条件？）
				while(iter < maxLineSearchIter && abs(dev)>=lineSearchAccuracy &&
					((dev>0 && node_link[i]->volume > 0) ||(dev<0 && node_link[minindex]->volume > 0)))
				{
					iter++;
					dflow = (ldflow + rdflow) / 2.0 ;//二分法确定转移量
					node_link[minindex]->volume = node_link[minindex]->volume + dflow - lastdflow; //这里的加减lastdflow是为了重置volume
					node_link[i]->volume = node_link[i]->volume - dflow + lastdflow;

					//更新头节点与plink的流量
					node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;
					node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;

					node_link[minindex]->m_linkPtr->volume +=  dflow -lastdflow;
					node_link[i]->m_linkPtr->volume +=  -dflow + lastdflow;

					//更新行驶弧的流量
					if(node_link[i]->m_linkPtr->rLink)
					{
						node_link[i]->m_linkPtr->rLink->volume +=  -dflow + lastdflow;
					}
					if(node_link[minindex]->m_linkPtr->rLink)
					{
						node_link[minindex]->m_linkPtr->rLink->volume +=  dflow -lastdflow;
					}

					//如果上车弧的流量为负，报错
					if((abs(node_link[i]->volume) > 1e-10 && node_link[i]->volume < 0)||(abs(node_link[minindex]->volume)> 1e-10 && node_link[minindex]->volume<0))
					{
						cout<<node_link[i]->volume<<","<<node_link[minindex]->volume<<endl;
						system("PAUSE");
					}

					/*if(node_link[i]->volume < 0 && node_link[i]->volume > -0.0000000001)
					{
						node_link[i]->m_linkPtr->volume -= node_link[i]->volume;
						node_link[i]->m_linkPtr->rLink->volume -= node_link[i]->volume;
						node_link[i]->volume = 0;
						node_link[minindex]->volume += node_link[i]->volume;
					}

					if(node_link[minindex]->volume < 0 && node_link[minindex]->volume > -0.0000000001)
					{
						node_link[minindex]->m_linkPtr->volume -= node_link[minindex]->volume;
						node_link[minindex]->m_linkPtr->rLink->volume -= node_link[minindex]->volume;
						node_link[minindex]->volume = 0;
						node_link[i]->volume += node_link[minindex]->volume;
					}*/
					

					node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
					node_link[i]->m_linkPtr->UpdateEffectiveFreq();

					node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
					node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;

					dev = node_link[i]->cost - node_link[minindex]->cost;
					lastdflow = dflow;
					node_link[minindex]->prevolume = node_link[minindex]->volume;
					node_link[i]->prevolume = node_link[i]->volume;

					if(dev > 0)
					{
						ldflow = dflow;
					}
					else  
					{
						rdflow = dflow;
					}

				}

				//转移完之后，再重新计算下参考路径和非参考路径的有效发车频率
				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;

			}
		}

		gap = 0.0;
		for(int i = 0; i < node_link.size(); i++)
		{
			gap += pow(node_link[i] - node_link[minindex], 2);
		}
		count++;

	}while(gap > 0.00001 && count < 1000);

	//update m_data（更新上车弧的使用概率）
	floatType ttfreq = 0.0;
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		//glink->m_head->flow = glink->volume;//?????
		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
		//glink->m_head->flow += glink->volume - glink->prevolume;
		
		//更新弧的使用概率和头节点的使用概率
		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		glink->m_linkPtr->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);//更新link的general_cost
		glink->m_linkPtr->UpdateEffectiveFreq();

		//重置行驶弧的状态
		if(glink->m_linkPtr->rLink)
		{
			glink->m_linkPtr->rLink->volume -= glink->volume;
			glink->m_linkPtr->rLink->TL_UpdateGeneralPTLink_Cost_DerCost(lambda);
		}
		//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
	}

	return 0;
}

int	GNODE::SolveFPwithGPlinesearch()
{
	if(node_link.size() == 1)//如果只有一条boarding_link，那就只使用这条上车弧，流量相应加载
	{
		node_link[0]->prevolume = flow;//由节点的流量加载到上车弧（boarding_link）中
		node_link[0]->volume = flow;
		node_link[0]->m_linkPtr->volume += flow;
		if(node_link[0]->m_linkPtr->rLink)
		{
			node_link[0]->m_linkPtr->rLink->volume += flow;//更新行驶弧的流量（为了计算有效发车频率）
		}

		node_link[0]->m_head->flow += flow;//更新下一个节点（link的头节点）的流量

		node_link[0]->m_data = m_data;//更新行驶弧的概率
		node_link[0]->m_head->m_data += node_link[0]->m_data;//更新下一个节点（link的头节点）的流量

		if(node_link[0]->m_linkPtr->volume < -1e-10)
		{cout<<endl;}

		//node_link[0]->m_linkPtr->UpdatePTLinkCost();//更新弧的费用
		//node_link[0]->m_linkPtr->UpdatePTDerLinkCost();//更新弧的一阶导
		node_link[0]->m_linkPtr->UpdateEffectiveFreq();//更新有效发车频率

		//重置行驶弧的流量
		if(node_link[0]->m_linkPtr->rLink)
		{
			if(node_link[0]->m_linkPtr->rLink->volume < -1e-10)
			{cout<<endl;}
			node_link[0]->m_linkPtr->rLink->volume -= flow;

			//node_link[0]->m_linkPtr->rLink->UpdatePTLinkCost();
			//node_link[0]->m_linkPtr->rLink->UpdatePTDerLinkCost();
			//node_link[0]->m_linkPtr->rLink->UpdateEffectiveFreq();
		}
		return 0;
	}

	//init
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink = node_link[i];
		
		//如果上车弧的流量为负，更新上车弧和行驶弧的流量
		if(glink->m_linkPtr->volume < 0 && glink->m_linkPtr->volume > -0.00000001)
		{
			glink->m_linkPtr->rLink->volume -= glink->m_linkPtr->volume;
			glink->m_linkPtr->volume = 0;
		}
	}

	floatType RATIO = 0.4;
	//floatType TT = 0.0;
	//初始化boarding_link的流量和有效发车频率
	for(int i = 0;i<node_link.size();i++)
	{
		node_link[i]->volume = flow / node_link.size();
		node_link[i]->prevolume = flow / node_link.size();

		/*just ceshi*/
		//if (i == 0)
		//{
		//	node_link[i]->volume = flow * (RATIO);
		//	node_link[i]->prevolume = flow * (RATIO);
		//}

		//if (i == 1)
		//{
		//	node_link[i]->volume = flow * (1 - RATIO);
		//	node_link[i]->prevolume = flow * (1 - RATIO);
		//}

		node_link[i]->m_head->flow += node_link[i]->prevolume;
		node_link[i]->m_linkPtr->volume += flow / node_link.size();

		/*just ceshi*/
		//if (i == 0)
		//{
		//	node_link[i]->m_linkPtr->volume += flow * (RATIO);
		//}

		//if (i == 1)
		//{
		//	node_link[i]->m_linkPtr->volume += flow * (1 - RATIO);
		//}

		if(node_link[i]->m_linkPtr->rLink)
		{
			node_link[i]->m_linkPtr->rLink->volume += flow / node_link.size();

			//if (i == 0)
			//{
			//	node_link[i]->m_linkPtr->rLink->volume += flow * (RATIO);
			//}

			//if (i == 1)
			//{
			//	node_link[i]->m_linkPtr->rLink->volume += flow * (1 - RATIO);
			//}
		}
		node_link[i]->m_linkPtr->UpdateEffectiveFreq();
		if(node_link[i]->m_linkPtr->efffreq < 0.0001)
		{
			//应该不需要
			//cout<<node_link[i]->m_linkPtr->freq<<","<<node_link[i]->m_linkPtr->volume<<","<<node_link[i]->m_linkPtr->cap<<","<<node_link[i]->m_linkPtr->rLink->volume<<endl;//(freq)*(1-pow(volume/(cap * 60 * freq+volume-rLink->volume),0.2));
		}

		node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;//->m_linkPtr

		/*GLINK* glink = node_link[i];
		cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << "flow: " << glink->m_linkPtr->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << glink->cost << endl;*/
	
		//TT += glink->m_linkPtr->efffreq;
	}

	//cout << "wait_cost: " << 1.0 / TT << endl;

	//cout << "=========================================" << endl;

	//main loop
	int count = 0;
	floatType gap = 0.0;
	int maxLineSearchIter = 100;
	floatType lineSearchAccuracy = 1e-5;
	do
	{
		//cout << "Iter: " <<count<<"---------------------------------------" << endl;

		//找出最短费用的线路（boarding_link）
		floatType mincost = 1e15;
		int minindex;
		for(int i = 0;i<node_link.size();i++)
		{
			node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
			if(node_link[i]->cost < mincost)
			{
				mincost = node_link[i]->cost;
				minindex = i;
			}
		}

	
		for(int i = 0;i<node_link.size();i++)
		{
			node_link[i]->prevolume = node_link[i]->volume;
			node_link[minindex]->prevolume = node_link[minindex]->volume;
			if(i != minindex)
			{
				//以volume/eff作为boarding_link的cost
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;
				int iter = 0; 
				floatType dev = node_link[i]->cost - node_link[minindex]->cost;
				floatType ldflow = 0.0,rdflow = 0.0;
				if (dev > 0) //非参考路径 > 参考路径
				{
					rdflow =  node_link[i]->volume;
				}
				else // //参考路径 < 非参考路径
				{
					ldflow = -node_link[minindex]->volume;	
				}
				floatType dflow = 0.0 ,lastdflow = 0.0;

				//两条路径（参考路径 vs 非参考路径）间通过二分法进行流量转移（之前应当判断一下最大转移量是否满足收敛条件？）
				while(iter < maxLineSearchIter && abs(dev) > lineSearchAccuracy &&
					((dev>0 && node_link[i]->volume > 0) ||(dev<0 && node_link[minindex]->volume > 0)))
				{
					iter++;
					dflow = (ldflow + rdflow) / 2.0 ;//二分法确定转移量
					node_link[minindex]->volume = node_link[minindex]->volume + dflow -lastdflow; //这里的加减lastdflow是为了重置volume
					node_link[i]->volume = node_link[i]->volume - dflow + lastdflow;

					//更新头节点与plink的流量
					node_link[minindex]->m_head->flow += node_link[minindex]->volume - node_link[minindex]->prevolume;
					node_link[i]->m_head->flow += node_link[i]->volume - node_link[i]->prevolume;

					node_link[minindex]->m_linkPtr->volume +=  dflow -lastdflow;
					node_link[i]->m_linkPtr->volume +=  -dflow + lastdflow;

					//更新行驶弧的流量
					if(node_link[i]->m_linkPtr->rLink)
					{
						node_link[i]->m_linkPtr->rLink->volume +=  -dflow + lastdflow;
					}
					if(node_link[minindex]->m_linkPtr->rLink)
					{
						node_link[minindex]->m_linkPtr->rLink->volume +=  dflow -lastdflow;
					}

					//如果上车弧的流量为负，报错
					if((abs(node_link[i]->volume) > 1e-10 && node_link[i]->volume < 0)||(abs(node_link[minindex]->volume)> 1e-10 && node_link[minindex]->volume<0))
					{
						cout<<node_link[i]->volume<<","<<node_link[minindex]->volume<<endl;
						system("PAUSE");
					}

					/*if(node_link[i]->volume < 0 && node_link[i]->volume > -0.0000000001)
					{
						node_link[i]->m_linkPtr->volume -= node_link[i]->volume;
						node_link[i]->m_linkPtr->rLink->volume -= node_link[i]->volume;
						node_link[i]->volume = 0;
						node_link[minindex]->volume += node_link[i]->volume;
					}

					if(node_link[minindex]->volume < 0 && node_link[minindex]->volume > -0.0000000001)
					{
						node_link[minindex]->m_linkPtr->volume -= node_link[minindex]->volume;
						node_link[minindex]->m_linkPtr->rLink->volume -= node_link[minindex]->volume;
						node_link[minindex]->volume = 0;
						node_link[i]->volume += node_link[minindex]->volume;
					}*/
					

					node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
					node_link[i]->m_linkPtr->UpdateEffectiveFreq();

					node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
					node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;

					dev = node_link[i]->cost - node_link[minindex]->cost;
					lastdflow = dflow;
					node_link[minindex]->prevolume = node_link[minindex]->volume;
					node_link[i]->prevolume = node_link[i]->volume;

					if(dev > 0)
					{
						ldflow = dflow;
					}
					else  
					{
						rdflow = dflow;
					}

					//floatType tt = 0.0;
					//for (int i = 0; i < node_link.size(); i++)
					//{
					//	GLINK* glink = node_link[i];

					//	tt += node_link[i]->m_linkPtr->efffreq;
					//	cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << " volume: " << glink->m_linkPtr->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
					//}
					//cout << iter<<" "<<"wait_cost: " << 1.0 / tt << endl;
					//cout << dev << endl;
					//cout << "-------------------------" << endl;

				}

				node_link[minindex]->m_linkPtr->UpdateEffectiveFreq();
				node_link[i]->m_linkPtr->UpdateEffectiveFreq();
				node_link[i]->cost = node_link[i]->volume / node_link[i]->m_linkPtr->efffreq;
				node_link[minindex]->cost = node_link[minindex]->volume / node_link[minindex]->m_linkPtr->efffreq;

			}

		}

		//gap = 0.0;
		//for(int i = 0;i<node_link.size();i++)
		//{
		//	gap += pow(node_link[i]-node_link[minindex],2);

		//	GLINK* glink = node_link[i];
		//	//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() << "flow: " << glink->m_linkPtr->volume << " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << glink->cost << endl;
		//}

		GLINK* glink = node_link[0];
		floatType aa = glink->volume / glink->m_linkPtr->efffreq;
		gap = 0.0;
		for (int i = 0; i < node_link.size(); i++)
		{
			GLINK* glink = node_link[i];
			floatType temp = glink->volume / glink->m_linkPtr->efffreq;
			gap += pow(aa - temp, 2);

			//GLINK* glink = node_link[i];

			//cout << "link->id: " << glink->m_linkPtr->id << " type: " << glink->m_linkPtr->GetTransitLinkTypeName() <<" volume: "<< glink->m_linkPtr->volume<< " efffreq: " << glink->m_linkPtr->efffreq << " cost: " << node_link[i]->volume / node_link[i]->m_linkPtr->efffreq << endl;
		}

		//cout << gap << endl;
		count++;

	}while(gap > lineSearchAccuracy && maxLineSearchIter < 50);

	//update m_data（更新上车弧的使用概率）
	floatType ttfreq = 0.0;
	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		ttfreq += glink->m_linkPtr->efffreq;
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		glink->m_data = glink->m_linkPtr->efffreq / ttfreq;// * glink->m_tail->m_data
	}

	for(int i = 0;i<node_link.size();i++)
	{
		GLINK* glink =  node_link[i];
		//glink->m_head->flow = glink->volume;//?????
		//glink->m_head->m_data = glink->m_data * glink->m_tail->m_data;
		//glink->m_head->flow += glink->volume - glink->prevolume;
		
		//更新弧的使用概率和头节点的使用概率
		glink->m_data = m_data * glink->m_data;
		glink->m_head->m_data += glink->m_data;

		glink->m_linkPtr->UpdatePTLinkCost();
		glink->m_linkPtr->UpdatePTDerLinkCost();
		glink->m_linkPtr->UpdateEffectiveFreq();

		//重置行驶弧的状态
		if(glink->m_linkPtr->rLink)
		{
			glink->m_linkPtr->rLink->volume -= glink->volume;
			glink->m_linkPtr->rLink->UpdatePTLinkCost();
			glink->m_linkPtr->rLink->UpdatePTDerLinkCost();
		}
		//cout<<glink->volume/glink->m_linkPtr->efffreq<<endl;
	}

	return 0;
}

void PTNET::UpdateHyperpathGPFlow_eff(PTDestination* dest,PTOrg* org)
{

}
