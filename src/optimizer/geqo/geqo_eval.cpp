// geqo_eval.cpp — GEQO chromosome fitness evaluation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/geqo/geqo_eval.c.
//
// Implements the two-phase evaluation strategy:
//   1. GeqoEvalFitness — fast heuristic cost estimate (no Path allocation).
//   2. GeqoBuildBestPath — constructs the real left-deep Path tree for the
//      winning chromosome using build_join_rel + add_paths_to_joinrel.
//
// The heuristic uses a NestLoop-style cost model: at each join step, the
// accumulated cost grows by (outer_rows * inner_scan_cost), and the output
// row count is (outer_rows * inner_rows * selectivity). Selectivity is
// kGeqoDefaultSelectivity (0.1) when a join clause connects the two sides,
// 1.0 otherwise (cross product — heavily penalized by the row explosion).
#include "pgcpp/optimizer/geqo/geqo_eval.hpp"

#include <algorithm>
#include <unordered_set>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/cost.hpp"
#include "pgcpp/optimizer/path/joinpath.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/optimizer/util/relnode.hpp"
#include "pgcpp/optimizer/util/restrictinfo.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace mytoydb::optimizer::geqo {
using mytoydb::nodes::makePallocNode;
using mytoydb::parser::JoinType;

namespace {

// Collect RestrictInfos whose required_relids touch both the accumulated
// outer relids and the new inner relindex. Generalizes CollectJoinClauses
// (in joinrels.cpp) to a multi-rel outer side.
std::vector<RestrictInfo*> CollectJoinClausesMulti(PlannerInfo* root,
                                                   const std::vector<int>& outer_relids,
                                                   int inner_relindex) {
    std::vector<RestrictInfo*> result;
    std::unordered_set<RestrictInfo*> seen;
    // Scan every base rel in the outer set and the inner rel.
    for (int relidx : outer_relids) {
        RelOptInfo* rel = find_base_rel(root, relidx);
        if (rel == nullptr)
            continue;
        for (RestrictInfo* ri : rel->joininfo) {
            if (ri == nullptr || ri->clause == nullptr)
                continue;
            if (seen.count(ri) > 0)
                continue;
            bool touches_outer = false;
            bool touches_inner = false;
            for (int r : ri->required_relids) {
                if (r == inner_relindex) {
                    touches_inner = true;
                } else {
                    for (int o : outer_relids) {
                        if (r == o) {
                            touches_outer = true;
                            break;
                        }
                    }
                }
            }
            if (touches_outer && touches_inner) {
                result.push_back(ri);
                seen.insert(ri);
            }
        }
    }
    // Also scan the inner rel's joininfo (some clauses may be attached there).
    RelOptInfo* inner_rel = find_base_rel(root, inner_relindex);
    if (inner_rel != nullptr) {
        for (RestrictInfo* ri : inner_rel->joininfo) {
            if (ri == nullptr || ri->clause == nullptr)
                continue;
            if (seen.count(ri) > 0)
                continue;
            bool touches_outer = false;
            for (int r : ri->required_relids) {
                for (int o : outer_relids) {
                    if (r == o) {
                        touches_outer = true;
                        break;
                    }
                }
            }
            if (touches_outer) {
                result.push_back(ri);
                seen.insert(ri);
            }
        }
    }
    return result;
}

// Return true if any join clause connects the outer rel set and the inner rel.
// Short-circuit version of CollectJoinClausesMulti for the heuristic fitness.
bool HasJoinClause(PlannerInfo* root, const std::vector<int>& outer_relids, int inner_relindex) {
    for (int relidx : outer_relids) {
        RelOptInfo* rel = find_base_rel(root, relidx);
        if (rel == nullptr)
            continue;
        for (RestrictInfo* ri : rel->joininfo) {
            if (ri == nullptr || ri->clause == nullptr)
                continue;
            bool touches_outer = false;
            bool touches_inner = false;
            for (int r : ri->required_relids) {
                if (r == inner_relindex) {
                    touches_inner = true;
                } else {
                    for (int o : outer_relids) {
                        if (r == o) {
                            touches_outer = true;
                            break;
                        }
                    }
                }
            }
            if (touches_outer && touches_inner)
                return true;
        }
    }
    RelOptInfo* inner_rel = find_base_rel(root, inner_relindex);
    if (inner_rel != nullptr) {
        for (RestrictInfo* ri : inner_rel->joininfo) {
            if (ri == nullptr || ri->clause == nullptr)
                continue;
            bool touches_outer = false;
            for (int r : ri->required_relids) {
                for (int o : outer_relids) {
                    if (r == o) {
                        touches_outer = true;
                        break;
                    }
                }
            }
            if (touches_outer)
                return true;
        }
    }
    return false;
}

}  // namespace

Cost GeqoEvalFitness(PlannerInfo* root, const std::vector<Gene>& chromosome) {
    if (chromosome.empty())
        return kInvalidCost;
    RelOptInfo* first = find_base_rel(root, chromosome[0]);
    if (first == nullptr)
        return kInvalidCost;
    Cost current_cost = (first->cheapest_path != nullptr) ? first->cheapest_path->total_cost : 10.0;
    Cardinality current_rows = (first->rows > 0) ? first->rows : 1.0;
    std::vector<int> current_relids = {chromosome[0]};

    for (size_t i = 1; i < chromosome.size(); ++i) {
        RelOptInfo* inner = find_base_rel(root, chromosome[i]);
        if (inner == nullptr)
            return kInvalidCost;
        Cost inner_cost =
            (inner->cheapest_path != nullptr) ? inner->cheapest_path->total_cost : 10.0;
        Cardinality inner_rows = (inner->rows > 0) ? inner->rows : 1.0;

        bool has_join = HasJoinClause(root, current_relids, chromosome[i]);
        // Selectivity: 0.1 when a join clause exists (equi-join heuristic),
        // 1.0 otherwise (cross product — penalized by row explosion).
        Selectivity sel = has_join ? kGeqoDefaultSelectivity : 1.0;
        Cardinality output_rows = current_rows * inner_rows * sel;
        // NestLoop-style cost: current + outer_rows * inner_scan_cost.
        Cost join_cost = current_cost + current_rows * inner_cost;
        current_cost = join_cost;
        current_rows = output_rows;
        current_relids.push_back(chromosome[i]);
    }
    return current_cost;
}

Path* GeqoBuildBestPath(PlannerInfo* root, const std::vector<Gene>& chromosome) {
    if (chromosome.empty())
        return nullptr;
    RelOptInfo* current = find_base_rel(root, chromosome[0]);
    if (current == nullptr || current->cheapest_path == nullptr)
        return nullptr;
    std::vector<int> current_relids = {chromosome[0]};

    for (size_t i = 1; i < chromosome.size(); ++i) {
        RelOptInfo* inner = find_base_rel(root, chromosome[i]);
        if (inner == nullptr || inner->cheapest_path == nullptr)
            return nullptr;

        auto join_clauses = CollectJoinClausesMulti(root, current_relids, chromosome[i]);

        auto* sjinfo = makePallocNode<SpecialJoinInfo>();
        sjinfo->jointype = JoinType::kInner;
        sjinfo->min_lefthand = current_relids[0];
        sjinfo->min_righthand = chromosome[i];
        sjinfo->lhs_strict = true;

        Relids joinrelids = current_relids;
        joinrelids.push_back(chromosome[i]);

        RelOptInfo* joinrel =
            build_join_rel(root, joinrelids, current, inner, sjinfo, join_clauses);
        if (joinrel == nullptr)
            return nullptr;
        joinrel->relindex = 0;
        joinrel->relids = joinrelids;

        // Estimate output rows based on whether a join clause exists.
        bool has_join = !join_clauses.empty();
        Selectivity sel = has_join ? kGeqoDefaultSelectivity : 1.0;
        joinrel->rows = current->rows * inner->rows * sel;
        joinrel->width = current->width + inner->width;

        // Generate NestLoop/HashJoin/MergeJoin candidate paths.
        add_paths_to_joinrel(root, joinrel, current, inner, sjinfo, join_clauses);
        if (joinrel->cheapest_path == nullptr)
            return nullptr;
        current = joinrel;
        current_relids = std::move(joinrelids);
    }
    return current->cheapest_path;
}

}  // namespace mytoydb::optimizer::geqo
