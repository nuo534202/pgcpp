// planner.cpp — Top-level planner entry point.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/planner.c.
//
// The planner takes a parser Query tree and produces an executor Plan tree.
// For SELECT, it delegates to subplanner(). For INSERT/UPDATE/DELETE, it
// plans the source query and wraps the result in a ModifyTable node.
//
// Task 15.3 adds a parallel PG-style pipeline (standard_planner →
// subquery_planner → grouping_planner → query_planner) exercised by the
// new unit tests. The existing planner()/subplanner() entry points remain
// unchanged for ClickBench compatibility.
#include "mytoydb/optimizer/planner.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/optimizer/plan/create_plan.hpp"
#include "mytoydb/optimizer/plan/init_splan.hpp"
#include "mytoydb/optimizer/plan/set_refs.hpp"
#include "mytoydb/optimizer/util/pathnode.hpp"
#include "mytoydb/optimizer/util/relnode.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::optimizer {
using mytoydb::nodes::makePallocNode;

using mytoydb::executor::Agg;
using mytoydb::executor::ModifyTable;
using mytoydb::executor::Plan;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::CmdType;
using mytoydb::parser::FromExpr;
using mytoydb::parser::Node;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::SortGroupClause;
using mytoydb::parser::TargetEntry;

// Forward declaration — implemented in subplanner.cpp.
Plan* subplanner(PlannerInfo* root);

Plan* planner(Query* query) {
    if (query == nullptr)
        return nullptr;

    // Create PlannerInfo.
    auto* root = makePallocNode<PlannerInfo>();
    root->parse = query;

    // Handle LIMIT.
    if (query->limit_count != nullptr) {
        // Try to extract a constant limit value.
        // For now, just set a flag; the actual value is handled in subplanner.
        root->limit_tuples = -1;  // Will be set from the Const if possible.
    }

    Plan* plan = nullptr;

    switch (query->command_type) {
        case CmdType::kSelect:
            plan = subplanner(root);
            break;

        case CmdType::kInsert:
        case CmdType::kUpdate:
        case CmdType::kDelete: {
            // Plan the source query (the SELECT part of INSERT ... SELECT,
            // or the scan for UPDATE/DELETE).
            plan = subplanner(root);

            // Wrap in ModifyTable.
            auto* mt = makePallocNode<ModifyTable>();
            mt->operation = query->command_type;
            mt->resultRelid = query->result_relation;
            mt->lefttree = plan;
            // Copy target list for RETURNING (if any).
            for (Node* te : query->returning_list) {
                mt->targetlist.push_back(static_cast<mytoydb::parser::TargetEntry*>(te));
            }
            plan = mt;
            break;
        }

        default:
            // Other command types (UTILITY, MERGE) are not yet supported.
            break;
    }

    return plan;
}

// =============================================================================
// PG-style planner entry points (Task 15.3)
//
// These mirror PostgreSQL's planner.c entry hierarchy:
//   standard_planner → subquery_planner → grouping_planner → query_planner
//
// The pipeline builds a Path tree (SeqScanPath → AggPath → SortPath) and then
// translates it to a Plan tree via create_plan, finalizing with
// set_plan_references. This is parallel to subplanner() but uses the PG-style
// Path abstraction. The existing planner()/subplanner() path is untouched.
// =============================================================================

// Find the first base relation in the query's join tree (single-table case).
// Returns the 1-based range table index, or 0 if no base relation.
static int FindFirstBaseRelIndex(Query* parse) {
    if (parse->jointree == nullptr)
        return 0;
    if (parse->jointree->GetTag() != NodeTag::kFromExpr)
        return 0;
    auto* from = static_cast<FromExpr*>(parse->jointree);
    if (from->fromlist.empty())
        return 0;
    Node* item = from->fromlist[0];
    if (item == nullptr || item->GetTag() != NodeTag::kRangeTblRef)
        return 0;
    return static_cast<RangeTblRef*>(item)->rtindex;
}

// query_planner — complete PG-style pipeline for the base scan:
//   query_planner_init → path generation → create_plan → set_plan_references.
//
// Builds a Path tree (SeqScanPath, optionally wrapped in AggPath and
// SortPath), translates to a Plan tree, and finalizes references.
Plan* query_planner(PlannerInfo* root, Query* parse) {
    // 1. Initialize planner state (build RelOptInfos, distribute quals).
    query_planner_init(root, parse);

    // 2. Generate paths and select the cheapest for each base relation.
    //    For single-table queries, this creates a SeqScanPath per base rel.
    for (RelOptInfo* rel : root->simple_rel_array) {
        if (rel == nullptr)
            continue;
        SeqScanPath* seqpath = create_seqscan_path(root, rel);
        add_path(rel, seqpath);
    }

    // 3. Build the best Path tree (scan → agg → sort).
    int base_rel = FindFirstBaseRelIndex(parse);
    bool has_aggs = parse->has_aggs || !parse->group_clause.empty();
    bool has_sort = !parse->sort_clause.empty();

    Path* best_path = nullptr;

    if (base_rel == 0) {
        // No FROM clause: use a ResultPath (one-time evaluation).
        RelOptInfo dummy_rel;  // stack-resident placeholder (no catalog data)
        best_path = create_result_path(root, &dummy_rel, {});
    } else {
        RelOptInfo* rel = find_base_rel(root, base_rel);
        if (rel == nullptr)
            return nullptr;
        best_path = cheapest_path(rel);

        // Wrap in AggPath if the query has aggregates or GROUP BY.
        if (has_aggs) {
            Agg::Strategy strategy =
                parse->group_clause.empty() ? Agg::Strategy::kPlain : Agg::Strategy::kHashed;
            // Estimate group count: heuristic (1 for plain agg, 10 for hashed).
            int num_groups = (strategy == Agg::Strategy::kHashed) ? 10 : 1;
            best_path =
                create_agg_path(root, rel, best_path, strategy, parse->group_clause, num_groups);
        }
    }

    // Wrap in SortPath if the query has ORDER BY.
    if (has_sort) {
        // Cast sort_clause (vector<Node*>) to SortGroupClause* list.
        std::vector<SortGroupClause*> pathkeys;
        for (Node* node : parse->sort_clause) {
            if (node != nullptr && node->GetTag() == NodeTag::kSortGroupClause) {
                pathkeys.push_back(static_cast<SortGroupClause*>(node));
            }
        }
        // Use the base rel (or nullptr for no-FROM) as the SortPath's parent.
        RelOptInfo* sort_rel = (base_rel > 0) ? find_base_rel(root, base_rel) : nullptr;
        best_path = create_sort_path(root, sort_rel, best_path, std::move(pathkeys));
    }

    // 4. Translate the Path tree into a Plan tree.
    Plan* plan = create_plan(root, best_path);
    if (plan == nullptr)
        return nullptr;

    // 5. Finalize Var references (PG's setrefs.c).
    set_plan_references(root, plan);

    return plan;
}

// grouping_planner — handle aggregation/sort/distinct/window (simplified).
// Delegates the base scan to query_planner, which builds the complete Path
// tree (including Agg/Sort layers) and translates it to a Plan.
Plan* grouping_planner(PlannerInfo* root, double tuple_fraction, bool can_sort) {
    (void)tuple_fraction;  // LIMIT hint not yet used in the simplified pipeline
    (void)can_sort;        // always allowed in the simplified pipeline
    return query_planner(root, root->parse);
}

// subquery_planner — plan a subquery (simplified: delegates to grouping_planner).
Plan* subquery_planner(PlannerInfo* root, Query* parse, PlannerInfo* parent_root,
                       bool has_recursion, double tuple_fraction) {
    root->parse = parse;
    root->parent_root = parent_root;
    root->has_recursion = has_recursion;
    root->tuple_fraction = tuple_fraction;
    return grouping_planner(root, tuple_fraction, /*can_sort=*/true);
}

// standard_planner — PG-style top-level planner entry.
// Creates a PlannerInfo and delegates to subquery_planner.
Plan* standard_planner(Query* query) {
    if (query == nullptr)
        return nullptr;
    auto* root = makePallocNode<PlannerInfo>();
    return subquery_planner(root, query, /*parent_root=*/nullptr, /*has_recursion=*/false,
                            /*tuple_fraction=*/0.0);
}

}  // namespace mytoydb::optimizer
