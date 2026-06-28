// parse_coerce.h — Type coercion functions for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_coerce.h.
#pragma once

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;

// CoercionContext — controls how strict the coercion is.
enum class CoercionContext {
    kImplicit,    // implicitly cast to the target type
    kAssignment,  // assignment cast (slightly more permissive)
    kExplicit,    // explicit CAST() cast
};

// CoercionForm — how to display the coercion (already in parsenodes.h).
// using CoercionForm from parsenodes.h.

// can_coerce_type — can source type be coerced to target type?
bool can_coerce_type(int nargs, const Oid* input_typeids, const Oid* target_typeids,
                     CoercionContext ccontext);

// coerce_type — coerce an expression to the target type.
// Returns the coerced expression, or nullptr if no coercion is needed.
Node* coerce_type(ParseState* pstate, Node* node, Oid input_typeid, Oid target_typeid,
                  int target_typmod, CoercionContext ccontext, CoercionForm cformat, int location);

// coerce_to_target_type — coerce an expression to a target type/modifier.
Node* coerce_to_target_type(ParseState* pstate, Node* expr, Oid expr_type, Oid target_type,
                            int target_typmod, CoercionContext ccontext, CoercionForm cformat,
                            int location);

// select_common_type — select a common type for a list of input types.
// Returns InvalidOid if no common type can be found.
Oid select_common_type(ParseState* pstate, const std::vector<Node*>& exprs, const char* context,
                       Node** which_expr);

// coerce_to_common_type — coerce an expression to a common type.
Node* coerce_to_common_type(ParseState* pstate, Node* node, Oid common_type, const char* context);

// IsBinaryCoercible — can source be binary-coerced to target?
bool IsBinaryCoercible(Oid srctype, Oid targettype);

}  // namespace pgcpp::parser
