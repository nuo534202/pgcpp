// relnode.cpp — RelOptInfo construction and lookup.
//
// Converted from PostgreSQL 15's src/backend/optimizer/util/relnode.c.
//
// Builds RelOptInfo objects for base relations and (skeleton) join relations.
// For pgcpp's single-table workload, the join-rel machinery is minimal.
#include "optimizer/util/relnode.hpp"

#include "common/containers/node.hpp"
#include "optimizer/planner.hpp"

namespace pgcpp::optimizer {
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::RangeTblEntry;

RelOptInfo* build_simple_rel(PlannerInfo* root, int relid, RelOptInfo* parent) {
    (void)parent;  // inheritance not supported in the simplified model
    // relid is 1-based; simple_rel_array is 0-based (slot 0 holds relid 1).
    int idx = relid - 1;
    if (idx < 0 || idx >= static_cast<int>(root->simple_rel_array.size())) {
        return nullptr;
    }
    if (root->simple_rel_array[idx] != nullptr) {
        return root->simple_rel_array[idx];  // already built
    }
    // Look up the RTE from simple_rte_array (also 1-based → 0-based index).
    if (idx >= static_cast<int>(root->simple_rte_array.size())) {
        return nullptr;
    }
    RangeTblEntry* rte = root->simple_rte_array[idx];
    if (rte == nullptr) {
        return nullptr;
    }
    auto* rel = makePallocNode<RelOptInfo>();
    rel->relid = rte->relid;
    rel->relindex = relid;
    rel->rte = rte;
    root->simple_rel_array[idx] = rel;
    return rel;
}

RelOptInfo* build_join_rel(PlannerInfo* root, Relids joinrelids, RelOptInfo* outer,
                           RelOptInfo* inner, SpecialJoinInfo* sjinfo,
                           std::vector<RestrictInfo*> restrictlist) {
    (void)joinrelids;
    (void)sjinfo;
    (void)restrictlist;
    auto* joinrel = makePallocNode<RelOptInfo>();
    // Inherit rows/width from the outer side (rough estimate).
    joinrel->rows = (outer != nullptr) ? outer->rows : 1.0;
    joinrel->width = (outer != nullptr) ? outer->width : 0;
    if (inner != nullptr) {
        joinrel->width += inner->width;
    }
    add_join_rel(root, joinrel);
    return joinrel;
}

RelOptInfo* find_base_rel(PlannerInfo* root, int relid) {
    int idx = relid - 1;
    if (idx < 0 || idx >= static_cast<int>(root->simple_rel_array.size())) {
        return nullptr;
    }
    return root->simple_rel_array[idx];
}

RelOptInfo* find_join_rel(PlannerInfo* root, Relids joinrelids) {
    (void)root;
    (void)joinrelids;
    // Skeleton: join rels are not indexed. A real implementation would look up
    // root->join_rel_list by joinrelids.
    return nullptr;
}

void add_join_rel(PlannerInfo* root, RelOptInfo* joinrel) {
    root->join_rel_list.push_back(joinrel);
}

}  // namespace pgcpp::optimizer
