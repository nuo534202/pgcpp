// analyze.cpp — Parse analysis entry point.
//
// Converted from PostgreSQL 15's src/backend/parser/analyze.c.
// Provides parse_analyze(), the public entry point that transforms
// RawStmt parse trees into Query nodes.
#include "parser/analyze.hpp"

#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "parser/parse_agg.hpp"
#include "parser/parse_clause.hpp"
#include "parser/parse_coerce.hpp"
#include "parser/parse_cte.hpp"
#include "parser/parse_expr.hpp"
#include "parser/parse_relation.hpp"
#include "parser/parse_target.hpp"
#include "parser/parse_type.hpp"
#include "parser/parse_utilcmd.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;

// Forward declarations of internal transform functions.
static Query* transformSelectStmt(ParseState* pstate, SelectStmt* stmt);
static Query* transformInsertStmt(ParseState* pstate, InsertStmt* stmt);
static Query* transformUpdateStmt(ParseState* pstate, UpdateStmt* stmt);
static Query* transformDeleteStmt(ParseState* pstate, DeleteStmt* stmt);
static Query* transformSetOperationStmt(ParseState* pstate, SelectStmt* stmt);

// ---------------------------------------------------------------------------
// parse_analyze — transform a list of RawStmt nodes into a list of Query nodes.
// ---------------------------------------------------------------------------

std::vector<Query*> parse_analyze(const std::vector<RawStmt*>& parse_trees,
                                  const char* source_string) {
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

std::vector<Query*> parse_analyze_varparams(const std::vector<RawStmt*>& parse_trees,
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
        case NodeTag::kCreateStmt:
            return transformCreateStmt(pstate, static_cast<CreateStmt*>(stmt));
        case NodeTag::kAlterTableStmt:
            return transformAlterTableStmt(pstate, static_cast<AlterTableStmt*>(stmt));
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

    // Process the WITH clause first, so CTEs are visible to FROM/WHERE/etc.
    if (stmt->with_clause != nullptr) {
        transformWithClause(pstate, stmt->with_clause);
    }

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

    // F-4b: ON CONFLICT is parsed but not implemented — fail explicitly
    // rather than silently ignoring the clause (which would cause unique
    // violations to error instead of being handled).
    if (stmt->on_conflict_clause != nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ON CONFLICT is not supported");
    }

    // Process the WITH clause first, so CTEs are visible to the source query.
    if (stmt->with_clause != nullptr) {
        transformWithClause(pstate, stmt->with_clause);
    }

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
                if (sel->values_lists.size() > 1) {
                    // Multi-row INSERT: build a VALUES RTE so the planner
                    // can emit a ValuesScan over all rows. Each row's
                    // expressions are coerced to the corresponding target
                    // column type, and the query's target list becomes Vars
                    // referencing the VALUES RTE's columns.
                    //
                    // Range table layout (1-based):
                    //   [1..N]   target relation (added above)
                    //   N+1      VALUES RTE (added here)
                    auto* values_rte = makeNode<RangeTblEntry>();
                    values_rte->rtekind = RTEKind::kValues;
                    values_rte->in_from_cl = true;
                    values_rte->lateral = false;

                    // Build eref with synthetic column names (col1, col2, ...).
                    size_t num_cols = sel->values_lists[0].size();
                    auto* eref = makeNode<Alias>();
                    eref->aliasname = "*VALUES*";
                    for (size_t i = 0; i < num_cols; ++i) {
                        eref->colnames.push_back(
                            pgcpp::nodes::makeString("col" + std::to_string(i + 1)));
                    }
                    values_rte->eref = eref;

                    // Coerce each row's expressions to the target column
                    // types, then move them into the RTE.
                    RangeTblEntry* tgt_rte = pstate->p_target_relation;
                    std::vector<Oid> tgt_types;
                    std::vector<int> tgt_typmods;
                    if (tgt_rte != nullptr && GetCatalog() != nullptr && tgt_rte->relid != 0) {
                        auto attrs = GetCatalog()->GetAttributes(tgt_rte->relid);
                        for (const auto* attr : attrs) {
                            if (attr->attnum <= 0)
                                continue;
                            tgt_types.push_back(attr->atttypid);
                            tgt_typmods.push_back(attr->atttypmod);
                        }
                    }

                    for (auto& row : sel->values_lists) {
                        std::vector<Node*> coerced_row;
                        coerced_row.reserve(row.size());
                        for (size_t i = 0; i < row.size(); ++i) {
                            // Transform the raw parse-tree node (e.g., AConst →
                            // Const) before coercion. The single-row INSERT
                            // path goes through transformTargetList →
                            // transformExpr; the multi-row path must do the
                            // same, otherwise ValuesScan's ExecEvalExpr sees
                            // an AConst (unsupported) and throws
                            // "unsupported expression type in ExecEvalExpr".
                            Node* expr =
                                transformExpr(pstate, row[i], ParseExprKind::kInsertTarget);
                            if (i < tgt_types.size()) {
                                Oid expr_type = exprType(expr);
                                if (expr_type != tgt_types[i]) {
                                    Node* coerced = coerce_to_target_type(
                                        pstate, expr, expr_type, tgt_types[i], tgt_typmods[i],
                                        CoercionContext::kAssignment, CoercionForm::kImplicit, -1);
                                    if (coerced != nullptr) {
                                        expr = coerced;
                                    }
                                }
                            }
                            coerced_row.push_back(expr);
                        }
                        values_rte->values_lists.push_back(std::move(coerced_row));
                    }

                    // Add the VALUES RTE to the range table.
                    pstate->p_rtable.push_back(values_rte);
                    int values_rtindex = static_cast<int>(pstate->p_rtable.size());

                    // Build a FromExpr with a RangeTblRef pointing at the RTE.
                    auto* rtr = makeNode<RangeTblRef>();
                    rtr->rtindex = values_rtindex;
                    auto* from_expr = makeNode<FromExpr>();
                    from_expr->fromlist.push_back(rtr);
                    from_expr->quals = nullptr;
                    qry->jointree = from_expr;

                    // Build the target list as Vars (varno=values_rtindex,
                    // varattno=1..N) referencing the VALUES RTE's columns.
                    // The Vars carry the target column types so ModifyTable's
                    // downstream projection matches the table schema.
                    std::vector<Node*> target_list;
                    for (size_t i = 0; i < num_cols; ++i) {
                        Oid col_type = (i < tgt_types.size()) ? tgt_types[i] : 0;
                        int col_typmod = (i < tgt_typmods.size()) ? tgt_typmods[i] : -1;
                        Var* var = makeVar(values_rtindex, static_cast<int>(i) + 1, col_type,
                                           col_typmod, 0, 0, -1);
                        auto* res = makeNode<ResTarget>();
                        res->val = var;
                        target_list.push_back(res);
                    }
                    qry->target_list = transformTargetList(pstate, target_list);
                } else {
                    // Single-row INSERT ... VALUES — convert the first row's
                    // expressions into ResTarget nodes and transform them
                    // into a target list.
                    const auto& first_row = sel->values_lists[0];
                    std::vector<Node*> target_list;
                    for (Node* expr : first_row) {
                        auto* res = makeNode<ResTarget>();
                        res->val = expr;
                        target_list.push_back(res);
                    }
                    qry->target_list = transformTargetList(pstate, target_list);

                    // Coerce each value to the corresponding target column type.
                    // This is essential for string literals (UNKNOWN type) being
                    // inserted into TEXT/VARCHAR columns — the Datum must be
                    // converted from a C string to a varlena structure.
                    if (pstate->p_target_relation != nullptr) {
                        RangeTblEntry* rte = pstate->p_target_relation;
                        if (GetCatalog() != nullptr && rte->relid != 0) {
                            auto attrs = GetCatalog()->GetAttributes(rte->relid);
                            size_t attr_idx = 0;
                            for (Node* tle_node : qry->target_list) {
                                if (attr_idx >= attrs.size())
                                    break;
                                const auto* attr = attrs[attr_idx];
                                if (attr->attnum <= 0) {
                                    ++attr_idx;
                                    continue;
                                }
                                if (nodeTag(tle_node) == NodeTag::kTargetEntry) {
                                    auto* te = static_cast<TargetEntry*>(tle_node);
                                    Oid expr_type = exprType(te->expr);
                                    if (expr_type != attr->atttypid) {
                                        Node* coerced = coerce_to_target_type(
                                            pstate, te->expr, expr_type, attr->atttypid,
                                            attr->atttypmod, CoercionContext::kAssignment,
                                            CoercionForm::kImplicit, -1);
                                        if (coerced != nullptr) {
                                            te->expr = coerced;
                                        }
                                    }
                                }
                                ++attr_idx;
                            }
                        }
                    }
                }
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

    // F-4g: RETURNING on UPDATE is not implemented — the executor silently
    // returns zero rows. Fail explicitly rather than producing wrong results.
    if (!stmt->returning_list.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "RETURNING is not supported on UPDATE");
    }

    // Process the WITH clause first, so CTEs are visible to SET/FROM/WHERE.
    if (stmt->with_clause != nullptr) {
        transformWithClause(pstate, stmt->with_clause);
    }

    // Add the target relation to the range table
    if (stmt->relation != nullptr) {
        int rtindex = 0;
        RangeTblEntry* rte = addRangeTableEntry(pstate, stmt->relation, stmt->relation->alias,
                                                stmt->relation->inh, true, &rtindex);
        qry->result_relation = rtindex;
        pstate->p_target_relation = rte;

        // Add the target relation to the namespace so column references in
        // SET/WHERE clauses can be resolved.
        auto* ns_item = makePallocNode<ParseNamespaceItem>();
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

    // Expand UPDATE's SET target list to include ALL table columns.
    // SET columns keep their SET expression (with resno fixed to attnum);
    // non-SET columns get a Var referencing the target table column.
    // Without this, the SeqScan would only output SET columns (losing
    // non-SET column values) and ModifyTable's ExecProject would have
    // nothing to project — causing heap_update to write an all-NULL tuple.
    // Mirrors PostgreSQL's rewriteTargetListUid in rewriteHandler.c.
    if (pstate->p_target_relation != nullptr && qry->result_relation > 0) {
        qry->target_list = expandUpdateTargetList(pstate, pstate->p_target_relation,
                                                  qry->result_relation, qry->target_list);
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
// transformDeleteStmt — transform a DELETE statement.
// ---------------------------------------------------------------------------

static Query* transformDeleteStmt(ParseState* pstate, DeleteStmt* stmt) {
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kDelete;
    qry->can_set_tag = true;

    // F-4g: RETURNING on DELETE is not implemented — the executor silently
    // returns zero rows. Fail explicitly rather than producing wrong results.
    if (!stmt->returning_list.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "RETURNING is not supported on DELETE");
    }

    // Process the WITH clause first, so CTEs are visible to USING/WHERE.
    if (stmt->with_clause != nullptr) {
        transformWithClause(pstate, stmt->with_clause);
    }

    // Add the target relation to the range table
    if (stmt->relation != nullptr) {
        int rtindex = 0;
        RangeTblEntry* rte = addRangeTableEntry(pstate, stmt->relation, stmt->relation->alias,
                                                stmt->relation->inh, true, &rtindex);
        qry->result_relation = rtindex;
        pstate->p_target_relation = rte;

        // Add the target relation to the namespace so column references in
        // WHERE/USING clauses can be resolved.
        auto* ns_item = makePallocNode<ParseNamespaceItem>();
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
    (void)right_query;

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

}  // namespace pgcpp::parser
