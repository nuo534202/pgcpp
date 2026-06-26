// parse_clause.cpp — Clause transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_clause.c.
// Transforms FROM clauses (including JOINs and subqueries), WHERE clauses,
// and LIMIT/OFFSET clauses.
#include "mytoydb/parser/parse_clause.hpp"

#include <string>
#include <vector>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/parser/analyze.hpp"
#include "mytoydb/parser/parse_coerce.hpp"
#include "mytoydb/parser/parse_expr.hpp"
#include "mytoydb/parser/parse_relation.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::parser {

using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::nodes::Value;
using mytoydb::types::kBoolOid;
using mytoydb::types::kInt8Oid;

// ---------------------------------------------------------------------------
// transformFromClauseItem — transform one item in the FROM clause.
// Returns the join tree node (RangeTblRef, JoinExpr, etc.).
// ---------------------------------------------------------------------------

Node* transformFromClauseItem(ParseState* pstate, Node* n, RangeTblEntry** top_rte, int* top_rti,
                              std::vector<ParseNamespaceItem*>& nsitem) {
    if (n == nullptr)
        return nullptr;

    NodeTag tag = nodeTag(n);

    // Case 1: RangeVar (plain table reference)
    if (tag == NodeTag::kRangeVar) {
        auto* rv = static_cast<RangeVar*>(n);
        int rtindex = 0;
        RangeTblEntry* rte = addRangeTableEntry(pstate, rv, rv->alias, rv->inh, true, &rtindex);

        if (top_rte)
            *top_rte = rte;
        if (top_rti)
            *top_rti = rtindex;

        // Build the namespace item
        auto* item = makePallocNode<ParseNamespaceItem>();
        item->p_rte = rte;
        item->p_rtindex = rtindex;
        item->p_names = rte->alias ? rte->alias : rte->eref;
        item->p_rel_visible = true;
        item->p_cols_visible = true;
        item->p_lateral_only = false;
        item->p_lateral_ok = true;
        nsitem.push_back(item);

        auto* rtr = makeNode<RangeTblRef>();
        rtr->rtindex = rtindex;
        return rtr;
    }

    // Case 2: RangeSubselect (subquery in FROM)
    if (tag == NodeTag::kRangeSubselect) {
        auto* rs = static_cast<RangeSubselect*>(n);

        // Transform the subquery
        ParseState* sub_pstate = make_parsestate(pstate);
        sub_pstate->p_expr_kind = ParseExprKind::kFromSubselect;
        Query* subquery = transformStmt(sub_pstate, rs->subquery);
        free_parsestate(sub_pstate);

        int rtindex = 0;
        RangeTblEntry* rte =
            addRangeTableEntryForSubquery(pstate, subquery, rs->alias, rs->lateral, true, &rtindex);

        if (top_rte)
            *top_rte = rte;
        if (top_rti)
            *top_rti = rtindex;

        auto* item = makePallocNode<ParseNamespaceItem>();
        item->p_rte = rte;
        item->p_rtindex = rtindex;
        item->p_names = rte->alias ? rte->alias : rte->eref;
        item->p_rel_visible = true;
        item->p_cols_visible = true;
        item->p_lateral_only = false;
        item->p_lateral_ok = true;
        nsitem.push_back(item);

        auto* rtr = makeNode<RangeTblRef>();
        rtr->rtindex = rtindex;
        return rtr;
    }

    // Case 3: JoinExpr (JOIN)
    if (tag == NodeTag::kJoinExpr) {
        auto* j = static_cast<JoinExpr*>(n);

        // Transform left and right sides
        RangeTblEntry* left_rte = nullptr;
        int left_rti = 0;
        std::vector<ParseNamespaceItem*> left_ns;
        Node* left_tree = transformFromClauseItem(pstate, j->larg, &left_rte, &left_rti, left_ns);

        RangeTblEntry* right_rte = nullptr;
        int right_rti = 0;
        std::vector<ParseNamespaceItem*> right_ns;
        Node* right_tree =
            transformFromClauseItem(pstate, j->rarg, &right_rte, &right_rti, right_ns);

        // Add left and right namespace items to pstate so JOIN quals can
        // resolve qualified column references (e.g., hits.user_id).
        for (ParseNamespaceItem* ns : left_ns) {
            pstate->p_namespace.push_back(ns);
        }
        for (ParseNamespaceItem* ns : right_ns) {
            pstate->p_namespace.push_back(ns);
        }

        // Transform the JOIN quals
        Node* quals = nullptr;
        if (j->quals != nullptr) {
            quals = transformExpr(pstate, j->quals, ParseExprKind::kJoinOn);
        }

        // Build the JoinExpr result. MyToyDB does not create a separate join
        // RTE; the left/right RTEs expose their columns directly.
        auto* result = makeNode<JoinExpr>();
        result->jointype = j->jointype;
        result->is_natural = j->is_natural;
        result->larg = left_tree;
        result->rarg = right_tree;
        result->using_clause = j->using_clause;
        result->join_using_alias = j->join_using_alias;
        result->quals = quals;
        result->alias = j->alias;
        result->rtindex = 0;

        // left_ns and right_ns were already added to pstate->p_namespace
        // above (needed for quals resolution). Don't pass them back in nsitem
        // to avoid duplicate namespace entries.

        if (top_rte)
            *top_rte = left_rte;
        if (top_rti)
            *top_rti = left_rti;

        return result;
    }

    ereport(mytoydb::error::LogLevel::kError, "unrecognized node type in FROM clause");
    return nullptr;
}

// ---------------------------------------------------------------------------
// transformFromClause — transform the FROM clause into a join tree.
// Returns a FromExpr node (the root of the join tree).
// ---------------------------------------------------------------------------

Node* transformFromClause(ParseState* pstate, const std::vector<Node*>& frmList) {
    // Transform each item in the FROM list
    for (Node* n : frmList) {
        RangeTblEntry* top_rte = nullptr;
        int top_rti = 0;
        std::vector<ParseNamespaceItem*> nsitem;

        Node* transformed = transformFromClauseItem(pstate, n, &top_rte, &top_rti, nsitem);

        // Add the transformed item to the join list
        pstate->p_joinlist.push_back(transformed);

        // Add the namespace items to the namespace
        for (ParseNamespaceItem* item : nsitem) {
            pstate->p_namespace.push_back(item);
        }
    }

    // Build the FromExpr from the join list
    auto* from_expr = makeNode<FromExpr>();
    from_expr->fromlist = pstate->p_joinlist;
    from_expr->quals = nullptr;  // WHERE clause is added separately

    return from_expr;
}

// ---------------------------------------------------------------------------
// transformWhereClause — transform a WHERE/HAVING clause.
// ---------------------------------------------------------------------------

Node* transformWhereClause(ParseState* pstate, Node* clause, ParseExprKind exprKind,
                           const char* constructName) {
    if (clause == nullptr)
        return nullptr;

    Node* qual = transformExpr(pstate, clause, exprKind);

    // Coerce to boolean (simplified — full implementation would use
    // coerce_to_boolean)
    if (qual != nullptr) {
        Oid qual_type = exprType(qual);
        if (qual_type != kBoolOid && qual_type != 705 /* unknown */) {
            // Try to coerce to boolean
            qual = coerce_type(pstate, qual, qual_type, kBoolOid, -1, CoercionContext::kImplicit,
                               CoercionForm::kImplicit, -1);
        }
    }

    return qual;
}

// ---------------------------------------------------------------------------
// transformLimitClause — transform a LIMIT/OFFSET clause.
// ---------------------------------------------------------------------------

Node* transformLimitClause(ParseState* pstate, Node* clause, ParseExprKind exprKind,
                           const char* constructName) {
    if (clause == nullptr)
        return nullptr;

    Node* qual = transformExpr(pstate, clause, exprKind);

    // Coerce to int8 (bigint)
    if (qual != nullptr) {
        Oid qual_type = exprType(qual);
        if (qual_type != kInt8Oid && qual_type != 705 /* unknown */) {
            qual = coerce_type(pstate, qual, qual_type, kInt8Oid, -1, CoercionContext::kImplicit,
                               CoercionForm::kImplicit, -1);
        }
    }

    return qual;
}

}  // namespace mytoydb::parser
