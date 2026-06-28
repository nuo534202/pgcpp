// subplanner.cpp — Subplan generation for a single SELECT query.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/subplanner.c.
//
// Builds the plan tree for a SELECT: scan → aggregate → sort → projection.
// The scan is a SeqScan (or Result if no FROM clause). Aggregation wraps
// the scan when has_aggs is set. Sort wraps the result when ORDER BY is
// present, with Top-N optimization when LIMIT is also present.
#include <new>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace mytoydb::optimizer {
using mytoydb::nodes::makePallocNode;

using mytoydb::executor::Agg;
using mytoydb::executor::Plan;
using mytoydb::executor::Result;
using mytoydb::executor::SeqScan;
using mytoydb::executor::Sort;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Aggref;
using mytoydb::parser::Const;
using mytoydb::parser::FromExpr;
using mytoydb::parser::makeVar;
using mytoydb::parser::Node;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::RelabelType;
using mytoydb::parser::SortGroupClause;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
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

// Recursively extract all Var nodes from an expression tree.
// Returns the Vars found (does not deduplicate).
static void ExtractVars(Node* expr, std::vector<Var*>* vars) {
    if (expr == nullptr)
        return;
    switch (expr->GetTag()) {
        case NodeTag::kVar:
            vars->push_back(static_cast<Var*>(expr));
            break;
        case NodeTag::kRelabelType:
            ExtractVars(static_cast<RelabelType*>(expr)->arg, vars);
            break;
        case NodeTag::kTargetEntry:
            ExtractVars(static_cast<TargetEntry*>(expr)->expr, vars);
            break;
        case NodeTag::kAggref: {
            auto* agg = static_cast<Aggref*>(expr);
            for (Node* arg : agg->args)
                ExtractVars(arg, vars);
            break;
        }
        case NodeTag::kOpExpr: {
            auto* op = static_cast<mytoydb::parser::OpExpr*>(expr);
            for (Node* arg : op->args)
                ExtractVars(arg, vars);
            break;
        }
        case NodeTag::kFuncExpr: {
            auto* f = static_cast<mytoydb::parser::FuncExpr*>(expr);
            for (Node* arg : f->args)
                ExtractVars(arg, vars);
            break;
        }
        default:
            break;
    }
}

// Build a physical scan target list containing Var nodes for ALL columns
// of the base relation. This matches PostgreSQL's "physical tlist"
// optimization: the scan outputs every column in order, so the Agg's
// argument Vars can reference any column by varattno (which equals the
// position in the scan's output, since BuildTupleDescFromTargetList
// assigns sequential positions). This avoids the SeqScan trying to
// evaluate Aggref nodes (which would return NULL since the scan has no
// aggregates slot).
//
// If the catalog has no attributes for the relation (e.g., in unit tests
// that use synthetic OIDs), fall back to extracting Vars from the Agg's
// target list.
static std::vector<TargetEntry*> BuildScanTargetList(
    mytoydb::catalog::Oid relid, int varno, const std::vector<TargetEntry*>& agg_targetlist) {
    std::vector<TargetEntry*> scan_tlist;
    if (relid != 0 && mytoydb::catalog::GetCatalog() != nullptr) {
        // Get all attributes for the relation (ordered by attnum).
        auto attrs = mytoydb::catalog::GetCatalog()->GetAttributes(relid);
        for (const mytoydb::catalog::FormData_pg_attribute* attr : attrs) {
            if (attr->attnum < 1) {
                continue;  // Skip system columns (attnum < 1).
            }
            auto* var = makeVar(varno, attr->attnum, attr->atttypid, attr->atttypmod,
                                attr->attcollation, 0, -1);
            auto* te = mytoydb::parser::makeNode<TargetEntry>();
            te->expr = var;
            te->resno = attr->attnum;
            te->resname = attr->attname;
            scan_tlist.push_back(te);
        }
        if (!scan_tlist.empty()) {
            return scan_tlist;
        }
    }
    // Fallback: extract Vars from the Agg's target list.
    std::vector<bool> seen;  // tracks varattno values already added
    for (TargetEntry* te : agg_targetlist) {
        if (te == nullptr || te->expr == nullptr)
            continue;
        std::vector<Var*> vars;
        ExtractVars(te->expr, &vars);
        for (Var* var : vars) {
            int attno = var->varattno;
            if (attno < 1)
                continue;
            if (static_cast<size_t>(attno) >= seen.size())
                seen.resize(attno + 1, false);
            if (seen[attno])
                continue;
            seen[attno] = true;
            auto* new_var = mytoydb::parser::makeNode<Var>();
            *new_var = *var;
            auto* new_te = mytoydb::parser::makeNode<TargetEntry>();
            new_te->expr = new_var;
            new_te->resno = attno;
            scan_tlist.push_back(new_te);
        }
    }
    return scan_tlist;
}

// Map GROUP BY clauses to 1-based attribute numbers in the child plan's
// output. Each SortGroupClause's tle_sort_group_ref matches a TargetEntry's
// ressortgroupref. The returned attribute numbers are the varattno values
// of the Vars in the target list entries, which correspond to the column
// positions in the scan's output (since the scan target list places Vars
// at resno = varattno).
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
                // Extract the varattno from the Var in this target entry.
                // The scan's output has Vars at resno = varattno, so the
                // group column index should be varattno.
                std::vector<Var*> vars;
                ExtractVars(te->expr, &vars);
                if (!vars.empty()) {
                    result.push_back(vars[0]->varattno);
                } else {
                    result.push_back(te->resno);
                }
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

    auto* sort = makePallocNode<Sort>();
    sort->lefttree = child;
    sort->limit = limit;
    // The Sort node passes through the child's columns unchanged; its target
    // list must match the child's so the result slot has the right attributes.
    sort->targetlist = child_targetlist;

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
                // Determine sort direction from sortop sentinel:
                // 0 = ASC (default), 1 = DESC.
                bool is_reverse = (sgc->sortop == 1);
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
    // Unwrap RelabelType (e.g., int4->int8 binary coercion wraps the Const).
    if (limit_count->GetTag() == NodeTag::kRelabelType) {
        limit_count = static_cast<RelabelType*>(limit_count)->arg;
    }
    if (limit_count == nullptr || limit_count->GetTag() != NodeTag::kConst)
        return -1;
    auto* con = static_cast<Const*>(limit_count);
    if (con->constisnull)
        return -1;
    return DatumGetInt64(con->constvalue);
}

// Extract a constant OFFSET value from the limit_offset expression.
// Returns 0 if there is no offset or it is not a constant.
static int64_t ExtractLimitOffset(Node* limit_offset) {
    if (limit_offset == nullptr)
        return 0;
    if (limit_offset->GetTag() == NodeTag::kRelabelType) {
        limit_offset = static_cast<RelabelType*>(limit_offset)->arg;
    }
    if (limit_offset == nullptr || limit_offset->GetTag() != NodeTag::kConst)
        return 0;
    auto* con = static_cast<Const*>(limit_offset);
    if (con->constisnull)
        return 0;
    return DatumGetInt64(con->constvalue);
}

Plan* subplanner(PlannerInfo* root) {
    Query* query = root->parse;

    // Build base relation infos and paths.
    BuildBaseRelInfos(root);

    // Extract the LIMIT count (for Top-N optimization).
    int64_t limit = ExtractLimitCount(query->limit_count);
    // Extract the OFFSET count (rows to skip after sort).
    int64_t offset = ExtractLimitOffset(query->limit_offset);

    // Determine the base scan plan.
    Plan* plan = nullptr;
    std::vector<TargetEntry*> targetlist = ConvertTargetList(query->target_list);
    Node* qual = ExtractQual(query);
    bool has_aggs = query->has_aggs || !query->group_clause.empty();

    int base_rel = FindBaseRelIndex(query);
    if (base_rel == 0) {
        // No FROM clause — create a Result plan.
        plan = makePallocNode<Result>();
        plan->targetlist = targetlist;
        plan->qual = nullptr;
    } else {
        // Create a SeqScan plan for the base relation.
        // For aggregate queries, the scan target list must contain only Var
        // nodes (not Aggref nodes), because the SeqScan cannot evaluate
        // aggregates. BuildScanTargetList builds a physical tlist with Var
        // nodes for ALL base table columns, so the Agg's argument Vars can
        // reference any column by varattno.
        std::vector<TargetEntry*> scan_tlist = targetlist;
        if (has_aggs) {
            // Look up the relation OID from the range table entry.
            int rte_idx = base_rel - 1;  // 1-based to 0-based
            mytoydb::catalog::Oid relid = 0;
            if (rte_idx >= 0 && rte_idx < static_cast<int>(query->rtable.size())) {
                Node* rte_node = query->rtable[rte_idx];
                if (rte_node != nullptr && rte_node->GetTag() == NodeTag::kRangeTblEntry) {
                    relid = static_cast<mytoydb::catalog::Oid>(
                        static_cast<RangeTblEntry*>(rte_node)->relid);
                }
            }
            scan_tlist = BuildScanTargetList(relid, base_rel, targetlist);
        }
        auto* scan = makePallocNode<SeqScan>();
        scan->scanrelid = base_rel;
        scan->targetlist = scan_tlist;
        scan->qual = qual;
        plan = scan;
    }

    // Layer aggregation on top if needed.
    if (has_aggs) {
        auto* agg = makePallocNode<Agg>();

        if (query->group_clause.empty()) {
            agg->aggstrategy = Agg::Strategy::kPlain;
        } else {
            agg->aggstrategy = Agg::Strategy::kHashed;
            agg->groupColIdx = MapGroupColIdx(query->group_clause, targetlist);
        }

        // The Agg's target list is the query's target list (which contains
        // Aggref nodes for aggregates and Var nodes for GROUP BY columns).
        agg->targetlist = targetlist;
        agg->lefttree = plan;
        agg->having_qual = query->having_qual;
        plan = agg;
    }

    // Layer sort on top if needed.
    Sort* sort = BuildSortPlan(query, plan, targetlist, limit);
    if (sort != nullptr) {
        sort->offset = offset;
        plan = sort;
    }

    return plan;
}

}  // namespace mytoydb::optimizer
