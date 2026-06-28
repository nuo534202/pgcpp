// parse_func.h — Function lookup functions for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_func.h.
#pragma once

#include <string>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace mytoydb::parser {

using mytoydb::catalog::Oid;
using mytoydb::nodes::Node;

// FuncLookupResult — result of looking up a function by name and arg types.
struct FuncLookupResult {
    Oid funcid = 0;             // function OID
    Oid rettype = 0;            // return type OID
    bool retset = false;        // returns set?
    bool is_aggregate = false;  // is this an aggregate function?
};

// LookupFuncName — look up a function by name and argument types.
// Returns true if found, false otherwise.
bool LookupFuncName(const std::vector<std::string>& funcname, int nargs, const Oid* argtypes,
                    FuncLookupResult* result);

// transformFuncCall — transform a FuncCall (raw parse tree) into a
// FuncExpr or Aggref (transformed expression tree).
Node* transformFuncCall(ParseState* pstate, FuncCall* fn, int location);

// IsAggregateFunction — check if a function name refers to an aggregate.
bool IsAggregateFunction(const std::string& funcname);

}  // namespace mytoydb::parser
