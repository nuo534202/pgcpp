// view.cpp — CREATE VIEW implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/view.c.
// Stores the view's query in pg_class (relkind 'v') without
// materializing data. pgcpp stores the SELECT query text on the
// view's pg_class entry so it can be retrieved for SELECT expansion
// (parser substitution rule).
#include "commands/view.hpp"

#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/analyze.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "parser/primnodes.hpp"
#include "rewrite/rewrite_handler.hpp"

namespace pgcpp::commands {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::catalog::RelPersistence;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::Node;
using pgcpp::parser::RawStmt;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::parser::ViewStmt;
using pgcpp::rewrite::StoreViewQuery;

std::string DefineView(ViewStmt* stmt) {
    if (stmt == nullptr || stmt->view == nullptr)
        return "";

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return "";

    const std::string& viewname = stmt->view->relname;

    if (cat->GetClassByName(viewname) != nullptr) {
        if (stmt->replace) {
            // CREATE OR REPLACE VIEW: drop the existing view first.
            // (Simplified: real PostgreSQL checks for compatibility.)
            const FormData_pg_class* existing = cat->GetClassByName(viewname);
            cat->DeleteClass(existing->oid);
            cat->DeleteAttributes(existing->oid);
        } else {
            ereport(pgcpp::error::LogLevel::kError, "relation \"" + viewname + "\" already exists");
        }
    }

    auto* class_row = makePallocNode<FormData_pg_class>();
    class_row->relname = viewname;
    class_row->relnamespace = 2200;
    class_row->relkind = RelKind::kView;
    class_row->relpersistence = RelPersistence::kPermanent;
    class_row->relnatts = 0;  // views don't have physical columns
    class_row->relispopulated = true;
    Oid relid = cat->InsertClass(class_row);
    class_row->relfilenode = relid;

    // Parse-analyze the view's SELECT query to produce a Query tree.
    // The Query tree is stored in memory by the rewrite system so that
    // SELECT * FROM <view> can expand it at query rewrite time.
    if (stmt->query != nullptr) {
        auto* raw = makePallocNode<RawStmt>();
        raw->stmt = stmt->query;
        std::vector<RawStmt*> raw_stmts{raw};
        std::vector<pgcpp::parser::Query*> queries =
            pgcpp::parser::parse_analyze(raw_stmts, nullptr);
        if (!queries.empty() && queries[0] != nullptr) {
            pgcpp::parser::Query* view_query = queries[0];
            StoreViewQuery(static_cast<int>(relid), view_query);

            // Create pg_attribute entries for each column in the view's
            // target list. This allows SELECT * FROM view to resolve
            // column names and types during parse analysis.
            int16_t attnum = 1;
            for (Node* te_node : view_query->target_list) {
                if (te_node == nullptr || te_node->GetTag() != NodeTag::kTargetEntry)
                    continue;
                auto* te = static_cast<TargetEntry*>(te_node);

                // Determine column name: use resname, or "?column?" if unset.
                std::string colname = te->resname;
                if (colname.empty())
                    colname = "?column?";

                // Determine type OID from the expression (Var has vartype).
                Oid typid = 0;
                int32_t typmod = -1;
                if (te->expr != nullptr && te->expr->GetTag() == NodeTag::kVar) {
                    auto* var = static_cast<Var*>(te->expr);
                    typid = var->vartype;
                    typmod = var->vartypmod;
                }

                auto* attr = makePallocNode<FormData_pg_attribute>();
                attr->attrelid = relid;
                attr->attname = colname;
                attr->atttypid = typid;
                attr->attnum = attnum;
                attr->atttypmod = typmod;
                attr->attislocal = true;
                cat->InsertAttribute(attr);
                attnum++;
            }

            // Update relnatts to the number of view columns.
            class_row->relnatts = attnum - 1;
        }
    }

    // Views have no physical storage — no RelationCreateStorage call.
    return "CREATE VIEW";
}

}  // namespace pgcpp::commands
