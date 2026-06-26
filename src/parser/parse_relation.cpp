// parse_relation.cpp — Range table and column resolution for parse analysis.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_relation.c.
// Implements addRangeTableEntry (for relations), column resolution
// (scanRTEForColumn, colNameToVar), and star expansion (expandRTE).
#include "mytoydb/parser/parse_relation.h"

#include <cstring>
#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_attribute.h"
#include "mytoydb/catalog/pg_class.h"
#include "mytoydb/catalog/syscache.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/parser/parse_coerce.h"
#include "mytoydb/parser/parse_type.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::parser {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::FormData_pg_class;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::GetSysCache;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::nodes::makePallocNode;
using mytoydb::nodes::Node;
using mytoydb::nodes::NodeTag;
using mytoydb::nodes::nodeTag;
using mytoydb::nodes::Value;

static constexpr Oid kUnknownOid = 705;

// ---------------------------------------------------------------------------
// buildRangeTblEntry — construct an RTE for a relation.
// ---------------------------------------------------------------------------

RangeTblEntry* buildRangeTblEntry(RangeVar* relation, Alias* alias, bool inh, bool in_from_cl) {
    auto* rte = makeNode<RangeTblEntry>();
    rte->rtekind = RTEKind::kRelation;
    rte->alias = alias;
    rte->inh = inh;
    rte->in_from_cl = in_from_cl;
    rte->rellockmode = 1;

    // Look up the relation in the catalog
    Oid relid = kInvalidOid;
    char relkind = 'r';

    if (GetCatalog() != nullptr) {
        const FormData_pg_class* rel = GetCatalog()->GetClassByName(relation->relname);
        if (rel != nullptr) {
            relid = rel->oid;
            relkind = static_cast<char>(rel->relkind);
        }
    }

    if (relid == kInvalidOid && GetSysCache() != nullptr) {
        const FormData_pg_class* rel = GetSysCache()->SearchClassByName(relation->relname, 0);
        if (rel != nullptr) {
            relid = rel->oid;
            relkind = static_cast<char>(rel->relkind);
        }
    }

    rte->relid = relid;
    rte->relkind = relkind;

    // Build the eref (expanded reference) — the table name and column names
    std::string refname = alias ? alias->aliasname : relation->relname;
    auto* eref = makeNode<Alias>();
    eref->aliasname = refname;

    // Get column names from the catalog
    if (relid != kInvalidOid && GetCatalog() != nullptr) {
        auto attrs = GetCatalog()->GetAttributes(relid);
        for (const auto* attr : attrs) {
            if (attr->attnum > 0) {  // skip system columns
                auto* colname_val = mytoydb::nodes::makeString(attr->attname);
                eref->colnames.push_back(colname_val);
            }
        }
    }

    rte->eref = eref;

    return rte;
}

// ---------------------------------------------------------------------------
// addRangeTableEntry — create an RTE for a relation and add it to pstate.
// ---------------------------------------------------------------------------

RangeTblEntry* addRangeTableEntry(ParseState* pstate, RangeVar* relation, Alias* alias, bool inh,
                                  bool in_from_cl, int* rtindex) {
    RangeTblEntry* rte = buildRangeTblEntry(relation, alias, inh, in_from_cl);

    // Add to the range table
    pstate->p_rtable.push_back(rte);
    *rtindex = static_cast<int>(pstate->p_rtable.size());

    return rte;
}

// ---------------------------------------------------------------------------
// addRangeTableEntryForSubquery — create an RTE for a subquery.
// ---------------------------------------------------------------------------

RangeTblEntry* addRangeTableEntryForSubquery(ParseState* pstate, Query* subquery, Alias* alias,
                                             bool lateral, bool in_from_cl, int* rtindex) {
    auto* rte = makeNode<RangeTblEntry>();
    rte->rtekind = RTEKind::kSubquery;
    rte->subquery = subquery;
    rte->alias = alias;
    rte->lateral = lateral;
    rte->in_from_cl = in_from_cl;
    rte->security_barrier = false;

    // Build eref from the subquery's target list
    auto* eref = makeNode<Alias>();
    eref->aliasname = alias ? alias->aliasname : "subquery";
    for (Node* tle_node : subquery->target_list) {
        if (nodeTag(tle_node) == NodeTag::kTargetEntry) {
            auto* tle = static_cast<TargetEntry*>(tle_node);
            if (!tle->resname.empty()) {
                eref->colnames.push_back(mytoydb::nodes::makeString(tle->resname));
            }
        }
    }
    rte->eref = eref;

    pstate->p_rtable.push_back(rte);
    *rtindex = static_cast<int>(pstate->p_rtable.size());

    return rte;
}

// ---------------------------------------------------------------------------
// refnameRangeTblEntry — find an RTE by alias name in the range table.
// ---------------------------------------------------------------------------

RangeTblEntry* refnameRangeTblEntry(ParseState* pstate, const char* refname, int* sublevels_up) {
    if (sublevels_up)
        *sublevels_up = 0;

    for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
        RangeTblEntry* rte = static_cast<RangeTblEntry*>(pstate->p_rtable[i]);
        if (rte->alias && rte->alias->aliasname == refname) {
            return rte;
        }
        if (rte->eref && rte->eref->aliasname == refname) {
            return rte;
        }
    }

    // Check parent parse states
    ParseState* parent = pstate->parent_parse_state;
    int level = 1;
    while (parent != nullptr) {
        for (size_t i = 0; i < parent->p_rtable.size(); ++i) {
            RangeTblEntry* rte = static_cast<RangeTblEntry*>(parent->p_rtable[i]);
            if (rte->alias && rte->alias->aliasname == refname) {
                if (sublevels_up)
                    *sublevels_up = level;
                return rte;
            }
            if (rte->eref && rte->eref->aliasname == refname) {
                if (sublevels_up)
                    *sublevels_up = level;
                return rte;
            }
        }
        parent = parent->parent_parse_state;
        ++level;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// scanRTEForColumn — search an RTE for a column matching the given name.
// ---------------------------------------------------------------------------

Node* scanRTEForColumn(ParseState* pstate, RangeTblEntry* rte, const std::string& colname,
                       int location) {
    if (rte == nullptr)
        return nullptr;

    // For relation RTEs, look up the column in the catalog
    if (rte->rtekind == RTEKind::kRelation && rte->relid != kInvalidOid) {
        if (GetCatalog() != nullptr) {
            const FormData_pg_attribute* attr = GetCatalog()->GetAttribute(rte->relid, 0);
            (void)attr;  // placeholder

            // Search through all attributes
            auto attrs = GetCatalog()->GetAttributes(rte->relid);
            for (const auto* a : attrs) {
                if (a->attnum > 0 && a->attname == colname) {
                    // Find the RTE's index in the range table
                    int rtindex = 0;
                    for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
                        if (pstate->p_rtable[i] == rte) {
                            rtindex = static_cast<int>(i + 1);
                            break;
                        }
                    }
                    return makeVar(rtindex, a->attnum, a->atttypid, a->atttypmod, 0, 0, location);
                }
            }
        }
        if (GetSysCache() != nullptr) {
            const FormData_pg_attribute* attr =
                GetSysCache()->SearchAttributeByName(rte->relid, colname);
            if (attr != nullptr && attr->attnum > 0) {
                int rtindex = 0;
                for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
                    if (pstate->p_rtable[i] == rte) {
                        rtindex = static_cast<int>(i + 1);
                        break;
                    }
                }
                Oid vartype = attr->atttypid;
                int vartypmod = attr->atttypmod;
                GetSysCache()->Release(attr);
                return makeVar(rtindex, attr->attnum, vartype, vartypmod, 0, 0, location);
            }
        }
    }

    // For subquery RTEs, look up the column in the subquery's target list
    if (rte->rtekind == RTEKind::kSubquery && rte->subquery != nullptr) {
        int attnum = 1;
        for (Node* tle_node : rte->subquery->target_list) {
            if (nodeTag(tle_node) == NodeTag::kTargetEntry) {
                auto* tle = static_cast<TargetEntry*>(tle_node);
                if (tle->resname == colname) {
                    int rtindex = 0;
                    for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
                        if (pstate->p_rtable[i] == rte) {
                            rtindex = static_cast<int>(i + 1);
                            break;
                        }
                    }
                    return makeVar(rtindex, attnum, exprType(tle->expr), exprTypmod(tle->expr), 0,
                                   0, location);
                }
            }
            ++attnum;
        }
    }

    // For JOIN RTEs, search the eref column names
    if (rte->rtekind == RTEKind::kJoin && rte->eref != nullptr) {
        int attnum = 1;
        for (Node* col_node : rte->eref->colnames) {
            if (nodeTag(col_node) == NodeTag::kString) {
                auto* v = static_cast<Value*>(col_node);
                if (v->GetString() == colname) {
                    int rtindex = 0;
                    for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
                        if (pstate->p_rtable[i] == rte) {
                            rtindex = static_cast<int>(i + 1);
                            break;
                        }
                    }
                    // For join columns, use the joinaliasvars if available
                    if (attnum <= static_cast<int>(rte->joinaliasvars.size())) {
                        Node* aliasvar = rte->joinaliasvars[attnum - 1];
                        if (aliasvar != nullptr) {
                            return copyObject(aliasvar);
                        }
                    }
                    return makeVar(rtindex, attnum, kUnknownOid, -1, 0, 0, location);
                }
            }
            ++attnum;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// colNameToVar — search the namespace for a column matching the given name.
// ---------------------------------------------------------------------------

Node* colNameToVar(ParseState* pstate, const std::string& colname, bool localonly,
                   int* sublevels_up) {
    if (sublevels_up)
        *sublevels_up = 0;

    for (ParseNamespaceItem* nsitem : pstate->p_namespace) {
        if (nsitem == nullptr || !nsitem->p_cols_visible)
            continue;

        Node* var = scanRTEForColumn(pstate, nsitem->p_rte, colname, -1);
        if (var != nullptr) {
            // Adjust varlevelsup if needed
            if (nodeTag(var) == NodeTag::kVar) {
                // varlevelsup is already 0 for current level
            }
            return var;
        }
    }

    // Check parent parse states
    if (!localonly) {
        ParseState* parent = pstate->parent_parse_state;
        int level = 1;
        while (parent != nullptr) {
            for (ParseNamespaceItem* nsitem : parent->p_namespace) {
                if (nsitem == nullptr || !nsitem->p_cols_visible)
                    continue;
                Node* var = scanRTEForColumn(parent, nsitem->p_rte, colname, -1);
                if (var != nullptr) {
                    if (nodeTag(var) == NodeTag::kVar) {
                        auto* v = static_cast<Var*>(var);
                        v->varlevelsup = level;
                    }
                    if (sublevels_up)
                        *sublevels_up = level;
                    return var;
                }
            }
            parent = parent->parent_parse_state;
            ++level;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// scanNameSpaceForColumn — search the namespace for an unqualified column.
// ---------------------------------------------------------------------------

Node* scanNameSpaceForColumn(ParseState* pstate, const std::string& colname, int location) {
    Node* result = nullptr;
    int count = 0;

    for (ParseNamespaceItem* nsitem : pstate->p_namespace) {
        if (nsitem == nullptr || !nsitem->p_cols_visible)
            continue;

        Node* var = scanRTEForColumn(pstate, nsitem->p_rte, colname, location);
        if (var != nullptr) {
            result = var;
            ++count;
        }
    }

    if (count > 1) {
        ereport(mytoydb::error::LogLevel::kError, "column reference is ambiguous");
    }

    return result;
}

// ---------------------------------------------------------------------------
// expandRTE — expand a range table entry into a list of Vars (for SELECT *).
// ---------------------------------------------------------------------------

std::vector<Node*> expandRTE(ParseState* pstate, RangeTblEntry* rte, int rtindex, int location) {
    std::vector<Node*> result;

    if (rte->rtekind == RTEKind::kRelation && rte->relid != kInvalidOid) {
        if (GetCatalog() != nullptr) {
            auto attrs = GetCatalog()->GetAttributes(rte->relid);
            for (const auto* attr : attrs) {
                if (attr->attnum > 0) {
                    auto* var = makeVar(rtindex, attr->attnum, attr->atttypid, attr->atttypmod, 0,
                                        0, location);
                    result.push_back(var);
                }
            }
        }
    } else if (rte->rtekind == RTEKind::kSubquery && rte->subquery != nullptr) {
        int attnum = 1;
        for (Node* tle_node : rte->subquery->target_list) {
            if (nodeTag(tle_node) == NodeTag::kTargetEntry) {
                auto* tle = static_cast<TargetEntry*>(tle_node);
                auto* var = makeVar(rtindex, attnum, exprType(tle->expr), exprTypmod(tle->expr), 0,
                                    0, location);
                result.push_back(var);
            }
            ++attnum;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// expandRelAttrs — expand a relation's attributes into Vars (for SELECT *).
// ---------------------------------------------------------------------------

std::vector<Node*> expandRelAttrs(ParseState* pstate, RangeTblEntry* rte, int rtindex,
                                  int location) {
    return expandRTE(pstate, rte, rtindex, location);
}

// ---------------------------------------------------------------------------
// addRTEToQuery — add an RTE to the namespace and joinlist.
// ---------------------------------------------------------------------------

void addRTEToQuery(ParseState* pstate, RangeTblEntry* rte, bool addToJoinList, bool addToNameSpace,
                   bool allowVLE) {
    int rtindex = 0;
    for (size_t i = 0; i < pstate->p_rtable.size(); ++i) {
        if (pstate->p_rtable[i] == rte) {
            rtindex = static_cast<int>(i + 1);
            break;
        }
    }

    if (addToNameSpace) {
        auto* nsitem = makePallocNode<ParseNamespaceItem>();
        nsitem->p_rte = rte;
        nsitem->p_rtindex = rtindex;
        nsitem->p_names = rte->alias ? rte->alias : rte->eref;
        nsitem->p_rel_visible = true;
        nsitem->p_cols_visible = true;
        nsitem->p_lateral_only = false;
        nsitem->p_lateral_ok = true;
        pstate->p_namespace.push_back(nsitem);
    }

    if (addToJoinList) {
        auto* rtr = makeNode<RangeTblRef>();
        rtr->rtindex = rtindex;
        pstate->p_joinlist.push_back(rtr);
    }
}

}  // namespace mytoydb::parser
