// create_plan.cpp — Path-to-Plan translation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/createplan.c.
//
// Translates the optimizer's chosen Path tree into an executor Plan tree.
// For scan paths, builds the target list and extracts scan clauses from the
// RelOptInfo's baserestrictinfo. For upper nodes (Agg/Sort), recursively
// translates the subpath. Join paths are skeleton implementations.
#include "optimizer/plan/create_plan.hpp"

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "common/containers/node.hpp"
#include "optimizer/util/pathnode.hpp"
#include "optimizer/util/restrictinfo.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::optimizer {
using pgcpp::catalog::GetCatalog;
using pgcpp::executor::Agg;
using pgcpp::executor::HashJoin;
using pgcpp::executor::IndexScan;
using pgcpp::executor::MergeJoin;
using pgcpp::executor::NestLoop;
using pgcpp::executor::Plan;
using pgcpp::executor::Result;
using pgcpp::executor::SeqScan;
using pgcpp::executor::Sort;
using pgcpp::executor::SubqueryScan;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Aggref;
using pgcpp::parser::FromExpr;
using pgcpp::parser::Node;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Query;
using pgcpp::parser::RelabelType;
using pgcpp::parser::SortGroupClause;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;

// --- Helpers (re-implemented from subplanner.cpp, which keeps its own static copies) ---

// Convert the query's target_list (vector<Node*>) to vector<TargetEntry*>.
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
            auto* op = static_cast<OpExpr*>(expr);
            for (Node* arg : op->args)
                ExtractVars(arg, vars);
            break;
        }
        case NodeTag::kFuncExpr: {
            auto* f = static_cast<pgcpp::parser::FuncExpr*>(expr);
            for (Node* arg : f->args)
                ExtractVars(arg, vars);
            break;
        }
        default:
            break;
    }
}

// Build a physical scan target list containing Var nodes for ALL columns of
// the base relation (PG's "physical tlist" optimization). If the catalog has
// no attributes, falls back to extracting Vars from the agg targetlist.
static std::vector<TargetEntry*> BuildPhysicalTlist(
    pgcpp::catalog::Oid relid, int varno, const std::vector<TargetEntry*>& agg_targetlist) {
    std::vector<TargetEntry*> scan_tlist;
    if (relid != 0 && GetCatalog() != nullptr) {
        auto attrs = GetCatalog()->GetAttributes(relid);
        for (const pgcpp::catalog::FormData_pg_attribute* attr : attrs) {
            if (attr->attnum < 1)
                continue;  // skip system columns
            auto* var = pgcpp::parser::makeVar(varno, attr->attnum, attr->atttypid, attr->atttypmod,
                                               attr->attcollation, 0, -1);
            auto* te = makePallocNode<TargetEntry>();
            te->expr = var;
            te->resno = attr->attnum;
            te->resname = attr->attname;
            scan_tlist.push_back(te);
        }
        if (!scan_tlist.empty())
            return scan_tlist;
    }
    // Fallback: extract Vars from the Agg's target list.
    std::vector<bool> seen;
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
            auto* new_var = makePallocNode<Var>();
            *new_var = *var;
            auto* new_te = makePallocNode<TargetEntry>();
            new_te->expr = new_var;
            new_te->resno = attno;
            scan_tlist.push_back(new_te);
        }
    }
    return scan_tlist;
}

// Extract scan clauses (Node*) from a RelOptInfo's baserestrictinfo.
static std::vector<Node*> ExtractScanClauses(RelOptInfo* rel) {
    std::vector<Node*> clauses;
    if (rel == nullptr)
        return clauses;
    for (RestrictInfo* ri : rel->baserestrictinfo) {
        if (ri != nullptr && ri->clause != nullptr)
            clauses.push_back(ri->clause);
    }
    return clauses;
}

// Combine a list of qual clauses into a single expression. If there's one
// clause, return it directly; if multiple, AND them together.
static Node* CombineQuals(const std::vector<Node*>& clauses) {
    if (clauses.empty())
        return nullptr;
    if (clauses.size() == 1)
        return clauses[0];
    auto* boolexpr = makePallocNode<pgcpp::parser::BoolExpr>();
    boolexpr->boolop = pgcpp::parser::BoolExprType::kAnd;
    boolexpr->args = clauses;
    return boolexpr;
}

// Map GROUP BY clauses to 1-based attribute numbers in the child plan's output.
static std::vector<int> MapGroupColIdx(const std::vector<Node*>& group_clause,
                                       const std::vector<TargetEntry*>& targetlist) {
    std::vector<int> result;
    for (Node* node : group_clause) {
        if (node == nullptr || node->GetTag() != NodeTag::kSortGroupClause)
            continue;
        auto* sgc = static_cast<SortGroupClause*>(node);
        for (TargetEntry* te : targetlist) {
            if (te->ressortgroupref == sgc->tle_sort_group_ref) {
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

// --- Scan plan builders ---

SeqScan* create_seqscan_plan(PlannerInfo* root, SeqScanPath* path, std::vector<TargetEntry*> tlist,
                             std::vector<Node*> scan_clauses) {
    (void)root;
    auto* scan = makePallocNode<SeqScan>();
    // scanrelid is the 1-based range table index from the parent RelOptInfo.
    scan->scanrelid = (path->parent_rel != nullptr) ? path->parent_rel->relindex : path->relid;
    scan->targetlist = std::move(tlist);
    scan->qual = CombineQuals(scan_clauses);
    scan->startup_cost = path->startup_cost;
    scan->total_cost = path->total_cost;
    scan->plan_rows = static_cast<int>(path->rows);
    scan->plan_width = path->width;
    return scan;
}

IndexScan* create_indexscan_plan(PlannerInfo* root, IndexPath* path,
                                 std::vector<TargetEntry*> tlist, std::vector<Node*> scan_clauses) {
    (void)root;
    auto* scan = makePallocNode<IndexScan>();
    scan->scanrelid = (path->parent_rel != nullptr) ? path->parent_rel->relindex : path->relid;
    scan->indexid = path->indexid;
    scan->indexqual = path->indexqual;
    scan->targetlist = std::move(tlist);
    // Non-index quals (residual) go into the plan's qual field.
    scan->qual = CombineQuals(scan_clauses);
    scan->startup_cost = path->startup_cost;
    scan->total_cost = path->total_cost;
    scan->plan_rows = static_cast<int>(path->rows);
    scan->plan_width = path->width;
    return scan;
}

// create_scan_plan — dispatch for scan paths. Computes the target list and
// scan clauses, then calls the appropriate builder.
static Plan* create_scan_plan(PlannerInfo* root, Path* best_path) {
    RelOptInfo* rel = best_path->parent_rel;
    Query* query = root->parse;
    std::vector<TargetEntry*> query_tlist = ConvertTargetList(query->target_list);
    bool has_aggs = query->has_aggs || !query->group_clause.empty();

    // Build the scan target list.
    std::vector<TargetEntry*> scan_tlist = query_tlist;
    if (has_aggs && rel != nullptr) {
        scan_tlist = BuildPhysicalTlist(rel->relid, rel->relindex, query_tlist);
    }
    // Extract scan clauses from the rel's baserestrictinfo.
    std::vector<Node*> scan_clauses = ExtractScanClauses(rel);

    switch (best_path->type) {
        case PathType::kSeqScan:
            return create_seqscan_plan(root, static_cast<SeqScanPath*>(best_path), scan_tlist,
                                       scan_clauses);
        case PathType::kIndexScan:
            return create_indexscan_plan(root, static_cast<IndexPath*>(best_path), scan_tlist,
                                         scan_clauses);
        case PathType::kSubqueryScan:
            // SubqueryScan carries its own tlist; scan_clauses ignored.
            return create_subqueryscan_plan(root, static_cast<SubqueryScanPath*>(best_path));
        default:
            return nullptr;
    }
}

// --- Join plan builders (skeletons) ---

NestLoop* create_nestloop_plan(PlannerInfo* root, NestLoopPath* path) {
    (void)root;
    auto* nl = makePallocNode<NestLoop>();
    nl->jointype = pgcpp::parser::JoinType::kInner;
    if (path->outer != nullptr)
        nl->lefttree = create_plan(root, path->outer);
    if (path->inner != nullptr)
        nl->righttree = create_plan(root, path->inner);
    // Join quals: extract clauses from the restrictlist.
    std::vector<Node*> join_clauses;
    for (RestrictInfo* ri : path->restrictlist) {
        if (ri != nullptr && ri->clause != nullptr)
            join_clauses.push_back(ri->clause);
    }
    nl->qual = CombineQuals(join_clauses);
    return nl;
}

HashJoin* create_hashjoin_plan(PlannerInfo* root, HashJoinPath* path) {
    (void)root;
    auto* hj = makePallocNode<HashJoin>();
    hj->jointype = pgcpp::parser::JoinType::kInner;
    hj->hashclauses = path->hashclauses;
    if (path->outer != nullptr)
        hj->lefttree = create_plan(root, path->outer);
    if (path->inner != nullptr) {
        // Build a Hash node as the right child.
        auto* hash = makePallocNode<pgcpp::executor::Hash>();
        hash->lefttree = create_plan(root, path->inner);
        hj->righttree = hash;
    }
    return hj;
}

// --- Task 15.15: MergeJoin + SubqueryScan plan builders ---

MergeJoin* create_mergejoin_plan(PlannerInfo* root, MergeJoinPath* path) {
    (void)root;
    auto* mj = makePallocNode<MergeJoin>();
    mj->jointype = path->jointype;
    mj->mergeclauses = path->mergeclauses;
    if (path->outer != nullptr)
        mj->lefttree = create_plan(root, path->outer);
    if (path->inner != nullptr)
        mj->righttree = create_plan(root, path->inner);
    return mj;
}

SubqueryScan* create_subqueryscan_plan(PlannerInfo* root, SubqueryScanPath* path) {
    (void)root;
    auto* ss = makePallocNode<SubqueryScan>();
    ss->scanrelid = path->scanrelid;
    ss->targetlist = path->tlist;
    if (path->subpath != nullptr) {
        ss->lefttree = create_plan(root, path->subpath);
    }
    return ss;
}

static Plan* create_join_plan(PlannerInfo* root, Path* best_path) {
    switch (best_path->type) {
        case PathType::kNestLoop:
            return create_nestloop_plan(root, static_cast<NestLoopPath*>(best_path));
        case PathType::kHashJoin:
            return create_hashjoin_plan(root, static_cast<HashJoinPath*>(best_path));
        case PathType::kMergeJoin:
            return create_mergejoin_plan(root, static_cast<MergeJoinPath*>(best_path));
        default:
            return nullptr;
    }
}

// --- Upper plan builders (Agg/Sort/Result) ---

Result* create_result_plan(PlannerInfo* root, ResultPath* path) {
    (void)root;
    auto* result = makePallocNode<Result>();
    // Result's target list is the query's target list.
    result->targetlist = ConvertTargetList(root->parse->target_list);
    result->qual = CombineQuals(path->quals);
    result->plan_rows = 1;
    return result;
}

Agg* create_agg_plan(PlannerInfo* root, AggPath* path) {
    auto* agg = makePallocNode<Agg>();
    agg->aggstrategy = path->aggstrategy;
    // The Agg's target list is the query's target list (Aggref nodes).
    agg->targetlist = ConvertTargetList(root->parse->target_list);
    agg->having_qual = root->parse->having_qual;
    // Recursively translate the subpath.
    if (path->subpath != nullptr) {
        agg->lefttree = create_plan(root, path->subpath);
    }
    // Map GROUP BY columns to attribute numbers.
    if (path->aggstrategy == Agg::Strategy::kHashed && !path->group_clause.empty()) {
        agg->groupColIdx = MapGroupColIdx(path->group_clause, agg->targetlist);
    }
    agg->plan_rows = static_cast<int>(path->rows);
    return agg;
}

Sort* create_sort_plan(PlannerInfo* root, SortPath* path) {
    (void)root;
    auto* sort = makePallocNode<Sort>();
    // Recursively translate the subpath.
    if (path->subpath != nullptr) {
        sort->lefttree = create_plan(root, path->subpath);
    }
    // The Sort node passes through the child's columns.
    if (sort->lefttree != nullptr) {
        sort->targetlist = sort->lefttree->targetlist;
    }
    // Build sort info from pathkeys (SortGroupClause list).
    for (SortGroupClause* sgc : path->pathkeys) {
        if (sgc == nullptr)
            continue;
        // Find the TargetEntry with matching ressortgroupref in the child tlist.
        for (TargetEntry* te : sort->targetlist) {
            if (te->ressortgroupref == sgc->tle_sort_group_ref) {
                sort->sortColIdx.push_back(te->resno);
                sort->sortOperators.push_back(sgc->sortop);
                sort->nullsFirst.push_back(sgc->nulls_first);
                sort->reverse.push_back(sgc->sortop == 1);  // 1 = DESC sentinel
                break;
            }
        }
    }
    return sort;
}

static Plan* create_upper_plan(PlannerInfo* root, Path* best_path) {
    switch (best_path->type) {
        case PathType::kAgg:
            return create_agg_plan(root, static_cast<AggPath*>(best_path));
        case PathType::kSort:
            return create_sort_plan(root, static_cast<SortPath*>(best_path));
        case PathType::kResult:
            return create_result_plan(root, static_cast<ResultPath*>(best_path));
        default:
            return nullptr;
    }
}

// --- Top-level dispatch ---

Plan* create_plan(PlannerInfo* root, Path* best_path) {
    if (best_path == nullptr)
        return nullptr;
    switch (best_path->type) {
        case PathType::kSeqScan:
        case PathType::kIndexScan:
        case PathType::kSubqueryScan:
            return create_scan_plan(root, best_path);
        case PathType::kNestLoop:
        case PathType::kHashJoin:
        case PathType::kMergeJoin:
            return create_join_plan(root, best_path);
        case PathType::kAgg:
        case PathType::kSort:
        case PathType::kResult:
            return create_upper_plan(root, best_path);
        default:
            return nullptr;
    }
}

}  // namespace pgcpp::optimizer
