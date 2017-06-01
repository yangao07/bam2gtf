#ifndef _BIAS_FLOW_H_
#define _BIAS_FLOW_H_
#include "cand_iso.h"
#include "splice_graph.h"

int bias_flow_gen_cand_iso(SG *sg, double ***weight, int src, int sink, int rep_n, cmptb_map_t **iso_map, sg_para *sgp);

#endif