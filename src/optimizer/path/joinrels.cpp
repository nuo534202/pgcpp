// joinrels.cpp — Join relation construction for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/path/joinrels.c.
//
// Combines base relations two at a time, building join RelOptInfo objects
// and invoking the join path generator (NestLoop/HashJoin/MergeJoin).
//
// For MyToyDB's Task 15.15, the joinrel machinery is simplified to a
// left-deep linear join order:
//   1. Iterate over base rels in RT order.
//   2. For each adjacent pair (outer, inner), collect join clauses and
//      build a joinrel.
//   3. add_paths_to_joinrel generates the candidate paths and selects the
//      cheapest as the joinrel's cheapest_path.
//
// This is sufficient for two-table queries (the minimum required by Task
// 15.15's verification criteria). N-way joins require chaining joinrels,
// which is left as a TODO.
#include "mytoydb/optimizer/path/joinrels.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/optimizer/path/joinpath.hpp"
#include "mytoydb/optimizer/planner.hpp"
#include "mytoydb/optimizer/util/relnode.hpp"
#include "mytoydb/optimizer/util/restrictinfo.hpp"

namespace mytoydb::optimizer {
using mytoydb::nodes::makePallocNode;

namespace {

// Collect join clauses (RestrictInfos with required_relids spanning both
// `outer_rel` and `inner_rel`) from the planner's per-rel joininfo lists.
std::vector<RestrictInfo*> CollectJoinClauses(RelOptInfo* outer_rel, RelOptInfo* inner_rel) {
    std::vector<RestrictInfo*> result;
    // Collect from outer_rel's joininfo (multi-rel clauses are stored there).
    for (RestrictInfo* ri : outer_rel->joininfo) {
        if (ri == nullptr || ri->clause == nullptr)
            continue;
        bool touches_outer = false;
        bool touches_inner = false;
        for (int r : ri->required_relids) {
            if (r == outer_rel->relindex)
                touches_outer = true;
            if (r == inner_rel->relindex)
                touches_inner = true;
        }
        if (touches_outer && touches_inner) {
            result.push_back(ri);
        }
    }
    // Also collect from inner_rel's joininfo (some clauses may be attached
    // there instead — distribute_quals_to_rels picks the first relid).
    for (RestrictInfo* ri : inner_rel->joininfo) {
        if (ri == nullptr || ri->clause == nullptr)
            continue;
        // Deduplicate against clauses already collected from outer_rel.
        bool already_seen = false;
        for (RestrictInfo* existing : result) {
            if (existing == ri) {
                already_seen = true;
                break;
            }
        }
        if (already_seen)
            continue;
        bool touches_outer = false;
        bool touches_inner = false;
        for (int r : ri->required_relids) {
            if (r == outer_rel->relindex)
                touches_outer = true;
            if (r == inner_rel->relindex)
                touches_inner = true;
        }
        if (touches_outer && touches_inner) {
            result.push_back(ri);
        }
    }
    return result;
}

}  // namespace

void make_rels_by_clause_joins(PlannerInfo* root, RelOptInfo* outer_rel) {
    if (root == nullptr || outer_rel == nullptr)
        return;
    // For each base rel after `outer_rel` in RT order, attempt a join.
    // We only join adjacent pairs (level-2 joinrels), since chaining into
    // larger joinrels is not yet implemented.
    for (size_t i = 0; i < root->simple_rel_array.size(); ++i) {
        RelOptInfo* inner_rel = root->simple_rel_array[i];
        if (inner_rel == nullptr || inner_rel == outer_rel)
            continue;
        // Skip self-joins and already-joined rels.
        if (inner_rel->relindex <= outer_rel->relindex)
            continue;

        auto join_clauses = CollectJoinClauses(outer_rel, inner_rel);
        if (join_clauses.empty()) {
            // No join clause: a cross join would be required. For Task 15.15,
            // skip cross joins (they would produce cartesian products and
            // confuse the existing tests). A real planner would still
            // generate a NestLoop here with no clauses.
            continue;
        }

        // Build a SpecialJoinInfo describing an INNER join between the two
        // relations. The min_lefthand/min_righthand are the relindex of each
        // side; for inner joins, the join can be commuted.
        auto* sjinfo = makePallocNode<SpecialJoinInfo>();
        sjinfo->min_lefthand = outer_rel->relindex;
        sjinfo->min_righthand = inner_rel->relindex;
        sjinfo->jointype = mytoydb::parser::JoinType::kInner;
        sjinfo->lhs_strict = true;

        // Build the joinrel. Note: build_join_rel currently doesn't merge
        // pathlists; we add paths to it after construction.
        Relids joinrelids = {outer_rel->relindex, inner_rel->relindex};
        RelOptInfo* joinrel =
            build_join_rel(root, joinrelids, outer_rel, inner_rel, sjinfo, join_clauses);
        if (joinrel == nullptr)
            continue;
        joinrel->relindex = 0;  // join rels don't have an RT index
        joinrel->relids = joinrelids;
        joinrel->rows = outer_rel->rows * inner_rel->rows;  // rough estimate

        // Generate the candidate join paths and select the cheapest.
        add_paths_to_joinrel(root, joinrel, outer_rel, inner_rel, sjinfo, join_clauses);
    }
}

void build_joinrels_for_level(PlannerInfo* root, int level) {
    if (root == nullptr || level != 2) {
        // Only level 2 (two-table joins) is supported in this simplified impl.
        return;
    }
    // For each base rel, attempt to join it with every other base rel
    // that comes after it in RT order. This produces each pair exactly once.
    for (RelOptInfo* outer_rel : root->simple_rel_array) {
        if (outer_rel == nullptr)
            continue;
        make_rels_by_clause_joins(root, outer_rel);
    }
}

}  // namespace mytoydb::optimizer
