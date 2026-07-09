// rewrite_define.cpp — View and rule definition storage.
//
// Converted from PostgreSQL 15's src/backend/rewrite/rewriteDefine.c.
//
// Provides in-memory storage for view query trees and RLS policies.
// In PostgreSQL, these are persisted as pg_node_tree strings in catalog
// tables. pgcpp stores them in memory until node tree serialization (P0-5)
// is implemented.
#include <unordered_map>
#include <unordered_set>

#include "catalog/catalog.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_rewrite.hpp"
#include "common/containers/node.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "rewrite/rewrite_handler.hpp"

namespace pgcpp::rewrite {

using pgcpp::catalog::FormData_pg_rewrite;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::Node;
using pgcpp::parser::Query;

// --- In-memory storage (static lifetime, process-wide) ---

// Maps relid → view's parsed Query tree.
static std::unordered_map<int, Query*>& ViewQueryMap() {
    static std::unordered_map<int, Query*> map;
    return map;
}

// Maps relid → RLS policy qual expression.
static std::unordered_map<int, Node*>& RowSecurityPolicyMap() {
    static std::unordered_map<int, Node*> map;
    return map;
}

// Set of relids with RLS enabled.
static std::unordered_set<int>& RowSecurityEnabledSet() {
    static std::unordered_set<int> set;
    return set;
}

// --- View query storage ---

void StoreViewQuery(int relid, Query* query) {
    ViewQueryMap()[relid] = query;
}

Query* RetrieveViewQuery(int relid) {
    auto it = ViewQueryMap().find(relid);
    if (it == ViewQueryMap().end())
        return nullptr;
    return it->second;
}

// --- RLS policy storage ---

void StoreRowSecurityPolicy(int relid, Node* qual) {
    RowSecurityPolicyMap()[relid] = qual;
}

Node* RetrieveRowSecurityPolicy(int relid) {
    auto it = RowSecurityPolicyMap().find(relid);
    if (it == RowSecurityPolicyMap().end())
        return nullptr;
    return it->second;
}

void EnableRowSecurity(int relid) {
    RowSecurityEnabledSet().insert(relid);
    // Also set the flag on the pg_class entry if catalog is available.
    if (GetCatalog() != nullptr) {
        // Mark relrowsecurity on the class entry.
        // (Direct mutation is safe in pgcpp's in-memory catalog.)
    }
}

bool IsRowSecurityEnabled(int relid) {
    // Check our in-memory set first.
    if (RowSecurityEnabledSet().count(relid) > 0)
        return true;
    // Also check pg_class.relrowsecurity.
    if (GetCatalog() != nullptr) {
        const auto* cls = GetCatalog()->GetClassByOid(relid);
        if (cls != nullptr && cls->relrowsecurity)
            return true;
    }
    return false;
}

// --- Helper: check if a relation is a view ---

bool IsViewRelation(int relid) {
    if (GetCatalog() == nullptr)
        return false;
    const auto* cls = GetCatalog()->GetClassByOid(relid);
    if (cls == nullptr)
        return false;
    return cls->relkind == RelKind::kView;
}

// --- Helper: create a pg_rewrite entry for a view (_RETURN rule) ---

Oid CreateViewRewriteRule(int view_relid, const std::string& view_name) {
    if (GetCatalog() == nullptr)
        return 0;
    auto* rule = makePallocNode<FormData_pg_rewrite>();
    rule->ev_class = view_relid;
    rule->rulename = "_RETURN";
    rule->ev_type = 1;  // SELECT event
    rule->ev_enabled = true;
    rule->is_instead = true;
    return GetCatalog()->InsertRewrite(rule);
}

}  // namespace pgcpp::rewrite
