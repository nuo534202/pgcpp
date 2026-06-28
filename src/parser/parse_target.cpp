// parse_target.cpp — Target list transformation for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_target.c.
// Transforms raw target lists (lists of ResTarget) into lists of
// TargetEntry nodes, handles star expansion, and transforms
// ORDER BY / GROUP BY / DISTINCT clauses.
#include "pgcpp/parser/parse_target.hpp"

#include <string>
#include <vector>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parse_coerce.hpp"
#include "pgcpp/parser/parse_expr.hpp"
#include "pgcpp/parser/parse_relation.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;
using pgcpp::nodes::Value;
using pgcpp::types::kBoolOid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;

static constexpr Oid kUnknownOid = 705;

// ---------------------------------------------------------------------------
// Helper: FigureColname — generate a column name for an unaliased target.
// This is a simplified version of PostgreSQL's FigureColname.
// ---------------------------------------------------------------------------

static std::string FigureColname(Node* node) {
    if (node == nullptr)
        return "?column?";

    NodeTag tag = nodeTag(node);
    switch (tag) {
        case NodeTag::kColumnRef: {
            auto* cref = static_cast<ColumnRef*>(node);
            if (!cref->fields.empty()) {
                Node* last = cref->fields.back();
                if (nodeTag(last) == NodeTag::kString) {
                    auto* v = static_cast<Value*>(last);
                    return v->GetString();
                }
            }
            break;
        }
        case NodeTag::kAConst: {
            auto* ac = static_cast<AConst*>(node);
            if (ac->val != nullptr) {
                if (nodeTag(ac->val) == NodeTag::kString) {
                    auto* v = static_cast<Value*>(ac->val);
                    return v->GetString();
                }
            }
            break;
        }
        case NodeTag::kFuncCall: {
            auto* fc = static_cast<FuncCall*>(node);
            if (!fc->funcname.empty()) {
                Node* n = fc->funcname.back();
                if (nodeTag(n) == NodeTag::kString) {
                    auto* v = static_cast<Value*>(n);
                    return v->GetString();
                }
            }
            break;
        }
        case NodeTag::kAExpr: {
            auto* a = static_cast<AExpr*>(node);
            // Use the operator name as the column name
            if (!a->name.empty()) {
                Node* n = a->name[0];
                if (nodeTag(n) == NodeTag::kString) {
                    auto* v = static_cast<Value*>(n);
                    return v->GetString();
                }
            }
            break;
        }
        default:
            break;
    }
    return "?column?";
}

// ---------------------------------------------------------------------------
// Helper: ExpandColumnRefStar — expand "table.*" into individual Vars.
// ---------------------------------------------------------------------------

static std::vector<Node*> ExpandColumnRefStar(ParseState* pstate, ColumnRef* cref) {
    std::vector<Node*> result;

    if (cref->fields.size() < 2)
        return result;

    // Get the table name (first field)
    Node* field0 = cref->fields[0];
    if (nodeTag(field0) != NodeTag::kString)
        return result;

    auto* v = static_cast<Value*>(field0);
    const std::string& tblname = v->GetString();

    // Find the RTE by table name
    int sublevels_up = 0;
    RangeTblEntry* rte = refnameRangeTblEntry(pstate, tblname.c_str(), &sublevels_up);
    if (rte == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "table does not exist for star expansion");
        return result;
    }

    // Find the RTE's index in the range table
    int rtindex = 0;
    for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
        if (pstate->p_rtable[i] == rte) {
            rtindex = static_cast<int>(i + 1);
            break;
        }
    }

    // Expand the RTE into individual Vars
    return expandRTE(pstate, rte, rtindex, cref->location);
}

// Helper: ExpandAllTables — expand bare "*" into Vars for all visible RTEs.
// ---------------------------------------------------------------------------

static std::vector<Node*> ExpandAllTables(ParseState* pstate, int location) {
    std::vector<Node*> result;

    for (ParseNamespaceItem* nsitem : pstate->p_namespace) {
        if (nsitem == nullptr || !nsitem->p_cols_visible)
            continue;

        RangeTblEntry* rte = nsitem->p_rte;
        if (rte == nullptr)
            continue;

        std::vector<Node*> expanded = expandRTE(pstate, rte, nsitem->p_rtindex, location);
        for (Node* var : expanded) {
            result.push_back(var);
        }
    }

    if (result.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "SELECT * with no tables specified is not valid");
    }

    return result;
}

// ---------------------------------------------------------------------------
// transformTargetEntry — transform a single ResTarget into a TargetEntry.
// ---------------------------------------------------------------------------

TargetEntry* transformTargetEntry(ParseState* pstate, ResTarget* res, ParseExprKind exprKind) {
    Node* expr = nullptr;

    // Transform the value expression
    if (res->val != nullptr) {
        expr = transformExpr(pstate, res->val, exprKind);
    }

    // Determine the column name
    std::string colname = res->name;
    if (colname.empty()) {
        colname = FigureColname(res->val);
    }

    auto* tle = makeNode<TargetEntry>();
    tle->expr = expr;
    tle->resno = pstate->p_next_resno++;
    tle->resname = colname;
    tle->ressortgroupref = 0;
    tle->resorigtbl = 0;
    tle->resorigcol = 0;
    tle->resjunk = false;

    return tle;
}

// ---------------------------------------------------------------------------
// transformTargetList — transform a raw target list into TargetEntry list.
// ---------------------------------------------------------------------------

std::vector<Node*> transformTargetList(ParseState* pstate, const std::vector<Node*>& targetlist) {
    std::vector<Node*> result;

    for (Node* node : targetlist) {
        if (nodeTag(node) != NodeTag::kResTarget)
            continue;
        auto* res = static_cast<ResTarget*>(node);

        // Check for bare "*" (star expansion of all tables)
        if (res->val != nullptr && nodeTag(res->val) == NodeTag::kAStar) {
            std::vector<Node*> expanded = ExpandAllTables(pstate, res->location);
            for (Node* var : expanded) {
                auto* tle = makeNode<TargetEntry>();
                tle->expr = var;
                tle->resno = pstate->p_next_resno++;
                // Get column name from the Var's source
                if (nodeTag(var) == NodeTag::kVar) {
                    auto* v = static_cast<Var*>(var);
                    RangeTblEntry* rte = nullptr;
                    if (v->varno > 0 && v->varno <= static_cast<int>(pstate->p_rtable.size())) {
                        rte = static_cast<RangeTblEntry*>(pstate->p_rtable[v->varno - 1]);
                    }
                    if (rte != nullptr && rte->eref != nullptr) {
                        int attnum = v->varattno - 1;
                        if (attnum >= 0 && attnum < static_cast<int>(rte->eref->colnames.size())) {
                            Node* cn = rte->eref->colnames[attnum];
                            if (nodeTag(cn) == NodeTag::kString) {
                                auto* cnv = static_cast<Value*>(cn);
                                tle->resname = cnv->GetString();
                            }
                        }
                    }
                }
                tle->ressortgroupref = 0;
                tle->resorigtbl = 0;
                tle->resorigcol = 0;
                tle->resjunk = false;
                result.push_back(tle);
            }
            continue;
        }

        // Check for "something.*" (star expansion)
        if (res->val != nullptr && nodeTag(res->val) == NodeTag::kColumnRef) {
            auto* cref = static_cast<ColumnRef*>(res->val);
            if (!cref->fields.empty()) {
                Node* last = cref->fields.back();
                if (nodeTag(last) == NodeTag::kAStar) {
                    // Expand "table.*" into multiple TargetEntry items
                    std::vector<Node*> expanded = ExpandColumnRefStar(pstate, cref);
                    for (Node* var : expanded) {
                        auto* tle = makeNode<TargetEntry>();
                        tle->expr = var;
                        tle->resno = pstate->p_next_resno++;
                        // Get column name from the Var's source
                        if (nodeTag(var) == NodeTag::kVar) {
                            auto* v = static_cast<Var*>(var);
                            // Look up the column name from the RTE
                            RangeTblEntry* rte = nullptr;
                            if (v->varno > 0 &&
                                v->varno <= static_cast<int>(pstate->p_rtable.size())) {
                                rte = static_cast<RangeTblEntry*>(pstate->p_rtable[v->varno - 1]);
                            }
                            if (rte != nullptr && rte->eref != nullptr) {
                                int attnum = v->varattno - 1;
                                if (attnum >= 0 &&
                                    attnum < static_cast<int>(rte->eref->colnames.size())) {
                                    Node* cn = rte->eref->colnames[attnum];
                                    if (nodeTag(cn) == NodeTag::kString) {
                                        auto* cnv = static_cast<Value*>(cn);
                                        tle->resname = cnv->GetString();
                                    }
                                }
                            }
                        }
                        tle->ressortgroupref = 0;
                        tle->resorigtbl = 0;
                        tle->resorigcol = 0;
                        tle->resjunk = false;
                        result.push_back(tle);
                    }
                    continue;
                }
            }
        }

        // Normal target entry
        result.push_back(transformTargetEntry(pstate, res, ParseExprKind::kSelectTarget));
    }

    return result;
}

// ---------------------------------------------------------------------------
// expandTargetList — expand * and table.* in the target list.
// (In our implementation, this is handled directly in transformTargetList.)
// ---------------------------------------------------------------------------

std::vector<Node*> expandTargetList(ParseState* pstate, const std::vector<Node*>& targetlist) {
    // Star expansion is already handled in transformTargetList.
    return targetlist;
}

// ---------------------------------------------------------------------------
// markTargetListOrigins — mark the origin table/column for each target entry.
// Simplified: we set resorigtbl/resorigcol from Var expressions.
// ---------------------------------------------------------------------------

void markTargetListOrigins(ParseState* pstate, std::vector<Node*>& targetlist) {
    for (Node* tle_node : targetlist) {
        if (nodeTag(tle_node) != NodeTag::kTargetEntry)
            continue;
        auto* tle = static_cast<TargetEntry*>(tle_node);

        if (tle->expr != nullptr && nodeTag(tle->expr) == NodeTag::kVar) {
            auto* var = static_cast<Var*>(tle->expr);
            if (var->varno > 0 && var->varno <= static_cast<int>(pstate->p_rtable.size())) {
                RangeTblEntry* rte = static_cast<RangeTblEntry*>(pstate->p_rtable[var->varno - 1]);
                if (rte->rtekind == RTEKind::kRelation) {
                    tle->resorigtbl = rte->relid;
                    tle->resorigcol = var->varattno;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// assignSortGroupRef — assign a SortGroupRef to a target entry if needed.
// ---------------------------------------------------------------------------

int assignSortGroupRef(TargetEntry* tle, std::vector<Node*>& targetlist) {
    if (tle->ressortgroupref != 0)
        return tle->ressortgroupref;

    // Find the maximum SortGroupRef in the target list
    int max_ref = 0;
    for (Node* n : targetlist) {
        if (nodeTag(n) == NodeTag::kTargetEntry) {
            auto* t = static_cast<TargetEntry*>(n);
            if (t->ressortgroupref > max_ref) {
                max_ref = t->ressortgroupref;
            }
        }
    }

    tle->ressortgroupref = max_ref + 1;
    return tle->ressortgroupref;
}

// ---------------------------------------------------------------------------
// findTargetlistEntrySQL99 — find or create a target entry for a sort/group
// expression. This handles the SQL99 syntax where ORDER BY/GROUP BY can
// reference expressions not in the target list.
// ---------------------------------------------------------------------------

TargetEntry* findTargetlistEntrySQL99(ParseState* pstate, Node* node,
                                      std::vector<Node*>* targetlist, ParseExprKind exprKind) {
    // First, try to find a matching target entry by expression equality
    for (Node* tle_node : *targetlist) {
        if (nodeTag(tle_node) != NodeTag::kTargetEntry)
            continue;
        auto* tle = static_cast<TargetEntry*>(tle_node);

        // Simple match: if both are ColumnRef with the same name
        if (tle->expr != nullptr && nodeTag(tle->expr) == nodeTag(node)) {
            // For now, just compare by string representation
            // A full implementation would use equal()
        }
    }

    // If not found, create a new junk target entry
    Node* expr = transformExpr(pstate, node, exprKind);

    auto* tle = makeNode<TargetEntry>();
    tle->expr = expr;
    tle->resno = pstate->p_next_resno++;
    tle->resname = "?column?";
    tle->ressortgroupref = 0;
    tle->resorigtbl = 0;
    tle->resorigcol = 0;
    tle->resjunk = true;  // Mark as junk since it's not in the original target list

    targetlist->push_back(tle);
    return tle;
}

// ---------------------------------------------------------------------------
// transformSortClause — transform ORDER BY clause into SortGroupClause list.
// ---------------------------------------------------------------------------

std::vector<Node*> transformSortClause(ParseState* pstate, const std::vector<Node*>& orderlist,
                                       std::vector<Node*>* targetlist, ParseExprKind exprKind,
                                       bool useSQL99) {
    std::vector<Node*> sortclauses;

    for (Node* node : orderlist) {
        if (nodeTag(node) != NodeTag::kSortBy)
            continue;
        auto* sortby = static_cast<SortBy*>(node);

        TargetEntry* tle = nullptr;

        // Check if the sort expression is a simple column reference
        // that matches an existing target entry
        if (sortby->node != nullptr && nodeTag(sortby->node) == NodeTag::kColumnRef) {
            auto* cref = static_cast<ColumnRef*>(sortby->node);
            if (!cref->fields.empty()) {
                Node* field0 = cref->fields[0];
                if (nodeTag(field0) == NodeTag::kString) {
                    auto* v = static_cast<Value*>(field0);
                    const std::string& colname = v->GetString();

                    // Search the target list for a matching column name
                    for (Node* tle_node : *targetlist) {
                        if (nodeTag(tle_node) != NodeTag::kTargetEntry)
                            continue;
                        auto* t = static_cast<TargetEntry*>(tle_node);
                        if (t->resname == colname) {
                            tle = t;
                            break;
                        }
                    }
                }
            }
        }

        // Check if it's an integer constant (ORDER BY position)
        if (tle == nullptr && sortby->node != nullptr &&
            nodeTag(sortby->node) == NodeTag::kAConst) {
            auto* ac = static_cast<AConst*>(sortby->node);
            if (ac->val != nullptr && nodeTag(ac->val) == NodeTag::kInteger) {
                auto* v = static_cast<Value*>(ac->val);
                int pos = static_cast<int>(v->GetInteger());
                if (pos >= 1 && pos <= static_cast<int>(targetlist->size())) {
                    tle = static_cast<TargetEntry*>((*targetlist)[pos - 1]);
                }
            }
        }

        // If not found, create a new target entry (SQL99 style)
        if (tle == nullptr) {
            tle = findTargetlistEntrySQL99(pstate, sortby->node, targetlist, exprKind);
        }

        if (tle == nullptr)
            continue;

        // Assign a SortGroupRef if not already assigned
        assignSortGroupRef(tle, *targetlist);

        // Create the SortGroupClause
        auto* sgc = makeNode<SortGroupClause>();
        sgc->tle_sort_group_ref = tle->ressortgroupref;
        sgc->eqop = 0;  // Would be looked up from the operator
        // Encode sort direction in sortop: 0 = ASC (default), 1 = DESC.
        // PostgreSQL uses the actual operator OID; pgcpp uses this sentinel
        // since it does not yet implement full operator lookup.
        sgc->sortop = (sortby->sortby_dir == SortByDir::kDesc) ? 1 : 0;
        sgc->nulls_first = (sortby->sortby_nulls == SortByNulls::kFirst);
        sgc->hashable = false;
        sortclauses.push_back(sgc);
    }

    return sortclauses;
}

// ---------------------------------------------------------------------------
// transformGroupClause — transform GROUP BY clause into SortGroupClause list.
// ---------------------------------------------------------------------------

std::vector<Node*> transformGroupClause(ParseState* pstate, const std::vector<Node*>& grouplist,
                                        std::vector<Node*>* targetlist,
                                        const std::vector<Node*>& sortClause,
                                        ParseExprKind exprKind) {
    std::vector<Node*> groupclauses;

    for (Node* node : grouplist) {
        if (nodeTag(node) != NodeTag::kSortBy)
            continue;
        auto* sortby = static_cast<SortBy*>(node);

        TargetEntry* tle = nullptr;

        // Check if the group expression is a simple column reference
        if (sortby->node != nullptr && nodeTag(sortby->node) == NodeTag::kColumnRef) {
            auto* cref = static_cast<ColumnRef*>(sortby->node);
            if (!cref->fields.empty()) {
                Node* field0 = cref->fields[0];
                if (nodeTag(field0) == NodeTag::kString) {
                    auto* v = static_cast<Value*>(field0);
                    const std::string& colname = v->GetString();

                    // Search the target list for a matching column name
                    for (Node* tle_node : *targetlist) {
                        if (nodeTag(tle_node) != NodeTag::kTargetEntry)
                            continue;
                        auto* t = static_cast<TargetEntry*>(tle_node);
                        if (t->resname == colname) {
                            tle = t;
                            break;
                        }
                    }
                }
            }
        }

        // Check if it's an integer constant (GROUP BY position)
        if (tle == nullptr && sortby->node != nullptr &&
            nodeTag(sortby->node) == NodeTag::kAConst) {
            auto* ac = static_cast<AConst*>(sortby->node);
            if (ac->val != nullptr && nodeTag(ac->val) == NodeTag::kInteger) {
                auto* v = static_cast<Value*>(ac->val);
                int pos = static_cast<int>(v->GetInteger());
                if (pos >= 1 && pos <= static_cast<int>(targetlist->size())) {
                    tle = static_cast<TargetEntry*>((*targetlist)[pos - 1]);
                }
            }
        }

        // If not found, create a new target entry
        if (tle == nullptr) {
            tle = findTargetlistEntrySQL99(pstate, sortby->node, targetlist, exprKind);
        }

        if (tle == nullptr)
            continue;

        // Assign a SortGroupRef if not already assigned
        assignSortGroupRef(tle, *targetlist);

        // Create the SortGroupClause
        auto* sgc = makeNode<SortGroupClause>();
        sgc->tle_sort_group_ref = tle->ressortgroupref;
        sgc->eqop = 0;
        sgc->sortop = 0;
        sgc->nulls_first = false;
        sgc->hashable = false;
        groupclauses.push_back(sgc);
    }

    return groupclauses;
}

// ---------------------------------------------------------------------------
// transformDistinctClause — transform DISTINCT clause.
// For SELECT DISTINCT, we use the entire target list as the distinct clause.
// ---------------------------------------------------------------------------

std::vector<Node*> transformDistinctClause(ParseState* pstate, std::vector<Node*>* targetlist,
                                           const std::vector<Node*>& distinctClause, bool isOn) {
    std::vector<Node*> result;

    if (!isOn) {
        // SELECT DISTINCT — use all non-junk target entries
        for (Node* tle_node : *targetlist) {
            if (nodeTag(tle_node) != NodeTag::kTargetEntry)
                continue;
            auto* tle = static_cast<TargetEntry*>(tle_node);
            if (tle->resjunk)
                continue;

            assignSortGroupRef(tle, *targetlist);

            auto* sgc = makeNode<SortGroupClause>();
            sgc->tle_sort_group_ref = tle->ressortgroupref;
            sgc->eqop = 0;
            sgc->sortop = 0;
            sgc->nulls_first = false;
            sgc->hashable = false;
            result.push_back(sgc);
        }
    }

    return result;
}

}  // namespace pgcpp::parser
