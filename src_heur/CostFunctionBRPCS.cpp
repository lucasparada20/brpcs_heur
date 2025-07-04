#include "CostFunctionBRPCS.h"
#include "RouteFeasibilityBRPCS.h"
#include "Solution.h"

double CostFunctionBRPCS::GetCost(Sol & s)
{
	double cost = 0;
	for(int i = 0 ; i < s.GetDriverCount() ; i++)
		cost += GetCost(s, s.GetDriver(i));
	return cost + s.GetUnassignedCount()*UNASSIGNED_COST;
}

double CostFunctionBRPCS::GetCost(Sol & s,Driver * d)
{
	double d1 = 0;
	int Q = d->capacity;
	int init_q = 0; int init_qe = 0;
	
	path.clear(); path.reserve( s.RoutesLength[d->id] + 2);
	
	Node * prev = s.GetNode( d->StartNodeID );
	path.push_back(prev);
	bool has_uncharged = false;
	while(prev->type != NODE_TYPE_END_DEPOT)
	{
		Node * next = s.Next[ prev->id ];
		d1 += s.GetDist(prev,next);
		path.push_back(next);
		
		if(next->h_u_i0 > 0) has_uncharged = true;
		if(next->type == NODE_TYPE_CUSTOMER) 
		{
			path_ids.emplace_back( next->id );
			init_q += next->q; init_qe += next->q_e;
		}	
		
		prev = next;
	}
	
	if(d1 > Parameters::MaxRouteDistance()) // Tighten this to have ~ 20 routes
		return INF_ROUTE_COST;
	
	double cost = INF_ROUTE_COST;
	double rec; 	
	bool has_zero_rec = true;
	if(init_q > Q || init_q < -Q || init_qe > Q || init_qe < -Q) has_zero_rec = false;
	if(init_q+init_qe > Q || init_q+init_qe < -Q) has_zero_rec = false; 
	
	if(!Parameters::GetCostPolicy()) // Continue-to-next
	{
		if(!RouteFeasibility::EndLoadHybrid(path,Q,false))
			return INF_ROUTE_COST;	
		 
		if(has_zero_rec) 
			has_zero_rec = RouteFeasibility::HasZeroHCUnchargedViolations(path, Q, false, init_q, init_qe);
		
		//has_zero_rec = RouteFeasibility::HasZeroHCUnchargedViolations(path, Q, false);
		//has_zero_rec = RouteFeasibility::HasZeroHCUncharged(path, Q, false); // Slightly less general
				
		rec = has_zero_rec ? 0 : _r->CalculateContinueToNextMIP(path,Q,1);//Last parameter is the integer delta
		
	} else { // Restock
		
		if(has_zero_rec)
			has_zero_rec = RouteFeasibility::HasZeroHCUncharged(path, Q, false, init_q, init_qe);
		//has_zero_rec = RouteFeasibility::HasZeroHCUncharged(path, Q, false);
		
		rec = has_zero_rec ? 0 :_r->CalculateRestockingTrips(path,Q,false);
	}
		
	if(rec < 9999.0)
		cost = d1 + rec;	
	
	return cost;
}


void CostFunctionBRPCS::Update(Sol & s)
{
	double sumd = 0; double sumr = 0; //int drvs = 0; 
	for(int i = 0 ; i < s.GetDriverCount() ; i++)
	{
		Driver * d = s.GetDriver(i);
		Update(s, d);
		sumd += d->curDistance;
		sumr += d->curRecourse;
		
		//if(s.RoutesLength[d->id] > 0) drvs++;
	}
	s.SetTotalDistances(sumd);
	s.SetTotalRecourse(sumr);
	//printf("Update Drvs:%d Cost:%.2lf Rec:%.2lf\n",drvs,sumd,sumr);
}

void CostFunctionBRPCS::Update(Sol & s, Driver * d)
{
    double d1 = 0;
    d->ResetFeasibilityQuantities();
	d->is_feasible = true;
	int Q = d->capacity;
	
    path.clear(); path.reserve( s.RoutesLength[d->id] + 2);

    Node * prev = s.GetNode( d->StartNodeID );
    path.push_back(prev);

    int lambda = 0;
    int min_lambda = 0;
    int mu = 0;
    int max_mu = 0;
	
	int accumulated_uncharged = 0;

    // Loop forward until the end depot is reached
    while(prev->type != NODE_TYPE_END_DEPOT)
    {
        Node * next = s.Next[ prev->id ];
        path.push_back(next);
        d1 += s.GetDist(prev, next);
        prev = next;

        if(prev->type != NODE_TYPE_CUSTOMER)
            continue;
		
		if(next->h_u_i0 > 0)
		{
			d->has_uncharged_bikes = true;
			accumulated_uncharged += next->h_u_i0;
			next->has_accumulated_uncharged = accumulated_uncharged;
		} else next->has_accumulated_uncharged = 0;			
        
        d->sum_q      += prev->q;
        d->sum_q_e    += prev->q_e;
        d->sum_h_i0   += prev->h_i0;
        d->sum_h_e_i0 += prev->h_e_i0;
        d->sum_W      += prev->maxWm;
        
        int combined_demand = prev->q + prev->q_e;
        int slack    = combined_demand - (prev->h_i0 + prev->h_e_i0);
        int surplus  = combined_demand + prev->maxWm;
	
        lambda += slack;
        min_lambda = std::min(lambda, min_lambda);
        mu   += surplus;
        max_mu = std::max(mu, max_mu);
		
		prev->ResetFeasibilityQuantities();
		prev->lambda = lambda; prev->min_lambda = min_lambda;
		prev->mu = mu; prev->max_mu = max_mu;

        int lb = lambda - min_lambda;
        int ub = mu + Q - max_mu;

        if(lb > ub)
		{
			prev->is_end_feasible = false;
			if( s.RoutesLength[d->id] > 0 ) 
				d->is_feasible = false;
		}
        else
            prev->is_end_feasible = true;
                
        //{
        //    printf("Update: Node %d, lb=%d, ub=%d, demand=%d, slack=%d, surplus=%d, lambda=%d, mu=%d, min_lambda=%d, max_mu=%d, infeasible=%d\n",
        //           prev->id, lb_hybrid, ub_hybrid, combined_demand, slack_hybrid, surplus_hybrid,
        //           lambda_hybrid, mu_hybrid, min_lambda_hybrid, max_mu_hybrid, prev->IsEndInfeasible);
        //}
    }
    
    d->curDistance = d1;

	bool has_zero_rec = true;
	bool is_continue = !Parameters::GetCostPolicy();
	bool is_feasible = d->is_feasible;

	bool within_capacity = 
		(d->sum_q <= Q && d->sum_q >= -Q) &&
		(d->sum_q_e <= Q && d->sum_q_e >= -Q) &&
		(d->sum_q + d->sum_q_e <= Q) &&
		(d->sum_q + d->sum_q_e >= -Q);

	if (is_continue) {
		if (!is_feasible || !within_capacity)
			has_zero_rec = false;
		
		if (has_zero_rec) 
			has_zero_rec = RouteFeasibility::HasZeroHCUnchargedViolations(path, Q, false, d->sum_q, d->sum_q_e);
		
		d->curRecourse = is_feasible
			? (has_zero_rec ? 0 : _r->CalculateContinueToNextMIP(path, Q, 1))
			: INF_ROUTE_COST;
	} else {
		if (!within_capacity)
			has_zero_rec = false;
		
		if (has_zero_rec)
			has_zero_rec = RouteFeasibility::HasZeroHCUncharged(path, Q, false, d->sum_q, d->sum_q_e);
	
		d->curRecourse = has_zero_rec ? 0 : _r->CalculateRestockingTrips(path, Q, 1);
	}
	
	if (path[path.size()-1]->type == NODE_TYPE_CUSTOMER)
	{
		printf("Update : Didn't add the end depot. Exiting\n");
		for(size_t i = 0; i<path.size();i++)
			path[i]->Show();
		exit(1);
	}
	
	int gamma = 0;
    int min_gamma = 0;
    int max_zeta = 0;
    int zeta = 0;

    // Loop backwards from the last node down to the first
    for (size_t i = path.size() - 1; i > 0; i--)
    {
        Node* n = path[i];
        if (n->type != NODE_TYPE_CUSTOMER)
            continue; // Skip depots

        int combined_demand = n->q + n->q_e;
        int gamma_i = std::min(Q, combined_demand + n->maxWm);
        int zeta_i = std::max(-Q, combined_demand - (n->h_i0 + n->h_e_i0));

        gamma -= gamma_i;
        min_gamma = std::min(gamma, min_gamma);
        zeta  -= zeta_i;
        max_zeta = std::max(zeta, max_zeta);
		
		n->gamma = gamma; n->min_gamma = min_gamma;
		n->zeta = zeta; n->max_zeta = max_zeta;

        int lb = gamma - min_gamma;
        int ub = zeta + Q - max_zeta;

        //if (show)
        //{
        //    printf("Update-StartLoadHybrid Lb>Ub?%d no:%d lb:%d ub:%d dmd:%d wp:%d wm:%d gamma:%d zeta:%d sum_gamma:%d sum_zeta:%d min_gamma:%d max_zeta:%d\n",
        //           lb > ub, n->no, lb, ub, combined_demand, n->h_i0 + n->h_e_i0, n->maxWm,
        //           gamma_i, zeta_i, sum_gamma, sum_zeta, min_gamma, max_zeta);
        //}

        if(lb > ub)
            n->is_start_feasible = false;
        else
            n->is_start_feasible = true;
    }	
}


void CostFunctionBRPCS::Show(Sol * s, Driver * d)
{
	double cost = GetCost(*s,d);
	std::vector<Node*> path;
	Node * cur = s->GetNode( d->StartNodeID );
	double d1 = 0;
	while(cur != NULL)
	{
		path.push_back(cur);
		Node * next = s->GetNext(cur);
		if(next != NULL)
			d1 += s->GetDist(cur,next);
		cur = next;
	}
	
	int Q = d->capacity;
	
	double policy_cost = !Parameters::GetCostPolicy() ? 
				_r->CalculateContinueToNextMIP(path,Q,1)
				: _r->CalculateRestockingTrips(path,Q,1);

	printf("Route:%d cost:%.1lf policy_cost:%.1lf dist:%.2lf len:%d: ", 
           d->id, cost, policy_cost, d1, s->RoutesLength[d->id]);
	
	cur = s->GetNode( d->StartNodeID );
	while(cur != NULL)
	{
		printf("%d-", cur->no);
		
		//Node * next = s->GetNext(cur);
		//if(next != NULL)
		//	printf("dist(%.1lf)-",s->GetDist(cur,next));
		
		cur = s->Next[ cur->id ];
			
	}
	printf("\n");
}
