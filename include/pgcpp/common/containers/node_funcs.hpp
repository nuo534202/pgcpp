// node_funcs.h — C++ version of PostgreSQL's nodeFuncs.c (P0 subset).
//
// Converted from PostgreSQL 15's src/include/nodes/nodeFuncs.h and
// src/backend/nodes/nodeFuncs.c. Provides:
//   - Expression metadata queries: exprType / exprTypmod / exprCollation /
//     exprLocation.
//   - A simplified expression_tree_walker covering the P0 node types.
//   - Predicate queries: contain_aggs_of_level / contain_volatile_functions
//     (stub) / contain_subplans.
//
// Only the subset needed by M10 (planner) and M9 (executor) is implemented
// here; the full PG nodeFuncs.c also handles mutation visitors, query-tree
// walkers, etc., which will be added in later phases.

#pragma once

#include <functional>

#include "pgcpp/catalog/catalog.hpp"  // Oid
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/primnodes.hpp"  // concrete node types

namespace pgcpp::nodes {

using pgcpp::catalog::Oid;
using pgcpp::parser::Node;

// ---------------------------------------------------------------------------
// Expression metadata (PG: exprType / exprTypmod / exprCollation / exprLocation)
// ---------------------------------------------------------------------------

// Returns the result type OID of an expression node. Unknown / unsupported
// nodes return InvalidOid (0). nullptr returns InvalidOid.
Oid exprType(const Node* expr);

// Returns the typmod of an expression, or -1 if unknown / unsupported.
int exprTypmod(const Node* expr);

// Returns the collation OID of an expression, or InvalidOid if none / unknown.
Oid exprCollation(const Node* expr);

// Returns the token location of an expression, or -1 if unknown.
int exprLocation(const Node* expr);

// ---------------------------------------------------------------------------
// Tree walker (simplified P0 version)
// ---------------------------------------------------------------------------

// Walker callback: invoked on each node during pre-order traversal. Returning
// true short-circuits the traversal and makes expression_tree_walker return
// true.
using NodeWalker = std::function<bool(Node* node)>;

// Pre-order traversal: calls walker(node) first, then recurses into the
// node's children. Returns true immediately if walker returns true.
//
// Supported container node types (children recursed in declaration order):
//   OpExpr/FuncExpr/ScalarArrayOpExpr  -> args
//   BoolExpr                            -> args
//   NullTest/BooleanTest/RelabelType/   -> arg
//     CoerceViaIO/CoerceToDomain
//   TargetEntry                         -> expr
//   CaseExpr                            -> arg, args, defresult
//   CaseWhen                            -> expr, result
//   SubLink                             -> testexpr, subselect
//   Aggref                              -> aggdirectargs, args, aggorder,
//                                            aggdistinct, aggfilter
//
// Leaf nodes (no recursion): Var, Const, Param, RangeTblRef.
// Unsupported node types: walker is called on the node, then false is
// returned (no recursion into unknown children).
bool expression_tree_walker(Node* node, const NodeWalker& walker);

// ---------------------------------------------------------------------------
// Predicate queries
// ---------------------------------------------------------------------------

// PG: contain_aggs_of_level. Returns true if the tree contains an Aggref
// whose agglevelsup equals `level`.
bool contain_aggs_of_level(Node* node, int level);

// PG: contain_volatile_functions. Returns true if the tree calls a volatile
// function. TODO: requires pg_proc.provolatile lookup; returns false until
// the catalog integration is in place.
bool contain_volatile_functions(Node* node);

// PG: contain_subplans. Returns true if the tree contains a SubLink node.
bool contain_subplans(Node* node);

}  // namespace pgcpp::nodes
