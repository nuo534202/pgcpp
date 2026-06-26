// parse_node.h — ParseState and related structures for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_node.h.
// ParseState holds the context needed during transformExpr / transformStmt
// to resolve column references, build range tables, and track expression state.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::parser {

using mytoydb::catalog::Oid;
using mytoydb::nodes::Node;

// ---------------------------------------------------------------------------
// ParseExprKind — what kind of expression we're parsing.
// Used to detect invalid aggregate/window function placements.
// ---------------------------------------------------------------------------

enum class ParseExprKind {
    kNone = 0,             // not in an expression
    kOther,                // reserved for extensions
    kJoinOn,               // JOIN ON
    kJoinUsing,            // JOIN USING
    kFromSubselect,        // sub-SELECT in FROM clause
    kFromFunction,         // function in FROM clause
    kWhere,                // WHERE
    kHaving,               // HAVING
    kFilter,               // FILTER
    kWindowPartition,      // window definition PARTITION BY
    kWindowOrder,          // window definition ORDER BY
    kWindowFrameRange,     // window frame clause with RANGE
    kWindowFrameRows,      // window frame clause with ROWS
    kWindowFrameGroups,    // window frame clause with GROUPS
    kSelectTarget,         // SELECT target list item
    kInsertTarget,         // INSERT target list item
    kUpdateSource,         // UPDATE assignment source item
    kUpdateTarget,         // UPDATE assignment target item
    kMergeWhen,            // MERGE WHEN condition
    kGroupBy,              // GROUP BY
    kOrderBy,              // ORDER BY
    kDistinctOn,           // DISTINCT ON
    kLimit,                // LIMIT
    kOffset,               // OFFSET
    kReturning,            // RETURNING
    kValues,               // VALUES
    kValuesSingle,         // single-row VALUES (in INSERT only)
    kCheckConstraint,      // CHECK constraint for a table
    kDomainCheck,          // CHECK constraint for a domain
    kColumnDefault,        // default value for a table column
    kFunctionDefault,      // default parameter value for function
    kIndexExpression,      // index expression
    kIndexPredicate,       // index predicate
    kStatsExpression,      // extended statistics expression
    kAlterColTransform,    // transform expr in ALTER COLUMN TYPE
    kExecuteParameter,     // parameter value in EXECUTE
    kTriggerWhen,          // WHEN condition in CREATE TRIGGER
    kPolicy,               // USING or WITH CHECK expr in policy
    kPartitionBound,       // partition bound expression
    kPartitionExpression,  // PARTITION BY expression
    kCallArgument,         // procedure argument in CALL
    kCopyWhere,            // WHERE condition in COPY FROM
    kGeneratedColumn,      // generation expression for a column
    kCycleMark,            // cycle mark value
};

// ---------------------------------------------------------------------------
// ParseNamespaceColumn — per-column namespace data.
// ---------------------------------------------------------------------------

struct ParseNamespaceColumn {
    int varno = 0;            // rangetable index
    int varattno = 0;         // attribute number of the column
    Oid vartype = 0;          // pg_type OID
    int vartypmod = -1;       // type modifier value
    Oid varcollid = 0;        // OID of collation, or InvalidOid
    int varnosyn = 0;         // rangetable index of syntactic referent
    int varattnosyn = 0;      // attribute number of syntactic referent
    bool dontexpand = false;  // not included in star expansion
};

// ---------------------------------------------------------------------------
// ParseNamespaceItem — one entry in the namespace (visible range table entry).
// ---------------------------------------------------------------------------

struct ParseNamespaceItem {
    Alias* p_names = nullptr;                       // Table and column names
    RangeTblEntry* p_rte = nullptr;                 // The relation's rangetable entry
    int p_rtindex = 0;                              // The relation's index in the rangetable
    std::vector<ParseNamespaceColumn> p_nscolumns;  // per-column data
    bool p_rel_visible = false;                     // Relation name is visible?
    bool p_cols_visible = false;                    // Column names visible as unqualified refs?
    bool p_lateral_only = false;                    // Is only visible to LATERAL expressions?
    bool p_lateral_ok = false;                      // If so, does join type allow use?
};

// ---------------------------------------------------------------------------
// ParseState — holds the context for parse analysis of one query level.
// ---------------------------------------------------------------------------

class ParseState {
public:
    ParseState* parent_parse_state = nullptr;      // stack link
    const char* p_sourcetext = nullptr;            // source text, or nullptr
    std::vector<Node*> p_rtable;                   // range table so far
    std::vector<Node*> p_joinexprs;                // JoinExprs for RTE_JOIN entries
    std::vector<Node*> p_joinlist;                 // join items (will become FromExpr)
    std::vector<ParseNamespaceItem*> p_namespace;  // currently-referenceable RTEs
    bool p_lateral_active = false;                 // p_lateral_only items visible?
    std::vector<Node*> p_ctenamespace;             // current namespace for CTEs
    std::vector<Node*> p_future_ctes;              // CTEs not yet in namespace
    CommonTableExpr* p_parent_cte = nullptr;       // this query's containing CTE
    RangeTblEntry* p_target_relation = nullptr;    // INSERT/UPDATE/DELETE target
    ParseNamespaceItem* p_target_nsitem = nullptr;
    bool p_is_insert = false;         // process assignment like INSERT
    std::vector<Node*> p_windowdefs;  // raw representations of window clauses
    ParseExprKind p_expr_kind = ParseExprKind::kNone;
    int p_next_resno = 1;                    // next targetlist resno to assign
    std::vector<Node*> p_multiassign_exprs;  // junk tlist entries for multiassign
    std::vector<Node*> p_locking_clause;     // raw FOR UPDATE/FOR SHARE info
    bool p_locked_from_parent = false;
    bool p_resolve_unknowns = true;  // resolve unknown-type as text

    // Flags telling about things found in the query:
    bool p_has_aggs = false;
    bool p_has_window_funcs = false;
    bool p_has_target_srfs = false;
    bool p_has_sub_links = false;
    bool p_has_modifying_cte = false;

    Node* p_last_srf = nullptr;  // most recent set-returning func/op found

    ParseState() = default;
    ~ParseState() = default;
};

// ---------------------------------------------------------------------------
// ParseState management functions.
// ---------------------------------------------------------------------------

// make_parsestate — create a new ParseState, optionally linked to a parent.
ParseState* make_parsestate(ParseState* parent);

// free_parsestate — free a ParseState (in C++ we just delete it; the range
// table entries and namespace items are owned by the Query being built).
void free_parsestate(ParseState* pstate);

// ---------------------------------------------------------------------------
// Helper functions for expression construction.
// ---------------------------------------------------------------------------

// make_const — convert an A_Const (raw parse tree) to a Const (transformed).
Const* make_const(ParseState* pstate, AConst* aconst);

// makeBoolConst — create a boolean Const node.
Node* makeBoolConst(bool value, bool isnull);

// make_andclause — create an AND BoolExpr from a list of expressions.
Node* make_andclause(std::vector<Node*> args);

// make_orclause — create an OR BoolExpr from a list of expressions.
Node* make_orclause(std::vector<Node*> args);

// make_notclause — create a NOT BoolExpr from an expression.
Node* make_notclause(Node* arg);

// make_ands_implicit — if there's more than one qual, AND them together.
Node* make_ands_implicit(std::vector<Node*> andclauses);

// ---------------------------------------------------------------------------
// Expression type utility functions (used throughout parse analysis).
// ---------------------------------------------------------------------------

// exprType — get the type OID of an expression node.
Oid exprType(const Node* expr);

// exprTypmod — get the type modifier of an expression node.
int exprTypmod(const Node* expr);

}  // namespace mytoydb::parser
