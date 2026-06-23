// parse_func.h — Function lookup functions for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_func.h.
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

// FuncLookupResult — result of looking up a function by name and arg types.
struct FuncLookupResult {
    Oid funcid = 0;          // function OID
    Oid rettype = 0;         // return type OID
    bool retset = false;     // returns set?
    bool is_aggregate = false; // is this an aggregate function?
};

// LookupFuncName — look up a function by name and argument types.
// Returns true if found, false otherwise.
bool LookupFuncName(const std::vector<std::string>& funcname,
                    int nargs, const Oid* argtypes,
                    FuncLookupResult* result);

// transformFuncCall — transform a FuncCall (raw parse tree) into a
// FuncExpr or Aggref (transformed expression tree).
Node* transformFuncCall(ParseState* pstate, FuncCall* fn, int location);

// IsAggregateFunction — check if a function name refers to an aggregate.
bool IsAggregateFunction(const std::string& funcname);

}  // namespace mytoydb::parser
