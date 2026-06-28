// pathnode.cpp — Path construction factory functions.
//
// Converted from PostgreSQL 15's src/backend/optimizer/util/pathnode.c.
//
// Allocates and initializes Path subclass objects. Each factory sets the
// PathType, links the parent RelOptInfo, and fills in default cost/width
// from the relation. Cost estimation is left to the caller (or done inline
// for SeqScan, matching the existing allpaths.cpp pattern).
#include "pgcpp/optimizer/util/pathnode.hpp"

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/optimizer/cost.hpp"

namespace pgcpp::optimizer {
using pgcpp::nodes::makePallocNode;

SeqScanPath* create_seqscan_path(PlannerInfo* root, RelOptInfo* rel) {
    (void)root;
    auto* path = makePallocNode<SeqScanPath>();
    path->parent_rel = rel;
    path->relid = rel->relid;
    path->rows = (rel->rows > 0) ? rel->rows : 1.0;
    path->width = rel->width;
    // Default cost: pages * seq_page_cost + tuples * cpu_tuple_cost.
    int pages = (rel->pages > 0) ? rel->pages : 10;
    int tuples = (rel->tuples > 0) ? rel->tuples : 1000;
    CostSeqScan(path, pages, tuples);
    return path;
}

IndexPath* create_index_path(PlannerInfo* root, RelOptInfo* rel, pgcpp::catalog::Oid indexid,
                             std::vector<pgcpp::parser::Node*> indexclauses) {
    (void)root;
    auto* path = makePallocNode<IndexPath>();
    path->parent_rel = rel;
    path->relid = rel->relid;
    path->indexid = indexid;
    path->indexqual = std::move(indexclauses);
    path->rows = (rel->rows > 0) ? rel->rows : 1.0;
    path->width = rel->width;
    return path;
}

NestLoopPath* create_nestloop_path(PlannerInfo* root, RelOptInfo* joinrel, Path* outer, Path* inner,
                                   std::vector<RestrictInfo*> restrictlist) {
    (void)root;
    auto* path = makePallocNode<NestLoopPath>();
    path->parent_rel = joinrel;
    path->outer = outer;
    path->inner = inner;
    path->restrictlist = std::move(restrictlist);
    // Cost: startup = outer.startup; total = outer.total + outer.rows * inner.total.
    path->startup_cost = (outer != nullptr) ? outer->startup_cost : 0.0;
    Cardinality outer_rows = (outer != nullptr) ? outer->rows : 1.0;
    Cost inner_total = (inner != nullptr) ? inner->total_cost : 0.0;
    path->total_cost = path->startup_cost + outer_rows * inner_total;
    path->rows = outer_rows * ((inner != nullptr) ? inner->rows : 1.0);
    return path;
}

HashJoinPath* create_hashjoin_path(PlannerInfo* root, RelOptInfo* joinrel, Path* outer, Path* inner,
                                   std::vector<pgcpp::parser::Node*> hashclauses) {
    (void)root;
    auto* path = makePallocNode<HashJoinPath>();
    path->parent_rel = joinrel;
    path->outer = outer;
    path->inner = inner;
    path->hashclauses = std::move(hashclauses);
    // Cost: startup = outer.startup + inner.total (build hash); total = + outer rows * cpu.
    Cost outer_startup = (outer != nullptr) ? outer->startup_cost : 0.0;
    Cost inner_total = (inner != nullptr) ? inner->total_cost : 0.0;
    path->startup_cost = outer_startup + inner_total;
    Cardinality outer_rows = (outer != nullptr) ? outer->rows : 1.0;
    path->total_cost = path->startup_cost + outer_rows * kCpuTupleCost;
    path->rows = outer_rows;
    return path;
}

MergeJoinPath* create_mergejoin_path(PlannerInfo* root, RelOptInfo* joinrel, Path* outer,
                                     Path* inner, std::vector<pgcpp::parser::Node*> mergeclauses,
                                     pgcpp::parser::JoinType jointype) {
    (void)root;
    auto* path = makePallocNode<MergeJoinPath>();
    path->parent_rel = joinrel;
    path->outer = outer;
    path->inner = inner;
    path->mergeclauses = std::move(mergeclauses);
    path->jointype = jointype;
    // Cost: startup = outer.startup + inner.startup (both must be sorted, so
    // startup is the max of the two subpaths' total costs before the first
    // output row can be produced — for sorted inputs this is the sort's
    // total cost). Total = outer.total + inner.total + (outer.rows + inner.rows) * cpu.
    Cost outer_total = (outer != nullptr) ? outer->total_cost : 0.0;
    Cost inner_total = (inner != nullptr) ? inner->total_cost : 0.0;
    path->startup_cost = outer_total + inner_total;
    Cardinality outer_rows = (outer != nullptr) ? outer->rows : 1.0;
    Cardinality inner_rows = (inner != nullptr) ? inner->rows : 1.0;
    // Merge cost: linear in outer + inner, plus a small per-tuple CPU cost.
    path->total_cost = path->startup_cost + (outer_rows + inner_rows) * kCpuTupleCost;
    // Output rows: heuristically, the join produces min(outer, inner) * selec.
    // For an equi-join with no selectivity info, use 1/10 of the cross product.
    path->rows = outer_rows * inner_rows * 0.1;
    return path;
}

SubqueryScanPath* create_subqueryscan_path(PlannerInfo* root, RelOptInfo* rel, Path* subpath,
                                           int scanrelid,
                                           std::vector<pgcpp::parser::TargetEntry*> tlist) {
    (void)root;
    auto* path = makePallocNode<SubqueryScanPath>();
    path->parent_rel = rel;
    path->subpath = subpath;
    path->scanrelid = scanrelid;
    path->tlist = std::move(tlist);
    if (subpath != nullptr) {
        path->rows = subpath->rows;
        path->width = subpath->width;
        path->startup_cost = subpath->startup_cost;
        // SubqueryScan adds a small per-tuple overhead for the projection.
        path->total_cost = subpath->total_cost + subpath->rows * kCpuTupleCost;
    }
    return path;
}

SortPath* create_sort_path(PlannerInfo* root, RelOptInfo* rel, Path* subpath,
                           std::vector<pgcpp::parser::SortGroupClause*> pathkeys) {
    (void)root;
    auto* path = makePallocNode<SortPath>();
    path->parent_rel = rel;
    path->subpath = subpath;
    path->pathkeys = std::move(pathkeys);
    if (subpath != nullptr) {
        path->rows = subpath->rows;
        path->width = subpath->width;
        path->startup_cost = subpath->total_cost;  // must read all input before first output
        Cost sort_cost = CostSort(static_cast<int>(subpath->rows), subpath->width, -1);
        path->total_cost = path->startup_cost + sort_cost;
    }
    return path;
}

AggPath* create_agg_path(PlannerInfo* root, RelOptInfo* rel, Path* subpath,
                         pgcpp::executor::Agg::Strategy aggstrategy,
                         std::vector<pgcpp::parser::Node*> group_clause, int num_groups) {
    (void)root;
    auto* path = makePallocNode<AggPath>();
    path->parent_rel = rel;
    path->subpath = subpath;
    path->aggstrategy = aggstrategy;
    path->group_clause = std::move(group_clause);
    path->num_groups = num_groups;
    if (subpath != nullptr) {
        path->startup_cost = subpath->total_cost;
        int input_rows = static_cast<int>(subpath->rows);
        int ngroups = (num_groups > 0) ? num_groups : 1;
        Cost agg_cost = CostAgg(input_rows, ngroups, subpath->width);
        path->total_cost = path->startup_cost + agg_cost;
        // Aggregated output has one row per group (or one row for plain agg).
        path->rows = (aggstrategy == pgcpp::executor::Agg::Strategy::kPlain)
                         ? 1.0
                         : static_cast<Cardinality>(ngroups);
    }
    return path;
}

ResultPath* create_result_path(PlannerInfo* root, RelOptInfo* rel,
                               std::vector<pgcpp::parser::Node*> quals) {
    (void)root;
    auto* path = makePallocNode<ResultPath>();
    path->parent_rel = rel;
    path->quals = std::move(quals);
    path->rows = 1.0;
    path->startup_cost = 0.0;
    path->total_cost = kCpuTupleCost;
    return path;
}

void add_path(RelOptInfo* rel, Path* path) {
    if (path == nullptr)
        return;
    rel->pathlist.push_back(path);
    // Update cheapest_path if this path is cheaper (or the first).
    if (rel->cheapest_path == nullptr || path->total_cost < rel->cheapest_path->total_cost) {
        rel->cheapest_path = path;
    }
}

Path* cheapest_path(RelOptInfo* rel) {
    return rel->cheapest_path;
}

}  // namespace pgcpp::optimizer
