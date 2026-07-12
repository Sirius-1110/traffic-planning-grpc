#include "PTNet.h"
#include "TNM_utility.h"

int PTNET::BuildFromPostgresInputs()
{
	if (!m_stops.empty() || !m_routes.empty() || !m_shapes.empty() ||
		!nodeVector.empty() || !linkVector.empty() || !PTDestVector.empty())
	{
		cout << "PostgreSQL inputs can only be built into an empty PTNET." << endl;
		return 1;
	}

	cout << "\tBuilding transit assignment network from PostgreSQL inputs..." << endl;

	for (const PTDbStopRecord& record : dbStopRecords)
	{
		if (record.stopId.empty())
		{
			cout << "PostgreSQL stop_id cannot be empty." << endl;
			return 2;
		}

		PTStop* stop = new PTStop;
		stop->m_id = record.stopId;
		stop->m_name = record.stopName;
		stop->m_pos.SetPos(0.0, 0.0);

		pair<PTStopMapIter, bool> inserted =
			m_stops.insert(pair<string, PTStop*>(stop->m_id, stop));
		if (!inserted.second)
		{
			cout << "Duplicate stop_id in PostgreSQL input: "
				 << stop->m_id << endl;
			delete stop;
			return 2;
		}
		PTstopvec.push_back(stop);
	}

	for (const PTDbRouteRecord& record : dbRouteRecords)
	{
		if (record.routeId.empty())
		{
			cout << "PostgreSQL route_id cannot be empty." << endl;
			return 3;
		}

		PTRoute* route = new PTRoute;
		route->m_id = record.routeId;
		pair<PTRouteMapIter, bool> inserted =
			m_routes.insert(pair<string, PTRoute*>(route->m_id, route));
		if (!inserted.second)
		{
			cout << "Duplicate route_id in PostgreSQL input: "
				 << route->m_id << endl;
			delete route;
			return 3;
		}
	}

	string currentShapeId;
	PTShape* currentShape = NULL;
	int previousSequence = 0;
	for (const PTDbShapeStopRecord& record : dbShapeStopRecords)
	{
		if (record.frequency <= 0.0)
		{
			cout << "Shape " << record.shapeId
				 << " has non-positive frequency." << endl;
			return 4;
		}

		if (record.shapeId != currentShapeId)
		{
			currentShapeId = record.shapeId;
			previousSequence = 0;

			if (m_shapes.find(record.shapeId) != m_shapes.end())
			{
				cout << "Shape rows are not contiguous for shape_id: "
					 << record.shapeId << endl;
				return 4;
			}

			PTRoute* route = GetRoutePtr(record.routeId);
			if (!route)
			{
				cout << "Cannot find route " << record.routeId
					 << " for shape " << record.shapeId << endl;
				return 4;
			}

			currentShape = new PTShape;
			currentShape->m_id = record.shapeId;
			currentShape->SetRoute(route);
			currentShape->m_freq = record.frequency;

			// AON uses constant costs. This remains positive only because
			// legacy link objects expect a capacity value during construction.
			currentShape->m_cap = 1.0;
			m_shapes.insert(
				PTShapeMapIter::value_type(record.shapeId, currentShape));
		}
		else if (!currentShape ||
				 currentShape->m_routePtr->m_id != record.routeId ||
				 fabs(currentShape->m_freq - record.frequency) > 1e-9)
		{
			cout << "Inconsistent route or frequency in shape "
				 << record.shapeId << endl;
			return 4;
		}

		if (record.stopSequence <= previousSequence)
		{
			cout << "stop_sequence must increase within shape "
				 << record.shapeId << endl;
			return 4;
		}
		previousSequence = record.stopSequence;

		PTStop* stop = GetStopPtr(record.stopId);
		if (!stop)
		{
			cout << "Cannot find stop " << record.stopId
				 << " in shape " << record.shapeId << endl;
			return 4;
		}
		currentShape->AddStop(stop);
		if (find(stop->mshapes.begin(), stop->mshapes.end(), currentShape) ==
			stop->mshapes.end())
		{
			stop->mshapes.push_back(currentShape);
		}
	}

	if (CreateTEAPNodeLinks() != 0)
	{
		cout << "Failed to create transit nodes and links." << endl;
		return 5;
	}

	for (const PTDbTransitRecord& record : dbTransitRecords)
	{
		if (record.fft < 0.0)
		{
			cout << "Transit fft cannot be negative for shape "
				 << record.shapeId << endl;
			return 6;
		}

		PTShape* shape = GetShapePtr(record.shapeId);
		PTStop* fromStop = GetStopPtr(record.fromStop);
		PTStop* toStop = GetStopPtr(record.toStop);
		if (!shape || !fromStop || !toStop)
		{
			cout << "Invalid transit row for shape " << record.shapeId
				 << ": " << record.fromStop << " -> "
				 << record.toStop << endl;
			return 6;
		}

		PTNode* tail = fromStop->GetNode(shape->m_id + ";ENROUTE");
		PTNode* head = toStop->GetNode(shape->m_id + ";ENROUTE");
		PTLink* link = tail && head ? CatchLinkPtr(tail, head) : NULL;
		if (!link || link->GetTransitLinkType() != PTLink::ENROUTE)
		{
			cout << "Cannot match transit row to an ENROUTE link for shape "
				 << record.shapeId << ": " << record.fromStop
				 << " -> " << record.toStop << endl;
			return 6;
		}
		if (link->pars.size() != 5)
		{
			cout << "Duplicate transit row for shape " << record.shapeId
				 << ": " << record.fromStop << " -> "
				 << record.toStop << endl;
			return 6;
		}

		link->pars.push_back(record.fft);
		link->fft = record.fft;
	}

	for (PTLink* link : linkVector)
	{
		if (link->GetTransitLinkType() == PTLink::ENROUTE &&
			link->pars.size() != 6)
		{
			cout << "Missing transit fft for shape "
				 << link->m_shape->m_id << " at sequence "
				 << link->seq << endl;
			return 6;
		}
	}

	int nextLinkId = numOfLink;
	for (const PTDbWalkRecord& record : dbWalkRecords)
	{
		if (record.fft < 0.0 || record.length < 0.0)
		{
			cout << "Walk fft and length cannot be negative: "
				 << record.fromStop << " -> " << record.toStop << endl;
			return 7;
		}

		PTStop* fromStop = GetStopPtr(record.fromStop);
		PTStop* toStop = GetStopPtr(record.toStop);
		if (!fromStop || !toStop ||
			!fromStop->GetTransferNode() || !toStop->GetTransferNode())
		{
			cout << "Invalid walk row: " << record.fromStop
				 << " -> " << record.toStop << endl;
			return 7;
		}
		if (record.fromStop == record.toStop)
		{
			cout << "Walk row cannot connect a stop to itself: "
				 << record.fromStop << endl;
			return 7;
		}

		PTLink* walkLink = new PTLink(
			++nextLinkId,
			fromStop->GetTransferNode(),
			toStop->GetTransferNode());
		walkLink->SetTransitLinkType(PTLink::WALK);
		walkLink->pars.push_back(1.0);
		walkLink->pars.push_back(record.fft);
		walkLink->fft = record.fft;
		walkLink->length = record.length;
		linkVector.push_back(walkLink);
	}
	UpdateLinkNum();
	cout << "\tCreate " << dbWalkRecords.size()
		 << " walking links from PostgreSQL inputs" << endl;

	map<string, vector<const PTDbTripRecord*> > tripsByDestination;
	for (const PTDbTripRecord& record : dbTripRecords)
	{
		if (record.demand < 0.0)
		{
			cout << "Trip demand cannot be negative: "
				 << record.originStop << " -> "
				 << record.destinationStop << endl;
			return 8;
		}
		if (record.demand == 0.0)
		{
			continue;
		}
		tripsByDestination[record.destinationStop].push_back(&record);
	}

	for (const auto& destinationTrips : tripsByDestination)
	{
		PTStop* destinationStop = GetStopPtr(destinationTrips.first);
		if (!destinationStop || !destinationStop->GetTransferNode())
		{
			cout << "Cannot find destination stop "
				 << destinationTrips.first << endl;
			return 8;
		}

		PTDestination* destination = CreatePTDestination(
			destinationStop->GetTransferNode(),
			static_cast<int>(destinationTrips.second.size()));
		if (!destination)
		{
			cout << "Cannot create destination "
				 << destinationTrips.first << endl;
			return 8;
		}

		for (size_t index = 0; index < destinationTrips.second.size(); ++index)
		{
			const PTDbTripRecord& record = *destinationTrips.second[index];
			PTStop* originStop = GetStopPtr(record.originStop);
			if (!originStop || !originStop->GetTransferNode())
			{
				cout << "Cannot find origin stop "
					 << record.originStop << endl;
				return 8;
			}
			if (!destination->SetOrg(
					static_cast<int>(index) + 1,
					originStop->GetTransferNode(),
					record.demand))
			{
				cout << "Cannot create OD demand "
					 << record.originStop << " -> "
					 << record.destinationStop << endl;
				return 8;
			}
			destination->m_tdmd += record.demand;
			numOfPTTrips += record.demand;
		}
	}

	ConnectAsymmetricLinks();

	cout << "============= node size:" << numOfNode
		 << ",link size:" << numOfLink
		 << ",od-pair size:" << numOfPTOD
		 << ", trips:" << numOfPTTrips
		 << ",numberofdestination:" << numOfPTDest
		 << "=============" << endl;
	return 0;
}

bool PTStop::TAPInitialize(const string &inf,bool isPos)
{
	vector<string> words;
	TNM_GetWordsFromLine(inf, words, ',', '"');
	if(words.size() <4)
	{
		cout<<"expected at least four columns, only found "<<words.size()<<" from line: "<<inf<<endl;
		return false;
	}
	m_id=words[0];
	m_name = words[1];
	if (isPos) // txt info inculde stop position
	{
		double lat, lon;
		if (TNM_FromString(lat, words[2], std::dec) && TNM_FromString(lon, words[3], std::dec))
		{
			m_pos.SetPos(lat,lon);
		}
	}
	else m_pos.SetPos(0.0,0.0);
	
	return true;
}

int  PTNET::ReadTEAPStops(bool ispos)
{
	string stopname = networkName + "_stop.txt";
	ifstream infile;
    if(!TNM_OpenInFile(infile, stopname))
    {
        return 1;
    }
	string pline;
    vector<string> words;
    cout<<"\tReading the stop file..."<<endl;
	if(!getline(infile, pline))//skip the first line
    {
        cout<<"Failed to read the stop informaiton from the following line: \n"<<pline<<endl;
        return 2;
    }

	 while(getline(infile, pline))
    {
        if(!pline.empty())//skip an empty line
        {
            PTStop *stop = new PTStop;
			stop->TAPInitialize(pline,ispos);//does 
			pair<PTStopMapIter,bool> ret = m_stops.insert(pair<string, PTStop*>(stop->m_id, stop));//map
			if(!ret.second)
            {
                cout<<"\tThe following stop may be duplicated and shall not be read: "<<endl;
                //stop->Print();
                delete stop;
            }
			else
			{
				PTstopvec.push_back(stop);
			}
		}
	 }

	cout<<"\tRead in "<<m_stops.size()<<" stops"<<endl;

    infile.close();
	return 0;
}

int  PTNET::ReadTEAPRoutes()
{
	string routename = networkName + "_route.txt";

	ifstream infile;
    if(!TNM_OpenInFile(infile, routename))
    {
        return 1;
    }
    string pline;
    vector<string> words;
    cout<<"\tReading the route file..."<<endl;
    if(!getline(infile, pline))
    {
        cout<<"Failed to read the route informaiton from the following line: "<<pline<<endl;
        return 2;
    }//skip the first line
    while(getline(infile, pline))
    {
        if(!pline.empty())//skip an empty line
        {
            PTRoute *route = new PTRoute;
			vector<string> words;
			TNM_GetWordsFromLine(pline, words, ',', '"');
			
			//cout<<pline<<endl;
			if(words.size() <4 || !TNM_FromString(route->m_type, words[3], std::dec))
			{
				cout<<"expected at least four columns, only found "<<words.size()<<" from line: "<<pline<<endl;
				return false;
			}
			route->m_id    = words[0];
			route->m_sname = words[1];
			route->m_lname = words[2];

			pair<PTRouteMapIter,bool> ret = m_routes.insert(pair<string, PTRoute*>(route->m_id, route));
			if(!ret.second)//去除插入失败的route
			{
				cout<<"\tThe following route may be duplicated and shall not be read: "<<endl;
				delete route;
			}
			
        }
    }

	cout<<"\tRead in "<<m_routes.size()<<" routes"<<endl;
    infile.close();
    return 0;


}

int	PTNET::ReadTEAPShapes()
{
	string infilename = networkName + "_shape.txt";
    ifstream infile;
    if(!TNM_OpenInFile(infile, infilename))
    {
        cout<<"\tCannot open file "<<infilename<<" to read"<<endl;
        return 1;
    }
    cout<<"\tReading shape pattern information "<<endl;
    string line;
    vector<string> words;
    getline(infile, line);
	double ttfreq = 0.0;
    while(getline(infile, line))
    {
		TNM_GetWordsFromLine(line, words,',');
		if(words.size() < 6)
        {
            cout<<"\tExpected at lest six columns, found "<<words.size()<<" on "<<line<<endl;
            return 1;
        }
		PTShape *shape = new PTShape;
		string shapeid=words[0];
		shape->m_id = shapeid;
		PTRoute *route = GetRoutePtr(words[1]);
		if(!route)
        {
            cout<<"\tCannot find route "<<words[1]<<" on line "<<line<<endl;
            return 1;
        }
		shape->SetRoute(route);
		double f,cap;
		TNM_FromString(f, words[2], std::dec);
		TNM_FromString(cap, words[3], std::dec);
		f = f / 1.0;//For test!
		shape->m_cap = f * cap;//线路容量
		shape->m_freq = f;
		ttfreq += f;
		for(int i = 4;i<words.size();i++)
        {
            PTStop *stop = GetStopPtr(words[i]);
            if(stop)
            {
                shape->AddStop(stop);
				// add lines to stop 
				vector<PTShape*>::iterator ittt = find(stop->mshapes.begin(), stop->mshapes.end(), shape);
				if (ittt==stop->mshapes.end())
					stop->mshapes.push_back(shape);
            }
            else
            {
                cout<<"\tCannot find stop "<<words[i]<<" from line "<<line<<endl;
                return 3;
            }
        }
		pair<PTShapeMapIter,bool> ret =m_shapes.insert(PTShapeMapIter::value_type(shapeid, shape));
        if(!ret.second)
        {
            cout<<"\tThe following stop may be duplicated and shall not be read: "<<line<<endl;
            delete shape;
        }

	}
	cout<<"\tRead in total "<<m_shapes.size()<<" shapes, total freqency = "<<ttfreq<<endl;
    infile.close();
    return 0;
}

int	PTNET::ReadTEAPTransitData()
{
	string infilename = networkName + "_transit.txt";
    ifstream infile;
    if(!TNM_OpenInFile(infile, infilename))
    {
		cout<<"_transit.txt does not exist!"<<endl;
		return 1;
	}
	cout<<"\tReading transit running data from transit.txt"<<endl;

	string line;
    vector<string> words;      
	getline(infile, line);

	while(getline(infile, line))
    {
		TNM_GetWordsFromLine(line, words,',');
        if(words.size() != 5)
        {
            cout<<"\tExpected five columns, found "<<words.size()<<" on "<<line<<endl;
            return 1;
        }
		string shapeid=words[0];
		PTShape* shp=GetShapePtr(words[0]);
		
		if (!shp)
		{
			cout<<"shape id "<<words[0]<<" does not exist in "<<line<<endl;
			return 3;		
		}
		PTStop* fstop=GetStopPtr(words[1]);
		PTStop* tstop=GetStopPtr(words[2]);
		if (!fstop||!tstop)
		{
			cout<<"stop id "<<words[0]<<" or "<<words[1]<<" does not exist in "<<line<<endl;
			return 4;		
		}
		if (words[1]!=words[2])
		{
			PTNode* tail;
			PTNode* head;
			tail=fstop->GetNode(shp->m_id+";ENROUTE");//?
			head=tstop->GetNode(shp->m_id+";ENROUTE");
			if (!tail||!head) 
			{
				cout<<"tranist link in shape "<<words[0]<<" in line"<<line<<"does not exist, check network representation!"<<endl;
				return 5;
			}
		
			PTLink *link = CatchLinkPtr(tail,head);
			if(link && link->GetTransitLinkType() == PTLink::ENROUTE) //link must be enroute
			{
				double fft,length;
				TNM_FromString(fft, words[3], std::dec);
				TNM_FromString(length, words[4], std::dec);
				link->pars.push_back(fft);//add transit free flow time to the parVector
				link->length = length;
				link->fft = fft;
			}
			else
			{
				cout<<"link must be enroute, check network representation!"<<endl;
				return 5;
			}
		}
		else
		{
			cout<<"Stop:"<<words[1]<<" to itself!"<<endl;
			return 5;
		}


	}
	infile.close();
	return 0;

}

int  PTNET::CreateTEAPWalks(bool Traj)
{
	int lid = numOfLink;
	int n=0;
	if (Traj)
	{
		cout << "\tStart to read walk file!" << endl;
		//without stop position, walk links comes from external file
		string walkname=networkName + "_walks.txt";
		ifstream infile;
		if(!TNM_OpenInFile(infile, walkname))
		{
			return 1;
		}
		string pline;
		vector<string> words;
		if(!getline(infile, pline))//skip the first line
		{
			cout<<"Failed to read the stop informaiton from the following line: \n"<<pline<<endl;
			return 2;
		}

		 while(getline(infile, pline))
		{
			if(!pline.empty())//skip an empty line
			{
				n++;
				TNM_GetWordsFromLine(pline, words,',','"');
				PTStop *fstop = GetStopPtr(words[0]);
				PTStop *tstop = GetStopPtr(words[1]);
				if (fstop&&tstop)
				{
					lid++;
					PTLink* walklink = new PTLink(lid,fstop->GetTransferNode(),tstop->GetTransferNode());
					walklink->SetTransitLinkType(PTLink::WALK);
					TNM_FromString(walklink->length, words[3], std::dec);
					double fft;
					TNM_FromString(fft, words[2], std::dec);
                    if(walklink==NULL)
                    {
						cout<<"\tFailed to create a walk link from stop:"<<fstop->m_id<<" to stop:"<<tstop->m_id<<endl;
                        return 2;
                    }     
					else
					{
						walklink->pars.push_back(1.0);//alpha_1
						walklink->pars.push_back(fft);
						walklink->fft = fft;
					}
					linkVector.push_back(walklink);
				}
				else
				{
					cout<<"Walking from the line"<<pline<<", can not find the stop!"<<endl;
					return 1;
				}
			}
		 }
		 infile.close();
	}
	else
	{
		// create walk link according to the stop position
		double walkSpeed = 5.0/60 * 1000 * 3.28084; //unit: ft per minutes. 
		//double walkRadius = walkSpeed * m_maxWalkTime; //total distance one can walk in five minutes.
		double walkRadius = 300.0 * 3.2808;
		COORDMAP xm, ym;
		for(PTStopMapIter ps = m_stops.begin(); ps!= m_stops.end(); ps++)
		{
			PTNode* node = ps->second->GetTransferNode();
			xm.insert(pair<long, int>(node->xCord, node->id));
			ym.insert(pair<long, int>(node->yCord, node->id));				
		}
		
		for(PTStopMapIter ps = m_stops.begin(); ps!= m_stops.end(); ps++)
		{
			PTNode* node = ps->second->GetTransferNode();
			if(node)  //A stop may not have a transfer node if it is not used by any pattern. 
			{
				std::vector<PTNode*> vnodes;
				GetVicinityNodes(xm, ym, vnodes, node, walkRadius);
				for(int i = 0;i<vnodes.size();i++)
				{
					PTNode* hnode = vnodes[i];
					if(hnode->GetTransitNodeType() == PTNode::TRANSFER && hnode!=node) //only if it is a transfer node. 
					{
						double dist = hnode->MeasureDist(node);
						if(dist <= walkRadius)
						{
							n++;
							lid++;
							PTLink* walklink = new PTLink(lid,node,hnode);
							walklink->SetTransitLinkType(PTLink::WALK);
							walklink->length = dist/3280.84;  //unit feet->km	
							if(walklink==NULL)
							{
								cout<<"\tFailed to create a walk link from node "<<node->id<<" to "<<hnode->id<<endl;
								return 2;
							}     
							else
							{
								walklink->pars.push_back(1.0);//alpha_1
								walklink->pars.push_back(walklink->length/5.0*60.0);
							}		
							linkVector.push_back(walklink);
						}
					}
				}
			}
		}
	
		
	}
	UpdateNodeNum();
	UpdateLinkNum();
	cout<<"\tCreate "<<n<<" walking links"<<endl;
	return 0;
}

int  PTNET::CreateTEAPWalksTP(bool Traj)
{
	int lid = numOfLink;
	int n=0;
	if (Traj)
	{
	
		//without stop position, walk links comes from external file
		string walkname=networkName + "_walks.txt";
		ifstream infile;
		if(!TNM_OpenInFile(infile, walkname))
		{
			return 1;
		}
		string pline;
		vector<string> words;
		if(!getline(infile, pline))//skip the first line
		{
			cout<<"Failed to read the stop informaiton from the following line: \n"<<pline<<endl;
			return 2;
		}

		 while(getline(infile, pline))
		{
			if(!pline.empty())//skip an empty line
			{
				n++;
				TNM_GetWordsFromLine(pline, words,',','"');
				PTStop *fstop = GetStopPtr(words[0]);
				PTStop *tstop = GetStopPtr(words[1]);
				if (fstop&&tstop)
				{
					lid++;
					PTLink* walklink = new PTLink(lid,fstop->GetTransferNodeTP(1),tstop->GetTransferNodeTP(0));
					walklink->SetTransitLinkType(PTLink::WALK);
					TNM_FromString(walklink->length, words[3], std::dec);
					double fft;
					TNM_FromString(fft, words[2], std::dec);
                    if(walklink==NULL)
                    {
						cout<<"\tFailed to create a walk link from stop:"<<fstop->m_id<<" to stop:"<<tstop->m_id<<endl;
                        return 2;
                    }     
					else
					{
						walklink->pars.push_back(1.0);//alpha_1
						walklink->pars.push_back(fft);
						walklink->fft = fft;
					}
					linkVector.push_back(walklink);
				}
				else
				{
					cout<<"Walking from the line"<<pline<<", can not find the stop!"<<endl;
					return 1;
				}
			}
		 }
		 infile.close();
	}
	else
	{
		// create walk link according to the stop position
		double walkSpeed = 5.0/60 * 1000 * 3.28084; //unit: ft per minutes. 
		//double walkRadius = walkSpeed * m_maxWalkTime; //total distance one can walk in five minutes.
		double walkRadius = 300.0 * 3.2808;
		COORDMAP xm, ym;
		for(PTStopMapIter ps = m_stops.begin(); ps!= m_stops.end(); ps++)
		{
			PTNode* node = ps->second->GetTransferNodeTP(1);
			xm.insert(pair<long, int>(node->xCord, node->id));
			ym.insert(pair<long, int>(node->yCord, node->id));				
		}
		
		for(PTStopMapIter ps = m_stops.begin(); ps!= m_stops.end(); ps++)
		{
			PTNode* node = ps->second->GetTransferNodeTP(1);
			if(node)  //A stop may not have a transfer node if it is not used by any pattern. 
			{
				std::vector<PTNode*> vnodes;
				GetVicinityNodes(xm, ym, vnodes, node, walkRadius);
				for(int i = 0;i<vnodes.size();i++)
				{
					PTNode* hnode = vnodes[i];
					if(hnode->GetTransitNodeType() == PTNode::TRANSFER && hnode!=node && hnode->m_type==0) //only if it is a transfer node. 
					{
						double dist = hnode->MeasureDist(node);
						if(dist <= walkRadius)
						{
							n++;
							lid++;
							PTLink* walklink = new PTLink(lid,node,hnode);
							walklink->SetTransitLinkType(PTLink::WALK);
							walklink->length = dist/3280.84;  //unit feet->km	
							if(walklink==NULL)
							{
								cout<<"\tFailed to create a walk link from node "<<node->id<<" to "<<hnode->id<<endl;
								return 2;
							}     
							else
							{
								walklink->pars.push_back(1.0);//alpha_1
								walklink->pars.push_back(walklink->length/5.0*60.0);
							}		
							linkVector.push_back(walklink);
						}
					}
				}
			}
		}
	
		
	}
	UpdateNodeNum();
	UpdateLinkNum();
	cout<<"\tCreate "<<n<<" walking links"<<endl;
	return 0;
}

int  PTNET::ReadTEAPODdemand()
{
	string odname = networkName + "_trip.txt";
	ifstream infile;
    if(!TNM_OpenInFile(infile, odname))
    {
        return 1;
    }
    string pline;
    vector<string> words,iwords;
	PTDestination* pDest;

    cout<<"\tReading the od demand file..."<<endl;
	if(!getline(infile, pline))//skip the first line
    {
        cout<<"Failed to read the demand informaiton from the following line: \n"<<pline<<endl;
        return 2;
    }

	//floatType total_demand = 0.0;
	while(getline(infile, pline))
    {
        if(!pline.empty())//skip an empty line
        {
			TNM_GetWordsFromLine(pline, words, '\t', '"');
			if (words[0]=="Destination")
			{
				int numoforg;				
				TNM_FromString(numoforg, words[2], std::dec);
				getline(infile, pline);
				TNM_GetWordsFromLine(pline, iwords, ',', '"');
				PTStop *stop = GetStopPtr(words[1]);
				if (!stop) 
				{
					cout<<"The stop:"<<words[1]<<" do not exist!"<<endl;
					return 3;
				}

				if((pDest = CreatePTDestination(stop->GetTransferNode()->id,numoforg)) == NULL)	//?????			
				{
					cout<<"cannot create static destination object!"<<endl;
					return 6;
				} 
//				pDest->m_tdmd = 0.0;
				if (iwords.size() == numoforg)
				{
					vector<string> dd;
					for (int i=0;i<iwords.size();++i)
					{
						TNM_GetWordsFromLine(iwords[i],dd, ':', '"');
						floatType ded;
						PTStop *dstop = GetStopPtr(dd[0]);
						TNM_FromString(ded, dd[1], std::dec);

						ded = ded;
						if (dstop)
						{
							//TNM_SNODE* node = CatchNodePtr(dstop->GetTransferNode()->id);
							if (dstop->GetTransferNode())
							{
								if (pDest->SetOrg(i+1,dstop->GetTransferNode(),ded))
								{
									pDest->m_tdmd += ded;
									numOfPTTrips += ded;
								}
								else
								{
									cout<<"cannot set org flow:"<<ded<<" for dest:"<<words[1]<<endl;
									return 4;
								}
							}
						}
						else
						{
							cout<<"The stop:"<<dd[0]<<" do not exist!"<<endl;
							return 3;						
						}		
					}
				}
				else
				{
					cout<<"Destination stop:"<<words[1]<<" has wrong number of org matching"<<endl;
					return 2;			
				}
			}
		}
	}

	cout<<"total_demand is: "<<numOfPTTrips<<endl;

	return 0;
}

int  PTNET::ReadTEAPODdemandTP()
{
	string odname=networkName + "_trip.txt";
	ifstream infile;
    if(!TNM_OpenInFile(infile, odname))
    {
        return 1;
    }
    string pline;
    vector<string> words,iwords;
	PTDestination* pDest;

    cout<<"\tReading the od demand file..."<<endl;
	if(!getline(infile, pline))//skip the first line
    {
        cout<<"Failed to read the demand informaiton from the following line: \n"<<pline<<endl;
        return 2;
    }
	while(getline(infile, pline))
    {
        if(!pline.empty())//skip an empty line
        {
			TNM_GetWordsFromLine(pline, words, '\t', '"');
			if (words[0]=="Destination")
			{
				int numoforg;				
				TNM_FromString(numoforg, words[2], std::dec);
				getline(infile, pline);
				TNM_GetWordsFromLine(pline, iwords, ',', '"');
				PTStop *stop = GetStopPtr(words[1]);
				if (!stop) 
				{
					cout<<"The stop:"<<words[1]<<" do not exist!"<<endl;
					return 3;
				}

				if((pDest = CreatePTDestination(stop->GetTransferNodeTP(1)->id,numoforg)) == NULL)	//?????			
				{
					cout<<"cannot create static destination object!"<<endl;
					return 6;
				} 
//				pDest->m_tdmd = 0.0;
				if (iwords.size() == numoforg)
				{
					vector<string> dd;
					for (int i=0;i<iwords.size();++i)
					{
						TNM_GetWordsFromLine(iwords[i],dd, ':', '"');
						floatType ded;
						PTStop *dstop = GetStopPtr(dd[0]);
						TNM_FromString(ded, dd[1], std::dec);
						if (dstop)
						{
							//TNM_SNODE* node = CatchNodePtr(dstop->GetTransferNode()->id);
							if (dstop->GetTransferNodeTP(1))
							{
								if (pDest->SetOrg(i+1,dstop->GetTransferNodeTP(0),ded))
								{
									pDest->m_tdmd += ded;
									numOfPTTrips += ded;
								}
								else
								{
									cout<<"cannot set org flow:"<<ded<<" for dest:"<<words[1]<<endl;
									return 4;
								}
							}
						}
						else
						{
							cout<<"The stop:"<<dd[0]<<" do not exist!"<<endl;
							return 3;						
						}		
					}
				}
				else
				{
					cout<<"Destination stop:"<<words[1]<<" has wrong number of org matching"<<endl;
					return 2;			
				}
			}
		}
	}
	return 0;




}

int	PTNET::BuildAN(bool stopPos, bool walkfile,bool isTP)
{
	if(ReadTEAPStops(stopPos)!=0)//读取站点信息（判断有无站点坐标）
    {
        cout<<"\tFailed to read stops"<<endl;
        return 1;
    }

	if(ReadTEAPRoutes()!=0)//读取route信息（不分上下行方向？）
	{
		cout<<"\tFailed to read routes"<<endl;
        return 2;
	}

	if(ReadTEAPShapes()!=0)//读取线路信息（有包含的站点信息）
    {
        cout<<"\tFailed to read shapes"<<endl;
        return 3;
    }

	if(isTP)
	{
		if(CreateTEAPNodeLinksTP()!=0)//在每个站点创建了两个换乘节点，用于模拟排队现象？？
		{
			cout<<"\tFailed to create transit nodes and links"<<endl;
			return 4;	
		}
	}
	else
	{
		if(CreateTEAPNodeLinks()!=0)//每个站点只创建一个换乘节点
		{
			cout<<"\tFailed to create transit nodes and links"<<endl;
			return 4;	
		}
	}
	

	
	//cout<<"numoflink:"<<numOfLink<<endl;

	if(ReadTEAPTransitData()!=0)
	{
		cout<<"\tFailed to create transit nodes and links"<<endl;
        return 5;	
	}

	if(isTP)
	{
		if(CreateTEAPWalksTP(walkfile)!=0)
		{
			cout<<"\tFailed to create walk links"<<endl;
			return 6;	
		}
	}
	else
	{
		if(CreateTEAPWalks(walkfile)!=0)
		{
			cout<<"\tFailed to create walk links"<<endl;
			return 6;	
		}
	}
	
	if(isTP)
	{
		if(ReadTEAPODdemandTP()!=0)
		{
			cout<<"\tFailed to read od demand"<<endl;
			return 7;	
		}
	}
	else
	{
		if(ReadTEAPODdemand()!=0)
		{
			cout<<"\tFailed to read od demand"<<endl;
			return 7;	
		}
	}
	
	/*if(isTP)
	{
		if(CreateTransferlink() != 0)
		{
			cout<<"\tFailed to create transfer link"<<endl;
			return 8;	
		}
	}*/
	

	ConnectAsymmetricLinks();

	//GenerateODWalkFile();//根据OD对间的cost，确定OD间walklink的费用；生成walkfile
	//ReadODWALK();//根据上面的walklink_file，创建OD对间的步行弧

	cout<<"============= node size:"<<numOfNode<<",link size:"<<numOfLink<<",od-pair size:"<<numOfPTOD<<", trips:"<<numOfPTTrips<<",numberofdestination:"<<numOfPTDest<<"============="<<endl;
	
	cout<<"Successfully build the network"<<endl;

	//PrintNetLinks();

	return 0;
}
