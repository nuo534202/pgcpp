// parse_oper.h — Operator lookup functions for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_oper.h.
#pragma once

#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/parser/parse_node.h"
#include "mytoydb/parser/primnodes.h"

namespace mytoydb::parser {

using mytoydb::catalog::Oid;
using mytoydb::nodes::Node;

// Operator lookup result.
struct OperatorResult {
    Oid opno = 0;          // operator OID
    Oid opfuncid = 0;      // underlying function OID
    Oid opresulttype = 0;  // result type OID
    bool opretset = false; // returns set?
};

// oper — look up a binary operator by name and operand types.
OperatorResult lookup_operator(const std::string& opname,
                               Oid left_type, Oid right_type);

// make_op — create an OpExpr node for a binary operator.
Node* make_op(ParseState* pstate, const std::string& opname,
              Node* ltree, Node* rtree, int location);

// make_scalar_array_op — create a ScalarArrayOpExpr for IN/ANY/ALL.
Node* make_scalar_array_op(ParseState* pstate, const std::string& opname,
                           bool useOr, Node* ltree, Node* rtree, int location);

}  // namespace mytoydb::parser
