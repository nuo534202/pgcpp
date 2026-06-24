// parse_target.h — Target list transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/include/parser/parse_target.h.
#pragma once

#include <string>
#include <vector>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/parser/parse_node.h"
#include "mytoydb/parser/primnodes.h"

namespace mytoydb::parser {

using mytoydb::nodes::Node;

// transformTargetList — transform a raw target list (SELECT clause)
// into a list of TargetEntry nodes.
std::vector<Node*> transformTargetList(ParseState* pstate, std::vector<Node*> targetlist);

// transformTargetEntry — transform a single ResTarget into a TargetEntry.
TargetEntry* transformTargetEntry(ParseState* pstate, ResTarget* res, ParseExprKind exprKind);

// expandTargetList — expand * and table.* in the target list.
std::vector<Node*> expandTargetList(ParseState* pstate, std::vector<Node*> targetlist);

// markTargetListOrigins — mark the origin table/column for each target entry.
void markTargetListOrigins(ParseState* pstate, std::vector<Node*>& targetlist);

// transformSortClause — transform ORDER BY clause into SortGroupClause list.
std::vector<Node*> transformSortClause(ParseState* pstate, std::vector<Node*> orderlist,
                                       std::vector<Node*>* targetlist, ParseExprKind exprKind,
                                       bool useSQL99);

// transformGroupClause — transform GROUP BY clause into SortGroupClause list.
std::vector<Node*> transformGroupClause(ParseState* pstate, std::vector<Node*> grouplist,
                                        std::vector<Node*>* targetlist,
                                        std::vector<Node*> sortClause, ParseExprKind exprKind);

// transformDistinctClause — transform DISTINCT clause.
std::vector<Node*> transformDistinctClause(ParseState* pstate, std::vector<Node*>* targetlist,
                                           std::vector<Node*> distinctClause, bool isOn);

// assignSortGroupRef — assign a SortGroupRef to a target entry if needed.
int assignSortGroupRef(TargetEntry* tle, std::vector<Node*>& targetlist);

// findTargetlistEntrySQL99 — find or create a target entry for a sort/group expr.
TargetEntry* findTargetlistEntrySQL99(ParseState* pstate, Node* node,
                                      std::vector<Node*>* targetlist, ParseExprKind exprKind);

}  // namespace mytoydb::parser
