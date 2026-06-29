// parse_relation.h — Range table and column resolution for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_relation.h.
#pragma once

#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/containers/node.hpp"
#include "parser/parse_node.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;

// addRangeTableEntry — create an RTE for a relation (table).
// Returns the RTE and sets *rtindex to its position in pstate->p_rtable.
RangeTblEntry* addRangeTableEntry(ParseState* pstate, RangeVar* relation, Alias* alias, bool inh,
                                  bool in_from_cl, int* rtindex);

// addRangeTableEntryForSubquery — create an RTE for a subquery.
RangeTblEntry* addRangeTableEntryForSubquery(ParseState* pstate, Query* subquery, Alias* alias,
                                             bool lateral, bool in_from_cl, int* rtindex);

// refnameRangeTblEntry — find an RTE by alias name in the range table.
RangeTblEntry* refnameRangeTblEntry(ParseState* pstate, const char* refname, int* sublevels_up);

// scanRTEForColumn — search an RTE for a column matching the given name.
// Returns a Var node for the column, or nullptr if not found.
Node* scanRTEForColumn(ParseState* pstate, RangeTblEntry* rte, const std::string& colname,
                       int location);

// colNameToVar — search the namespace for a column matching the given name.
// Returns a Var node, or nullptr if not found.
Node* colNameToVar(ParseState* pstate, const std::string& colname, bool localonly,
                   int* sublevels_up);

// scanNameSpaceForColumn — search the namespace for an unqualified column.
Node* scanNameSpaceForColumn(ParseState* pstate, const std::string& colname, int location);

// expandRTE — expand a range table entry into a list of Vars (for SELECT *).
// Returns a list of TargetEntry-like nodes (actually Var nodes).
std::vector<Node*> expandRTE(ParseState* pstate, RangeTblEntry* rte, int rtindex, int location);

// expandRelAttrs — expand a relation's attributes into Vars (for SELECT *).
std::vector<Node*> expandRelAttrs(ParseState* pstate, RangeTblEntry* rte, int rtindex,
                                  int location);

// buildRangeTblEntry — helper to construct an RTE for a relation.
RangeTblEntry* buildRangeTblEntry(RangeVar* relation, Alias* alias, bool inh, bool in_from_cl);

// addRTEToQuery — add an RTE to the namespace and joinlist.
void addRTEToQuery(ParseState* pstate, RangeTblEntry* rte, bool addToJoinList, bool addToNameSpace,
                   bool allowVLE);

}  // namespace pgcpp::parser
