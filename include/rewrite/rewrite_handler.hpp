// rewrite_handler.hpp — Query rewrite system public API.
//
// Converted from PostgreSQL 15's src/include/rewrite/rewriteHandler.h.
//
// QueryRewrite is the main entry point, called between parse_analyze and
// the planner. It performs:
//   1. View expansion — replaces view RTEs with subquery RTEs
//   2. Rule application — applies INSTEAD rules for non-SELECT commands
//   3. Row Level Security — injects policy quals into the query
#pragma once

#include "parser/parsenodes.hpp"

namespace pgcpp::rewrite {

using pgcpp::parser::Query;

// QueryRewrite — apply rewrite rules to a query tree.
//
// Takes the parse-analyzed Query and returns the (possibly rewritten) Query.
// For SELECT on a view, the view RTE is expanded to a subquery RTE.
// For INSERT/UPDATE/DELETE on a table with rules, INSTEAD rules may
// replace the query entirely.
//
// The returned Query* may be the same as the input or a new Query.
Query* QueryRewrite(Query* query);

// RewriteView — expand a single view RTE into a subquery RTE.
// Called internally by QueryRewrite for each RTE that references a view.
// Returns true if the RTE was rewritten.
bool RewriteView(Query* query, int rt_index);

// ApplyRowSecurity — inject RLS policy quals into a query.
// Called internally by QueryRewrite for relations with RLS enabled.
void ApplyRowSecurity(Query* query, int rt_index);

// --- View query storage (in-memory, until node tree serialization) ---

// StoreViewQuery — cache a view's parsed Query tree for later expansion.
// Called by DefineView when CREATE VIEW is executed.
void StoreViewQuery(int relid, Query* query);

// RetrieveViewQuery — get the cached Query tree for a view.
// Returns nullptr if no view query is stored for this relid.
Query* RetrieveViewQuery(int relid);

// --- Row Level Security policy storage (in-memory) ---

// StoreRowSecurityPolicy — cache an RLS policy qual for a relation.
void StoreRowSecurityPolicy(int relid, pgcpp::parser::Node* qual);

// RetrieveRowSecurityPolicy — get the RLS policy qual for a relation.
// Returns nullptr if no policy is stored.
pgcpp::parser::Node* RetrieveRowSecurityPolicy(int relid);

// EnableRowSecurity — mark a relation as having RLS enabled.
void EnableRowSecurity(int relid);

// IsRowSecurityEnabled — check if RLS is enabled for a relation.
bool IsRowSecurityEnabled(int relid);

}  // namespace pgcpp::rewrite
