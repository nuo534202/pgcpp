// relnode.h — RelOptInfo construction and lookup.
//
// Converted from PostgreSQL 15's src/include/optimizer/pathnode.h (RelOptInfo
// section) and src/backend/optimizer/util/relnode.c.
//
// Builds RelOptInfo objects for base relations and (skeleton) join relations,
// and provides lookup by relid / joinrelids.
#pragma once

#include "mytoydb/optimizer/path.hpp"
#include "mytoydb/optimizer/util/restrictinfo.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in mytoydb/optimizer/planner.hpp. Forward-
// declared here to avoid a circular include (planner.hpp → path.hpp → here).
struct PlannerInfo;

// SpecialJoinInfo — simplified join-info struct (PG's SpecialJoinInfo).
// For MyToyDB's single-table workload, only a minimal skeleton is needed.
struct SpecialJoinInfo {
    int min_lefthand = 0;   // min relids on the left side (simplified)
    int min_righthand = 0;  // min relids on the right side
    mytoydb::parser::JoinType jointype = mytoydb::parser::JoinType::kInner;
    bool lhs_strict = false;  // left join condition is strict
    bool semi_rhs_exprs = false;
};

// build_simple_rel — construct a RelOptInfo for a base relation identified by
// its 1-based range table index. If one already exists in simple_rel_array,
// it is returned unchanged. The `parent` argument is for child relations in
// inheritance; for MyToyDB it is unused (kept for API compatibility).
RelOptInfo* build_simple_rel(PlannerInfo* root, int relid, RelOptInfo* parent);

// build_join_rel — construct a RelOptInfo for a join of two relations.
// Skeleton: allocates the RelOptInfo and records it in join_rel_list.
RelOptInfo* build_join_rel(PlannerInfo* root, Relids joinrelids, RelOptInfo* outer,
                           RelOptInfo* inner, SpecialJoinInfo* sjinfo,
                           std::vector<RestrictInfo*> restrictlist);

// find_base_rel — look up a base RelOptInfo by its 1-based range table index.
// Returns nullptr if not found.
RelOptInfo* find_base_rel(PlannerInfo* root, int relid);

// find_join_rel — look up a join RelOptInfo by its joinrelids.
// Skeleton: always returns nullptr (join rels not indexed in the simplified model).
RelOptInfo* find_join_rel(PlannerInfo* root, Relids joinrelids);

// add_join_rel — record a join RelOptInfo in the planner's join_rel_list.
void add_join_rel(PlannerInfo* root, RelOptInfo* joinrel);

}  // namespace mytoydb::optimizer
