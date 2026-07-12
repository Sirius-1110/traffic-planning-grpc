#include "PTNet.h"
#include "TNM_utility.h"

namespace {

constexpr int kCsvDecimalPlaces = 4;

string CsvText(const string& value)
{
	if (value.find_first_of(",\"\r\n") == string::npos)
	{
		return value;
	}

	string escaped = "\"";
	for (char ch : value)
	{
		escaped += ch == '"' ? "\"\"" : string(1, ch);
	}
	escaped += '"';
	return escaped;
}

string JoinLinkIds(const vector<GLINK*>& links)
{
	ostringstream output;
	for (size_t i = 0; i < links.size(); ++i)
	{
		if (i > 0) output << ';';
		output << links[i]->m_linkPtr->id;
	}
	return output.str();
}

string JoinLinkProbabilities(const vector<GLINK*>& links)
{
	ostringstream output;
	output << fixed << setprecision(kCsvDecimalPlaces);
	for (size_t i = 0; i < links.size(); ++i)
	{
		if (i > 0) output << ';';
		output << links[i]->m_data;
	}
	return output.str();
}

}  // namespace

void PTNET::ReportAONCsvResults(double cpuTimeSeconds)
{
	const string baseName = networkName + "_";
	const string linkFileName = baseName + "pt_link_result.csv";
	const string pathFileName = baseName + "pt_path_result.csv";
	const string iterFileName = baseName + "pt_iter_result.csv";
	const string summaryFileName = baseName + "pt_summary_result.csv";

	double assignedDemand = 0.0;
	double infeasibleFlow = 0.0;
	double totalSystemCost = 0.0;
	double totalWaitCost = 0.0;
	int numHyperpaths = 0;

	for (PTDestination* destination : PTDestVector)
	{
		for (int originIndex = 0;
			 originIndex < destination->numOfOrg;
			 ++originIndex)
		{
			PTOrg* origin = destination->orgVector[originIndex];
			if (!origin->state || origin->pathSet.empty())
			{
				infeasibleFlow += origin->assDemand;
				continue;
			}

			for (TNM_HyperPath* path : origin->pathSet)
			{
				++numHyperpaths;
				assignedDemand += path->flow;
				totalSystemCost += path->flow * path->cost;
				totalWaitCost += path->flow * path->WaitCost;
			}
		}
	}

	{
		ofstream output;
		if (!TNM_OpenOutFile(output, linkFileName)) return;
		output << fixed << setprecision(kCsvDecimalPlaces);
		output
			<< "link_id,link_type,tail_node_id,head_node_id,"
			<< "tail_stop_id,head_stop_id,route_id,shape_id,"
			<< "seq,flow,cost\n";

		for (PTLink* link : linkVector)
		{
			const bool belongsToShape =
				link->GetTransitLinkType() != PTLink::WALK &&
				link->GetTransitLinkType() != PTLink::TRANSFER &&
				link->GetTransitLinkType() != PTLink::FAILWALK &&
				link->m_shape != NULL;
			const string routeId =
				belongsToShape && link->m_shape->m_routePtr
					? link->m_shape->m_routePtr->m_id
					: "";
			const string shapeId =
				belongsToShape ? link->m_shape->m_id : "";
			const string tailStopId =
				link->tail && link->tail->m_stop
					? link->tail->m_stop->m_id
					: "";
			const string headStopId =
				link->head && link->head->m_stop
					? link->head->m_stop->m_id
					: "";

			output
				<< link->id << ','
				<< link->GetTransitLinkTypeName() << ','
				<< link->tail->id << ','
				<< link->head->id << ','
				<< CsvText(tailStopId) << ','
				<< CsvText(headStopId) << ','
				<< CsvText(routeId) << ','
				<< CsvText(shapeId) << ',';
			if (belongsToShape)
			{
				output << link->seq + 1;
			}
			output << ',' << link->volume << ',' << link->cost << '\n';
		}
	}

	{
		ofstream output;
		if (!TNM_OpenOutFile(output, pathFileName)) return;
		output << fixed << setprecision(kCsvDecimalPlaces);
		output
			<< "origin_node_id,destination_node_id,origin_stop_id,"
			<< "destination_stop_id,path_index,path_flow,path_cost,"
			<< "wait_cost,num_links,link_sequence,link_probability\n";

		for (PTDestination* destination : PTDestVector)
		{
			for (int originIndex = 0;
				 originIndex < destination->numOfOrg;
				 ++originIndex)
			{
				PTOrg* origin = destination->orgVector[originIndex];
				for (size_t pathIndex = 0;
					 pathIndex < origin->pathSet.size();
					 ++pathIndex)
				{
					TNM_HyperPath* path = origin->pathSet[pathIndex];
					const vector<GLINK*> links = path->GetGlinks();
					output
						<< origin->org->id << ','
						<< destination->destination->id << ','
						<< CsvText(origin->org->m_stop->m_id) << ','
						<< CsvText(destination->destination->m_stop->m_id)
						<< ',' << pathIndex + 1 << ','
						<< path->flow << ','
						<< path->cost << ','
						<< path->WaitCost << ','
						<< links.size() << ','
						<< CsvText(JoinLinkIds(links)) << ','
						<< CsvText(JoinLinkProbabilities(links)) << '\n';
				}
			}
		}
	}

	{
		ofstream output;
		if (!TNM_OpenOutFile(output, iterFileName)) return;
		output << fixed << setprecision(kCsvDecimalPlaces);
		output << "cpu_time,num_hyperpaths,infeasible_flow\n";
		output
			<< cpuTimeSeconds << ','
			<< numHyperpaths << ','
			<< infeasibleFlow << '\n';
	}

	int walkLinks = 0;
	int positiveFlowLinks = 0;
	double totalPassengerLinkFlow = 0.0;
	for (PTLink* link : linkVector)
	{
		if (link->GetTransitLinkType() == PTLink::WALK)
		{
			++walkLinks;
		}
		if (link->volume > flowPrecision)
		{
			++positiveFlowLinks;
		}
		totalPassengerLinkFlow += link->volume;
	}

	{
		ofstream output;
		if (!TNM_OpenOutFile(output, summaryFileName)) return;
		output << fixed << setprecision(kCsvDecimalPlaces);
		output << "metric_key,metric_value\n";
		output << "num_of_nodes," << numOfNode << '\n';
		output << "num_of_links," << numOfLink << '\n';
		output << "num_of_stops," << m_stops.size() << '\n';
		output << "num_of_routes," << m_routes.size() << '\n';
		output << "num_of_shapes," << m_shapes.size() << '\n';
		output << "num_of_walk_links," << walkLinks << '\n';
		output << "num_of_od_pairs," << numOfPTOD << '\n';
		output << "num_of_destinations," << numOfPTDest << '\n';
		output << "num_of_hyperpaths," << numHyperpaths << '\n';
		output << "total_demand," << numOfPTTrips << '\n';
		output << "assigned_demand," << assignedDemand << '\n';
		output << "infeasible_flow," << infeasibleFlow << '\n';
		output << "positive_flow_links," << positiveFlowLinks << '\n';
		output
			<< "total_passenger_link_flow,"
			<< totalPassengerLinkFlow << '\n';
		output << "total_system_cost," << totalSystemCost << '\n';
		output
			<< "avg_path_cost,"
			<< (assignedDemand > 0.0
					? totalSystemCost / assignedDemand
					: 0.0)
			<< '\n';
		output << "total_wait_cost," << totalWaitCost << '\n';
		output
			<< "avg_wait_cost,"
			<< (assignedDemand > 0.0
					? totalWaitCost / assignedDemand
					: 0.0)
			<< '\n';
		output << "cpu_time," << cpuTimeSeconds << '\n';
	}

	cout << "\tWriting AON CSV results:\n"
		 << "\t  " << linkFileName << '\n'
		 << "\t  " << pathFileName << '\n'
		 << "\t  " << iterFileName << '\n'
		 << "\t  " << summaryFileName << endl;
}

void PTNET::ReportIter()
{
	string IterConvName = networkName + "-" + GetAlgorithmName() + "-conv.csv";
	ofstream outfile;

	if (!TNM_OpenOutFile(outfile, IterConvName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .conv file to write Pas Origin Information!"<<endl;
		return;
	}
	cout<<"\tWriting iterative information into "<<IterConvName<<" for covergence info!"<<endl;

	outfile<<"Iteration,Gap,RelativeGap,Time,Mainlooptime,Innerlooptime,inneriters,numofhyperpath,numofiarcs"<<endl;

	if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff)
	{
		for(int i = 0; i < iterRecord.size(); i++)
		{
			PTITERELEM* it = iterRecord[i];
			outfile<<TNM_IntFormat(it->iter)<<","<<TNM_FloatFormat(it->convGap,20,18)<<","<<TNM_FloatFormat(it->convRGap,20,18)<<","<<TNM_FloatFormat(it->time,6,3)<<","<<"-"<<","<<"-"<<","<<"-"<<","<<"-"<<endl;
		}
	}
	else
	{
		for(int i = 0; i < iterRecord.size(); i++)
		{
			PTITERELEM* it = iterRecord[i];
			outfile<<TNM_IntFormat(it->iter)<<","<<TNM_FloatFormat(it->convGap,20,18)<<","<<TNM_FloatFormat(it->convRGap,20,18)<<","<<TNM_FloatFormat(it->time,6,3)<<","<<TNM_FloatFormat(it->mainlooptime,6,3)<<","<<TNM_FloatFormat(it->innerlooptime,6,3)<<","<<TNM_IntFormat(it->innerIters)<<","<<TNM_IntFormat(it->numberofhyperpaths)<<","<<TNM_IntFormat(it->numof_infeasible_arcs)<<endl;
		}
	}

	outfile.close();
}

void PTNET::ReportPTlinkflow()
{
	string fileName  = networkName +"-"+ GetAlgorithmName() + "-linkinfo.csv";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, fileName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .linkinfo file to write Pas Origin Information!"<<endl;
		return;
	}
	cout<<"\tWriting PT link information into "<<fileName<<" linkinfo!"<<endl;

	PTLink *link;
	outfile<<"link id,from_stop, to_stop, link type,link flow,link cost,"<<endl;
	for(int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		outfile<<link->id<<","<< link->tail->m_stop->m_id << "," << link->head->m_stop->m_id << "," <<link->GetTransitLinkTypeName() << "," << link->volume <<","<<link->cost<<endl;
		//outfile<<TNM_IntFormat(link->id)<<","<<TNM_FloatFormat(link->volume,6,3)<<","<<link->cost<<","<<link->pfdcost<<",";
		//if (link->GetTransitLinkType()==PTLink::ABOARD)
		//{
		//	double cap = link->pars[link->pars.size() - 1] ;
		//	outfile<<cap<<",";

		//}
		//else if ( link->GetTransitLinkType()==PTLink::ENROUTE)
		//{
		//	double cap = link->pars[link->pars.size() - 2] ;
		//	outfile<<cap<<",";

		//}
		//else
		//{
		//	outfile<<"/"<<",";
		//	outfile<<"0"<<endl;
		//}

	}

	//if(PCTAE_ALG == PCTAE_algorithm::PCTAE_A_MSA_eff )
	//{
	//	outfile<<"netTTwaitcost:"<<max_w<<endl;
	//}
	//else
	//{
	//	outfile<<"netTTwaitcost:"<<netTTwaitcost<<endl;
	//}
	
	
	outfile.close();
}

void PTNET::ReportLinkCap()
{
	string fileName  = networkName +"-" + GetAlgorithmName() + ".cap";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, fileName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .cap file to write  Information!"<<endl;
		return;
	}
	cout<<"\tWriting link capacity information into "<<fileName<<" linkinfo!"<<endl;

	PTLink *link;
	outfile<<"id,flow,capacity"<<endl;
	floatType avg = 0.0, n = 0.0;
	floatType positive_avg = 0.0, positive_n = 0.0;
	for(int i = 0;i<numOfLink;i++)
	{
		link = linkVector[i];
		switch(link->GetTransitLinkType())
        {
			case PTLink::ABOARD:
				double alpha_2,beta_2,n1,cap1;
				alpha_2	=	link->pars[0];
				beta_2	=	link->pars[1];
				n1		=	link->pars[2];//power
				cap1		=	link->pars[3];		
				outfile<<link->id<<","<<(1 - beta_2) * link->volume +  beta_2 *link->rLink->volume<<","<<cap1<<endl;
				avg += ((1 - beta_2) * link->volume +  beta_2 *link->rLink->volume)/cap1;
				n++;
				if (link->volume>0||link->rLink->volume>0)
				{
					positive_avg += ((1 - beta_2) * link->volume +  beta_2 *link->rLink->volume)/cap1;
					positive_n++;
				}
				
				break;

			case PTLink::ENROUTE:
				double alpha_3,beta_3,gamma_3,n2,cap2,fft;
				alpha_3	=	link->pars[0];
				beta_3	=	link->pars[1];
				gamma_3	=	link->pars[2];
				n2		=	link->pars[3];//power
				cap2	=	link->pars[4];
				fft		=	link->pars[5];
				outfile<<link->id<<","<< link->volume +  (gamma_3 - 1) *link->rLink->volume<<","<<cap2<<endl;
				avg += (link->volume +  (gamma_3 - 1) * link->rLink->volume)/cap2;
				n++;
				if (link->volume>0||link->rLink->volume>0)
				{
					positive_avg += (link->volume +  (gamma_3 - 1) * link->rLink->volume)/cap2;
					positive_n++;
				}
				break;
        }
	}
	outfile<<"Avg: V/C: "<<avg / n<<", Pos Avg:"<<positive_avg/positive_n<<endl;
	outfile.close();
}

void PTNET::ReportPTHyperpaths()
{
	//cout << "1" << endl;
	string PathinfoName  = networkName +"-" + GetAlgorithmName() + ".pathinfo";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, PathinfoName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .pathinfo file to write hyperpath Information!"<<endl;
		return;
	}
	cout<<"\tWriting net hyperpath information into "<<PathinfoName<<" for covergence info!"<<endl;
	outfile<<"Destination,origin,numberofpaths,(link sequence,path flow,path cost)"<<endl<<endl;

	int numberhp=0;
	int numofboardings=0;
	int numofboardinglines = 0;
	for (int i = 0;i<numOfPTDest;i++)
	{
		PTDestination* dest = PTDestVector[i];
		double sumRGap = 0.0;

		for (int j=0; j<dest->numOfOrg; j++)
		{
			PTOrg* org= dest->orgVector[j];
			//int n1 =0, n2 =0;
			//for (int k=0; k<org->pathSet.size();++k) 
			//{
			//	n1 += org->pathSet[k]->Boardings;
			//	n2 += org->pathSet[k]->TotalBoardlines;
			//}
			outfile<<"OD:"<<org->org->id<<","<<dest->destination->id<<":"<<org->pathSet.size()<<endl;
			//outfile<<"OD:"<<TNM_IntFormat(org->org->id)<<","<<TNM_IntFormat(dest->destination->id)<<", path size"<<TNM_IntFormat(org->pathSet.size())<<endl;
			numberhp += org->pathSet.size();
			for (int k=0; k<org->pathSet.size();++k)
			{
				TNM_HyperPath* path = org->pathSet[k];
				vector<GLINK*> glinks=path->GetGlinks();

				for(int ix=0;ix<glinks.size();++ix)
				{
					PTLink* plink=glinks[ix]->m_linkPtr;
					if (ix<glinks.size()-1) outfile<<plink->id<<",";
					else outfile<<plink->id;
					//outfile<<TNM_IntFormat(plink->id)<<",";
				}
				outfile << ":	path flow:" << path->flow << ",path ttcost:" << path->cost << ",path waitcost:" << path->WaitCost << endl;
				//outfile<<":	path flow:"<<path->flow<<",path ttcost:"<<path->cost<<",path waitcost:"<<path->WaitCost<<",path transfer times:"<<org->org->transfer<<",path walktime:"<<org->org->walkt<<endl;
				//outfile<<TNM_FloatFormat(path->flow,12,6)<<TNM_FloatFormat(path->cost,12,6)<<endl;
			}
			outfile<<endl;
		}
	}
	outfile<<"Number of O-Dpairs:"<<numOfPTOD<<",Total number of hyperpath:"<<numberhp<<endl;
	outfile.close();
}

void PTNET::ReportPTHyperpathsTP()
{
	string PathinfoName  = networkName +"-" + GetAlgorithmName() + ".pathinfo";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, PathinfoName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .pathinfo file to write hyperpath Information!"<<endl;
		return;
	}
	cout<<"\tWriting net hyperpath information into "<<PathinfoName<<" for covergence info!"<<endl;
	outfile<<"Destination,origin,numberofpaths,(link sequence,path flow,path cost)"<<endl<<endl;

	int numberhp=0;
	int numofboardings=0;
	int numofboardinglines = 0;
	for (int i = 0;i<numOfPTDest;i++)
	{
		PTDestination* dest = PTDestVector[i];
		double sumRGap = 0.0;

		for (int j=0; j<dest->numOfOrg; j++)
		{
			PTOrg* org= dest->orgVector[j];
			//int n1 =0, n2 =0;
			//for (int k=0; k<org->pathSet.size();++k) 
			//{
			//	n1 += org->pathSet[k]->Boardings;
			//	n2 += org->pathSet[k]->TotalBoardlines;
			//}
			outfile<<"OD:"<<org->org->id<<","<<dest->destination->id<<":"<<org->pathSetTP.size()<<endl;
			//outfile<<"OD:"<<TNM_IntFormat(org->org->id)<<","<<TNM_IntFormat(dest->destination->id)<<", path size"<<TNM_IntFormat(org->pathSet.size())<<endl;
			numberhp += org->pathSetTP.size();
			for (int k=0; k<org->pathSetTP.size();++k)
			{
				TNM_HyperPath* path = org->pathSetTP[k];
				vector<GLINK*> glinks=path->GetGlinks();

				for(int ix=0;ix<glinks.size();++ix)
				{
					PTLink* plink=glinks[ix]->m_linkPtr;
					if (ix<glinks.size()-1) outfile<<plink->id<<",";
					else outfile<<plink->id;
					//outfile<<TNM_IntFormat(plink->id)<<",";
				}
				outfile<<":	path flow:"<<path->flow<<",path ttcost:"<<path->cost<<",path waitcost:"<<path->WaitCost<<",path transfer times:"<<org->org->transfer<<",path walktime:"<<org->org->walkt<<endl;
				//outfile<<TNM_FloatFormat(path->flow,12,6)<<TNM_FloatFormat(path->cost,12,6)<<endl;
			}
			outfile<<endl;
		}
	}
	outfile<<"Number of O-Dpairs:"<<numOfPTOD<<",Total number of hyperpath:"<<numberhp<<endl;
	outfile.close();




}

void PTNET::ReportPTStrategies()
{
	string StginfoName  = networkName +"-" + GetAlgorithmName() + ".stginfo";
	ofstream outfile;
	if (!TNM_OpenOutFile(outfile, StginfoName))
	{
		cout<<"\n\tFail to prepare report: Cannot open .stginfo file to write hyperpath Information!"<<endl;
		return;
	}
	vector<GNODE*>::iterator pv;
	cout<<"\tWriting net stg information into "<<StginfoName<<" for covergence info!"<<endl;
	outfile<<"Destination,numberofStgNodes,(node id: stg name, stg flow, stg wait)"<<endl<<endl;
	for (int i = 0;i<numOfPTDest;i++)
	{		
		PTDestination* dest = PTDestVector[i];

		clock_t t1 = clock();
		dest->MarkStgLinksOnNet();
		
		GNODE* gnode;
		outfile<<"Dest:"<<dest->destination->id<<","<<dest->tplNodeVec.size()<<endl;
		for (pv = dest->tplNodeVec.begin(); pv!=dest->tplNodeVec.end(); pv++)
		{
			gnode = (*pv);
			PTNode* tailnode = gnode->m_ptnodePtr;
			outfile<<tailnode->id<<":";
			for (vector<StgLinks*>::iterator pt = gnode->m_StgsVec.begin();pt != gnode->m_StgsVec.end();pt++)
			{
				StgLinks* stg = *pt;
				//outfile<<stg->sname<<"("<<TNM_FloatFormat(stg->sflow,12,6)<<","<<TNM_FloatFormat(stg->waitT,12,6)<<"),";
				if (pt == gnode->m_StgsVec.end()-1)
					outfile<<stg->sname<<"("<<stg->sflow<<","<<stg->waitT<<")";
				else outfile<<stg->sname<<"("<<stg->sflow<<","<<stg->waitT<<"),";
			}
			outfile<<endl;

		}
		outfile<<endl;
		dest->RemarkStgLinksOnNet();	
	}
	outfile.close();
}
