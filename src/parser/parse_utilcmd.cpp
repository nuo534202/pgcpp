// parse_utilcmd.cpp — DDL parse-analysis entry points.
//
// Converted from PostgreSQL 15's src/backend/parser/parse_utilcmd.c.
//
// Implements transformCreateStmt / transformAlterTableStmt / transformIndexStmt.
// These functions validate types, cook column DEFAULT and CHECK constraint
// expressions, and reject duplicate column names at parse-analysis time.
// The transformed statements are still wrapped as CMD_UTILITY Query nodes;
// the actual catalog/storage side effects happen later in ProcessUtility.
#include "pgcpp/parser/parse_utilcmd.hpp"

#include <string>
#include <unordered_set>
#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_type.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parse_expr.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/parse_relation.hpp"
#include "pgcpp/parser/parse_type.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"

namespace pgcpp::parser {

using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::nodes::NodeTag;
using pgcpp::nodes::nodeTag;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Extract the type name string from a TypeName node (last component of the
// qualified name list).
std::string ExtractTypeNameString(TypeName* type_name) {
    if (type_name == nullptr || type_name->names.empty())
        return "";
    Node* last = type_name->names.back();
    if (last->GetTag() == NodeTag::kString) {
        auto* v = static_cast<pgcpp::nodes::Value*>(last);
        return v->GetString();
    }
    return "";
}

// Resolve and set the type_oid on a TypeName. Returns the resolved OID
// (or ereports ERROR if the type does not exist).
Oid ResolveAndSetTypeOid(TypeName* type_name) {
    if (type_name == nullptr)
        return kInvalidOid;
    std::string type_str = ExtractTypeNameString(type_name);
    Oid type_oid = typenameTypeId(type_str);
    if (type_oid == kInvalidOid) {
        ereport(pgcpp::error::LogLevel::kError, "type \"" + type_str + "\" does not exist");
    }
    type_name->type_oid = type_oid;
    return type_oid;
}

// Build a synthetic subquery RTE for the table being defined, so that
// column references inside CHECK constraints can be resolved. The RTE
// contains one TargetEntry per column with the correct type OID, mirroring
// how PostgreSQL makes the new table's columns visible during transform.
RangeTblEntry* BuildSyntheticRTEForNewTable(CreateStmt* stmt) {
    auto* rte = makeNode<RangeTblEntry>();
    rte->rtekind = RTEKind::kSubquery;
    rte->subquery = makeNode<Query>();
    rte->subquery->command_type = CmdType::kSelect;

    auto* eref = makeNode<Alias>();
    eref->aliasname = (stmt->relation != nullptr) ? stmt->relation->relname : "new_table";

    int attnum = 1;
    for (Node* elt : stmt->table_elts) {
        if (elt == nullptr || elt->GetTag() != NodeTag::kColumnDef)
            continue;
        auto* cd = static_cast<ColumnDef*>(elt);

        // Skip columns whose type we have not resolved yet — they will be
        // resolved by the caller before CHECK constraints are cooked.
        Oid type_oid = (cd->type_name != nullptr) ? cd->type_name->type_oid : kInvalidOid;

        auto* te = makeNode<TargetEntry>();
        te->resname = cd->colname;
        te->expr = makeNullConst(type_oid, -1, 0);
        te->resno = attnum;
        rte->subquery->target_list.push_back(te);

        eref->colnames.push_back(pgcpp::nodes::makeString(cd->colname));
        ++attnum;
    }
    rte->eref = eref;

    return rte;
}

// Add a RangeTblEntry to pstate->p_rtable and create a namespace item for
// it so its columns are visible to expression transformation.
void AddRTEToNamespace(ParseState* pstate, RangeTblEntry* rte) {
    pstate->p_rtable.push_back(rte);
    int rtindex = static_cast<int>(pstate->p_rtable.size());

    auto* ns_item = makePallocNode<ParseNamespaceItem>();
    ns_item->p_rte = rte;
    ns_item->p_rtindex = rtindex;
    ns_item->p_names = rte->eref;
    ns_item->p_rel_visible = true;
    ns_item->p_cols_visible = true;
    ns_item->p_lateral_only = false;
    ns_item->p_lateral_ok = true;
    pstate->p_namespace.push_back(ns_item);
}

// Process a single ColumnDef: resolve type, mark NOT NULL, cook DEFAULT
// and CHECK constraints. The synthetic RTE must already be in the
// namespace so CHECK constraints can reference columns by name.
void ProcessColumnDef(ParseState* pstate, ColumnDef* coldef) {
    if (coldef == nullptr)
        return;

    // 1. Resolve the column type.
    ResolveAndSetTypeOid(coldef->type_name);

    // 2. Walk the column-level constraints (stored as DefElems by the grammar).
    for (Node* cn : coldef->constraints) {
        if (cn == nullptr || cn->GetTag() != NodeTag::kDefElem)
            continue;
        auto* de = static_cast<DefElem*>(cn);

        if (de->defname == "not_null") {
            coldef->is_not_null = true;
        } else if (de->defname == "default") {
            // Cook the default expression.
            if (de->arg != nullptr) {
                coldef->cooked_default =
                    transformExpr(pstate, de->arg, ParseExprKind::kColumnDefault);
            }
        } else if (de->defname == "check") {
            // Cook the CHECK expression in place.
            if (de->arg != nullptr) {
                de->arg = transformExpr(pstate, de->arg, ParseExprKind::kCheckConstraint);
            }
        }
        // primary_key / unique / references are recorded but not transformed
        // here — they are enforced at execution time.
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// transformCreateStmt
// ---------------------------------------------------------------------------

Query* transformCreateStmt(ParseState* pstate, CreateStmt* stmt) {
    if (stmt == nullptr) {
        auto* qry = makeNode<Query>();
        qry->command_type = CmdType::kUtility;
        qry->can_set_tag = true;
        return qry;
    }

    // 1. Reject duplicate column names early. PostgreSQL checks this in
    //    transformColumnDefinitions; we do it here for parity.
    std::unordered_set<std::string> seen;
    for (Node* elt : stmt->table_elts) {
        if (elt == nullptr || elt->GetTag() != NodeTag::kColumnDef)
            continue;
        auto* cd = static_cast<ColumnDef*>(elt);
        if (!seen.insert(cd->colname).second) {
            ereport(pgcpp::error::LogLevel::kError,
                    "column \"" + cd->colname + "\" specified more than once");
        }
    }

    // 2. Resolve each column's type first, so the synthetic RTE we build
    //    next carries the correct type OIDs for column references.
    for (Node* elt : stmt->table_elts) {
        if (elt == nullptr || elt->GetTag() != NodeTag::kColumnDef)
            continue;
        auto* cd = static_cast<ColumnDef*>(elt);
        ResolveAndSetTypeOid(cd->type_name);
    }

    // 3. Build a synthetic RTE for the table being defined, so CHECK
    //    constraints can reference its columns.
    RangeTblEntry* synth_rte = BuildSyntheticRTEForNewTable(stmt);
    AddRTEToNamespace(pstate, synth_rte);

    // 4. Process each column: NOT NULL, DEFAULT, CHECK.
    for (Node* elt : stmt->table_elts) {
        if (elt == nullptr || elt->GetTag() != NodeTag::kColumnDef)
            continue;
        ProcessColumnDef(pstate, static_cast<ColumnDef*>(elt));
    }

    // 5. Wrap as CMD_UTILITY.
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kUtility;
    qry->utility_stmt = stmt;
    qry->can_set_tag = true;
    return qry;
}

// ---------------------------------------------------------------------------
// transformAlterTableStmt
// ---------------------------------------------------------------------------

Query* transformAlterTableStmt(ParseState* pstate, AlterTableStmt* stmt) {
    if (stmt == nullptr) {
        auto* qry = makeNode<Query>();
        qry->command_type = CmdType::kUtility;
        qry->can_set_tag = true;
        return qry;
    }

    // For each subcommand, transform ADD COLUMN by reusing the CREATE TABLE
    // column-processing logic. Other subcommands (DROP, RENAME, ALTER) need
    // no parse-time type resolution.
    for (Node* cmd_node : stmt->cmds) {
        if (cmd_node == nullptr || cmd_node->GetTag() != NodeTag::kAlterTableCmd)
            continue;
        auto* cmd = static_cast<AlterTableCmd*>(cmd_node);

        if (cmd->subtype == AlterTableType::kAddColumn ||
            cmd->subtype == AlterTableType::kAddColumnRecurse) {
            if (cmd->def != nullptr && cmd->def->GetTag() == NodeTag::kColumnDef) {
                // ADD COLUMN does not need a synthetic RTE — there's only one
                // column, so its CHECK constraint cannot reference other
                // columns of the table being altered (they are already in the
                // catalog and resolved at execution time).
                auto* coldef = static_cast<ColumnDef*>(cmd->def);
                ResolveAndSetTypeOid(coldef->type_name);
                for (Node* cn : coldef->constraints) {
                    if (cn == nullptr || cn->GetTag() != NodeTag::kDefElem)
                        continue;
                    auto* de = static_cast<DefElem*>(cn);
                    if (de->defname == "not_null") {
                        coldef->is_not_null = true;
                    } else if (de->defname == "default" && de->arg != nullptr) {
                        coldef->cooked_default =
                            transformExpr(pstate, de->arg, ParseExprKind::kColumnDefault);
                    }
                }
            }
        }
    }

    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kUtility;
    qry->utility_stmt = stmt;
    qry->can_set_tag = true;
    return qry;
}

// ---------------------------------------------------------------------------
// transformIndexStmt
// ---------------------------------------------------------------------------

Query* transformIndexStmt(ParseState* pstate, IndexStmt* stmt) {
    (void)pstate;  // No transformation needed for the basic case.
    auto* qry = makeNode<Query>();
    qry->command_type = CmdType::kUtility;
    qry->utility_stmt = stmt;
    qry->can_set_tag = true;
    return qry;
}

}  // namespace pgcpp::parser
