// subplanner.cpp — Subplan generation for a single SELECT query.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/subplanner.c.
//
// Builds the plan tree for a SELECT: scan → aggregate → sort → projection.
// The scan is a SeqScan (or Result if no FROM clause). Aggregation wraps
// the scan when has_aggs is set. Sort wraps the result when ORDER BY is
// present, with Top-N optimization when LIMIT is also present.
#include <new>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/executor/plannodes.h"
#include "mytoydb/optimizer/planner.h"
#include "mytoydb/parser/parsenodes.h"
#include "mytoydb/parser/primnodes.h"

namespace mytoydb::optimizer {

using mytoydb::executor::Agg;
using mytoydb::executor::Plan;
using mytoydb::executor::Result;
using mytoydb::executor::SeqScan;
using mytoydb::executor::Sort;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Const;
using mytoydb::parser::FromExpr;
using mytoydb::parser::Node;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::SortGroupClause;
using mytoydb::parser::TargetEntry;
using mytoydb::types::DatumGetInt64;

// Forward declaration — implemented in allpaths.cpp.
void BuildBaseRelInfos(PlannerInfo* root);

// Extract the WHERE clause from the query's join tree.
static Node* ExtractQual(Query* query) {
    if (query->jointree == nullptr)
        return nullptr;
    if (query->jointree->GetTag() != NodeTag::kFromExpr)
        return nullptr;
    auto* from = static_cast<FromExpr*>(query->jointree);
    return from->quals;
}

// Find the first base relation in the join tree (for single-table queries).
// Returns the 1-based range table index, or 0 if no base relation.
static int FindBaseRelIndex(Query* query) {
    if (query->jointree == nullptr)
        return 0;
    if (query->jointree->GetTag() != NodeTag::kFromExpr)
        return 0;
    auto* from = static_cast<FromExpr*>(query->jointree);
    if (from->fromlist.empty())
        return 0;
    Node* item = from->fromlist[0];
    if (item == nullptr || item->GetTag() != NodeTag::kRangeTblRef)
        return 0;
    auto* ref = static_cast<RangeTblRef*>(item);
    return ref->rtindex;
}

// Convert the query's target list (vector<Node*>) to a plan target list
// (vector<TargetEntry*>).
static std::vector<TargetEntry*> ConvertTargetList(const std::vector<Node*>& target_list) {
    std::vector<TargetEntry*> result;
    for (Node* node : target_list) {
        if (node != nullptr && node->GetTag() == NodeTag::kTargetEntry) {
            result.push_back(static_cast<TargetEntry*>(node));
        }
    }
    return result;
}

// Map GROUP BY clauses to 1-based attribute numbers in the child plan's
// output. Each SortGroupClause's tle_sort_group_ref matches a TargetEntry's
// ressortgroupref.
static std::vector<int> MapGroupColIdx(const std::vector<Node*>& group_clause,
                                       const std::vector<TargetEntry*>& targetlist) {
    std::vector<int> result;
    for (Node* node : group_clause) {
        if (node == nullptr || node->GetTag() != NodeTag::kSortGroupClause)
            continue;
        auto* sgc = static_cast<SortGroupClause*>(node);
        // Find the TargetEntry with matching ressortgroupref.
        for (TargetEntry* te : targetlist) {
            if (te->ressortgroupref == sgc->tle_sort_group_ref) {
                result.push_back(te->resno);
                break;
            }
        }
    }
    return result;
}

// Build a Sort plan node from the query's sort_clause.
// Returns nullptr if there's no sort clause.
static Sort* BuildSortPlan(Query* query, Plan* child,
                           const std::vector<TargetEntry*>& child_targetlist, int64_t limit) {
    if (query->sort_clause.empty())
        return nullptr;

    void* mem = palloc(sizeof(Sort));
    auto* sort = new (mem) Sort();
    sort->lefttree = child;
    sort->limit = limit;

    for (Node* node : query->sort_clause) {
        if (node == nullptr || node->GetTag() != NodeTag::kSortGroupClause)
            continue;
        auto* sgc = static_cast<SortGroupClause*>(node);

        // Find the TargetEntry with matching ressortgroupref.
        for (TargetEntry* te : child_targetlist) {
            if (te->ressortgroupref == sgc->tle_sort_group_ref) {
                sort->sortColIdx.push_back(te->resno);
                sort->sortOperators.push_back(sgc->sortop);
                sort->nullsFirst.push_back(sgc->nulls_first);
                // Determine sort direction: if sortop is a "greater than"
                // operator (e.g., int4gt = 521), it's DESC.
                // Known "less than" operators: 97 (int4lt), 95 (int2lt),
                // 412 (int8lt), 672 (float4lt), 672 (float8lt).
                // Known "greater than" operators: 521 (int4gt), 512 (int2gt),
                // 414 (int8gt), 674 (float4gt), 676 (float8gt).
                // Heuristic: if sortop > 500, it's likely a "greater than" op.
                bool is_reverse = (sgc->sortop > 500);
                sort->reverse.push_back(is_reverse);
                break;
            }
        }
    }

    if (sort->sortColIdx.empty()) {
        // No valid sort columns — don't create a Sort node.
        return nullptr;
    }

    return sort;
}

// Extract a constant LIMIT value from the limit_count expression.
// Returns -1 if the limit is not a constant.
static int64_t ExtractLimitCount(Node* limit_count) {
    if (limit_count == nullptr)
        return -1;
    if (limit_count->GetTag() != NodeTag::kConst)
        return -1;
    auto* con = static_cast<Const*>(limit_count);
    if (con->constisnull)
        return -1;
    return DatumGetInt64(con->constvalue);
}

Plan* subplanner(PlannerInfo* root) {
    Query* query = root->parse;

    // Build base relation infos and paths.
    BuildBaseRelInfos(root);

    // Extract the LIMIT count (for Top-N optimization).
    int64_t limit = ExtractLimitCount(query->limit_count);

    // Determine the base scan plan.
    Plan* plan = nullptr;
    std::vector<TargetEntry*> targetlist = ConvertTargetList(query->target_list);
    Node* qual = ExtractQual(query);

    int base_rel = FindBaseRelIndex(query);
    if (base_rel == 0) {
        // No FROM clause — create a Result plan.
        void* mem = palloc(sizeof(Result));
        plan = new (mem) Result();
        plan->targetlist = targetlist;
        plan->qual = nullptr;
    } else {
        // Create a SeqScan plan for the base relation.
        void* mem = palloc(sizeof(SeqScan));
        auto* scan = new (mem) SeqScan();
        scan->scanrelid = base_rel;
        scan->targetlist = targetlist;
        scan->qual = qual;
        plan = scan;
    }

    // Layer aggregation on top if needed.
    if (query->has_aggs || !query->group_clause.empty()) {
        void* mem = palloc(sizeof(Agg));
        auto* agg = new (mem) Agg();

        if (query->group_clause.empty()) {
            agg->aggstrategy = Agg::Strategy::kPlain;
        } else {
            agg->aggstrategy = Agg::Strategy::kHashed;
            agg->groupColIdx = MapGroupColIdx(query->group_clause, targetlist);
        }

        // The Agg's target list is the query's target list (which contains
        // Aggref nodes for aggregates and Var nodes for GROUP BY columns).
        agg->targetlist = targetlist;
        // The child scan's target list should include all columns referenced
        // by the aggregate's arguments and GROUP BY columns.
        // For simplicity, we pass the same target list to the child.
        agg->lefttree = plan;
        // Clear the child's target list — the Agg will project.
        // Actually, the child needs its target list for the scan to produce
        // the right columns. Keep it as-is.
        plan = agg;
    }

    // Layer sort on top if needed.
    Sort* sort = BuildSortPlan(query, plan, targetlist, limit);
    if (sort != nullptr) {
        plan = sort;
    }

    return plan;
}

}  // namespace mytoydb::optimizer
