// analyze.cpp — Parse analysis entry point.
//
// Converted from PostgreSQL 15's src/backend/parser/analyze.c.
// Provides parse_analyze(), the public entry point that transforms
// RawStmt parse trees into Query nodes.
#include "mytoydb/parser/analyze.h"

#include <string>
#include <vector>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/parser/parse_agg.h"
#include "mytoydb/parser/parse_clause.h"
#include "mytoydb/parser/parse_expr.h"
#include "mytoydb/parser/parse_relation.h"
#include "mytoydb/parser/parse_target.h"

namespace mytoydb::parser {

using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;

// Forward declarations of internal transform functions.
static Query* transformSelectStmt(ParseState* pstate, SelectStmt* stmt);
static Query* transformInsertStmt(ParseState* pstate, InsertStmt* stmt);
static Query* transformUpdateStmt(ParseState* pstate, UpdateStmt* stmt);
static Query* transformDeleteStmt(ParseState* pstate, DeleteStmt* stmt);
static Query* transformSetOperationStmt(ParseState* pstate, SelectStmt* stmt);

// ---------------------------------------------------------------------------
// parse_analyze — transform a list of RawStmt nodes into a list of Query nodes.
// ---------------------------------------------------------------------------

std::vector<Query*> parse_analyze(std::vector<RawStmt*> parse_trees, const char* source_string) {
    std::vector<Query*> result;

    for (RawStmt* raw_stmt : parse_trees) {
        ParseState* pstate = make_parsestate(nullptr);
        pstate->p_sourcetext = source_string;

        Query* query = transformTopLevelStmt(pstate, raw_stmt);
        result.push_back(query);

        free_parsestate(pstate);
    }

    return result;
}

// ---------------------------------------------------------------------------
// parse_analyze_varparams — like parse_analyze but allows variable parameters.
// ---------------------------------------------------------------------------

std::vector<Query*> parse_analyze_varparams(std::vector<RawStmt*> parse_trees,
                                            const char* source_string) {
    // For now, this is the same as parse_analyze.
    // A full implementation would set up variable parameter handling.
    return parse_analyze(parse_trees, source_string);
}

// ---------------------------------------------------------------------------
// transformTopLevelStmt — transform a top-level statement.
// ---------------------------------------------------------------------------

Query* transformTopLevelStmt(ParseState* pstate, RawStmt* parse_tree) {
    Query* result = transformStmt(pstate, parse_tree->stmt);
    result->stmt_location = parse_tree->stmt_location;
    result->stmt_len = parse_tree->stmt_len;
    return result;
}

// ---------------------------------------------------------------------------
// transformStmt — transform a single statement into a Query.
// ---------------------------------------------------------------------------

Query* transformStmt(ParseState* pstate, Node* stmt) {
    if (stmt == nullptr) {
        return makeNode<Query>();
    }

    NodeTag tag = nodeTag(stmt);

    switch (tag) {
        case NodeTag::kSelectStmt: {
            auto* sel = static_cast<SelectStmt*>(stmt);
            // Check if it's a set operation (UNION, INTERSECT, EXCEPT)
            if (sel->op != SetOperation::kNone) {
                return transformSetOperationStmt(pstate, sel);
            }
            // Check if it's a VALUES clause
            if (!sel->values_lists.empty()) {
                // For now, treat VALUES as a simple SELECT
                return transformSelectStmt(pstate, sel);
            }
            return transformSelectStmt(pstate, sel);
        }
        case NodeTag::kInsertStmt:
            return transformInsertStmt(pstate, static_cast<InsertStmt*>(stmt));
        case NodeTag::kUpdateStmt:
            return transformUpdateStmt(pstate, static_cast<UpdateStmt*>(stmt));
        case NodeTag::kDeleteStmt:
            return transformDeleteStmt(pstate, static_cast<DeleteStmt*>(stmt));
        default:
            // For utility statements and other types, wrap in a CMD_UTILITY Query
            auto* qry = makeNode<Query>();
            qry->command_type = CmdType::kUtility;
            qry->utility_stmt = stmt;
            qry->can_set_tag = true;
            return qry;
    }
}

// ---------------------------------------------------------------------------
// transformSelectStmt — transform a SELECT statement.
// ---------------------------------------------------------------------------

static Query* transformSelectStmt(ParseState* pstate, SelectStmt* stmt) {
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kSelect;
    qry->can_set_tag = true;

    // Process the FROM clause
    if (!stmt->from_clause.empty()) {
        qry->jointree = transformFromClause(pstate, stmt->from_clause);
    } else {
        auto* from_expr = makeNode<FromExpr>();
        qry->jointree = from_expr;
    }

    // Transform target list
    qry->target_list = transformTargetList(pstate, stmt->target_list);

    // Mark column origins
    markTargetListOrigins(pstate, qry->target_list);

    // Transform WHERE clause
    Node* qual = transformWhereClause(pstate, stmt->where_clause, ParseExprKind::kWhere, "WHERE");

    // Transform HAVING clause
    qry->having_qual =
        transformWhereClause(pstate, stmt->having_clause, ParseExprKind::kHaving, "HAVING");

    // Transform ORDER BY clause
    qry->sort_clause = transformSortClause(pstate, stmt->sort_clause, &qry->target_list,
                                           ParseExprKind::kOrderBy, false);

    // Transform GROUP BY clause
    qry->group_clause = transformGroupClause(pstate, stmt->group_clause, &qry->target_list,
                                             qry->sort_clause, ParseExprKind::kGroupBy);
    qry->group_distinct = stmt->group_distinct;

    // Transform DISTINCT clause
    if (stmt->distinct_clause.empty()) {
        qry->distinct_clause.clear();
        qry->has_distinct_on = false;
    } else {
        // Check if it's SELECT DISTINCT or SELECT DISTINCT ON
        if (stmt->distinct_clause[0] == nullptr) {
            // SELECT DISTINCT
            qry->distinct_clause =
                transformDistinctClause(pstate, &qry->target_list, qry->sort_clause, false);
            qry->has_distinct_on = false;
        } else {
            // SELECT DISTINCT ON — simplified handling
            qry->distinct_clause =
                transformDistinctClause(pstate, &qry->target_list, qry->sort_clause, true);
            qry->has_distinct_on = true;
        }
    }

    // Transform LIMIT/OFFSET
    qry->limit_offset =
        transformLimitClause(pstate, stmt->limit_offset, ParseExprKind::kOffset, "OFFSET");
    qry->limit_count =
        transformLimitClause(pstate, stmt->limit_count, ParseExprKind::kLimit, "LIMIT");
    qry->limit_option = stmt->limit_option;

    // Set the range table and join tree
    qry->rtable = pstate->p_rtable;
    if (qry->jointree != nullptr) {
        auto* from_expr = static_cast<FromExpr*>(qry->jointree);
        from_expr->quals = qual;
    }

    // Set query flags
    qry->has_sub_links = pstate->p_has_sub_links;
    qry->has_window_funcs = pstate->p_has_window_funcs;
    qry->has_target_srfs = pstate->p_has_target_srfs;
    qry->has_aggs = pstate->p_has_aggs;

    // Check aggregates
    if (pstate->p_has_aggs || !qry->group_clause.empty() || qry->having_qual != nullptr) {
        parseCheckAggregates(pstate, qry);
    }

    return qry;
}

// ---------------------------------------------------------------------------
// transformInsertStmt — transform an INSERT statement.
// ---------------------------------------------------------------------------

static Query* transformInsertStmt(ParseState* pstate, InsertStmt* stmt) {
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kInsert;
    qry->can_set_tag = true;

    // Add the target relation to the range table
    if (stmt->relation != nullptr) {
        int rtindex = 0;
        RangeTblEntry* rte = addRangeTableEntry(pstate, stmt->relation, stmt->relation->alias,
                                                stmt->relation->inh, false, &rtindex);
        qry->result_relation = rtindex;
        pstate->p_target_relation = rte;
        pstate->p_is_insert = true;
    }

    // Transform the SELECT statement (the source of values)
    if (stmt->select_stmt != nullptr) {
        if (nodeTag(stmt->select_stmt) == NodeTag::kSelectStmt) {
            auto* sel = static_cast<SelectStmt*>(stmt->select_stmt);
            if (!sel->values_lists.empty()) {
                // INSERT ... VALUES — transform as a SELECT with VALUES
                qry->target_list = transformTargetList(pstate, sel->target_list);
            } else {
                // INSERT ... SELECT
                Query* subquery = transformSelectStmt(pstate, sel);
                qry->target_list = subquery->target_list;
                // Merge the range tables
                for (Node* rte : subquery->rtable) {
                    qry->rtable.push_back(rte);
                }
                qry->jointree = subquery->jointree;
            }
        }
    }

    // Set the range table
    if (qry->rtable.empty()) {
        qry->rtable = pstate->p_rtable;
    }

    return qry;
}

// ---------------------------------------------------------------------------
// transformUpdateStmt — transform an UPDATE statement.
// ---------------------------------------------------------------------------

static Query* transformUpdateStmt(ParseState* pstate, UpdateStmt* stmt) {
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kUpdate;
    qry->can_set_tag = true;

    // Add the target relation to the range table
    if (stmt->relation != nullptr) {
        int rtindex = 0;
        RangeTblEntry* rte = addRangeTableEntry(pstate, stmt->relation, stmt->relation->alias,
                                                stmt->relation->inh, true, &rtindex);
        qry->result_relation = rtindex;
        pstate->p_target_relation = rte;

        // Add the target relation to the namespace so column references in
        // SET/WHERE clauses can be resolved.
        auto* ns_item = new ParseNamespaceItem();
        ns_item->p_rte = rte;
        ns_item->p_rtindex = rtindex;
        ns_item->p_names = rte->alias ? rte->alias : rte->eref;
        ns_item->p_rel_visible = true;
        ns_item->p_cols_visible = true;
        ns_item->p_lateral_only = false;
        ns_item->p_lateral_ok = true;
        pstate->p_namespace.push_back(ns_item);

        auto* rtr = makeNode<RangeTblRef>();
        rtr->rtindex = rtindex;
        pstate->p_joinlist.push_back(rtr);
    }

    // Process the FROM clause (additional tables)
    if (!stmt->from_clause.empty()) {
        transformFromClause(pstate, stmt->from_clause);
    }

    // Transform the target list (SET assignments)
    qry->target_list = transformTargetList(pstate, stmt->target_list);

    // Transform WHERE clause
    Node* qual = transformWhereClause(pstate, stmt->where_clause, ParseExprKind::kWhere, "WHERE");

    // Set the range table and join tree
    qry->rtable = pstate->p_rtable;
    auto* from_expr = makeNode<FromExpr>();
    from_expr->fromlist = pstate->p_joinlist;
    from_expr->quals = qual;
    qry->jointree = from_expr;

    qry->has_sub_links = pstate->p_has_sub_links;
    qry->has_aggs = pstate->p_has_aggs;

    return qry;
}

// ---------------------------------------------------------------------------
// transformDeleteStmt — transform a DELETE statement.
// ---------------------------------------------------------------------------

static Query* transformDeleteStmt(ParseState* pstate, DeleteStmt* stmt) {
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kDelete;
    qry->can_set_tag = true;

    // Add the target relation to the range table
    if (stmt->relation != nullptr) {
        int rtindex = 0;
        RangeTblEntry* rte = addRangeTableEntry(pstate, stmt->relation, stmt->relation->alias,
                                                stmt->relation->inh, true, &rtindex);
        qry->result_relation = rtindex;
        pstate->p_target_relation = rte;

        // Add the target relation to the namespace so column references in
        // WHERE/USING clauses can be resolved.
        auto* ns_item = new ParseNamespaceItem();
        ns_item->p_rte = rte;
        ns_item->p_rtindex = rtindex;
        ns_item->p_names = rte->alias ? rte->alias : rte->eref;
        ns_item->p_rel_visible = true;
        ns_item->p_cols_visible = true;
        ns_item->p_lateral_only = false;
        ns_item->p_lateral_ok = true;
        pstate->p_namespace.push_back(ns_item);

        auto* rtr = makeNode<RangeTblRef>();
        rtr->rtindex = rtindex;
        pstate->p_joinlist.push_back(rtr);
    }

    // Process the USING clause (additional tables)
    if (!stmt->using_clause.empty()) {
        transformFromClause(pstate, stmt->using_clause);
    }

    // Transform WHERE clause
    Node* qual = transformWhereClause(pstate, stmt->where_clause, ParseExprKind::kWhere, "WHERE");

    // Set the range table and join tree
    qry->rtable = pstate->p_rtable;
    auto* from_expr = makeNode<FromExpr>();
    from_expr->fromlist = pstate->p_joinlist;
    from_expr->quals = qual;
    qry->jointree = from_expr;

    qry->has_sub_links = pstate->p_has_sub_links;
    qry->has_aggs = pstate->p_has_aggs;

    return qry;
}

// ---------------------------------------------------------------------------
// transformSetOperationStmt — transform a set operation (UNION, INTERSECT,
// EXCEPT) statement.
// ---------------------------------------------------------------------------

static Query* transformSetOperationStmt(ParseState* pstate, SelectStmt* stmt) {
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kSelect;
    qry->can_set_tag = true;

    // Transform the left and right sides of the set operation
    Query* left_query = nullptr;
    Query* right_query = nullptr;

    if (stmt->larg != nullptr) {
        ParseState* left_pstate = make_parsestate(pstate);
        left_query = transformSelectStmt(left_pstate, stmt->larg);
        free_parsestate(left_pstate);
    }

    if (stmt->rarg != nullptr) {
        ParseState* right_pstate = make_parsestate(pstate);
        right_query = transformSelectStmt(right_pstate, stmt->rarg);
        free_parsestate(right_pstate);
    }

    // Build a simple set operation tree
    // For now, we just store the left query's target list and range table
    if (left_query != nullptr) {
        qry->target_list = left_query->target_list;
        qry->rtable = left_query->rtable;
        qry->jointree = left_query->jointree;
    }

    // Transform ORDER BY and LIMIT (applied to the set operation result)
    qry->sort_clause = transformSortClause(pstate, stmt->sort_clause, &qry->target_list,
                                           ParseExprKind::kOrderBy, false);
    qry->limit_offset =
        transformLimitClause(pstate, stmt->limit_offset, ParseExprKind::kOffset, "OFFSET");
    qry->limit_count =
        transformLimitClause(pstate, stmt->limit_count, ParseExprKind::kLimit, "LIMIT");
    qry->limit_option = stmt->limit_option;

    return qry;
}

}  // namespace mytoydb::parser
