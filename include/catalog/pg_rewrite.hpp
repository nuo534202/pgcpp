#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_rewrite — C++ equivalent of PostgreSQL's catalog/pg_rewrite.h.
//
// Each row describes one rewrite rule (view expansion). A view's query tree
// is stored in ev_action as a node-tree string.

struct FormData_pg_rewrite {
    Oid oid = kInvalidOid;        // rewrite rule OID
    Oid ev_class = kInvalidOid;   // relation this rule is on
    std::string rulename;         // rule name (e.g. "_RETURN" for views)
    char ev_type = 0;             // event type: 1=SELECT, 2=UPDATE, 3=INSERT, 4=DELETE
    bool ev_enabled = true;       // is this rule enabled?
    bool is_instead = false;      // INSTEAD rule?
    Oid ev_qual = kInvalidOid;    // qualification expression tree (pg_node_tree)
    Oid ev_action = kInvalidOid;  // action expression tree (pg_node_tree)
};

using Form_pg_rewrite = FormData_pg_rewrite*;

}  // namespace pgcpp::catalog
