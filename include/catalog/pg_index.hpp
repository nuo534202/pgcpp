#pragma once

#include <cstdint>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::catalog {

// FormData_pg_index — C++ equivalent of PostgreSQL's catalog/pg_index.h.
//
// Each row describes one index. The index relation's OID is stored in
// indexrelid; the table the index is on is indrelid. Field names and
// semantics are preserved from PostgreSQL.

struct FormData_pg_index {
    Oid indexrelid = kInvalidOid;  // OID of the pg_class entry for this index
    Oid indrelid = kInvalidOid;    // OID of the relation we index
    int16_t indnatts = 0;          // number of columns in the index
    int16_t indnkeyatts = 0;       // number of key columns in the index
    bool indisunique = false;      // is this a unique index?
    bool indisprimary = false;     // is this the relation's primary key?
    bool indisexclusion = false;   // is this an exclusion constraint index?
    bool indisimmediate = false;   // enforce uniqueness immediately on insert?
    bool indisclustered = false;   // has clustered on this index?
    bool indisvalid = false;       // is the index logically valid?
    bool indcheckxmin = false;     // must wait for xmin to be old?
    bool indisready = false;       // is the index insert-ready?
    bool indislive = false;        // is the index live at all?
    bool indisreplident = false;   // use this index for logical replication identity?
    // indkey / indcollation / indclass / indoption are int2vectors in PG;
    // for pgcpp we keep them as std::vector<int16_t> / std::vector<Oid>.
    // To avoid pulling <vector> into the header (it's pulled in by catalog.hpp),
    // we use the already-included std::vector.
    std::vector<int16_t> indkey;     // column numbers (0 = attribute, attno otherwise)
    std::vector<Oid> indcollation;   // OIDs of collations for index columns
    std::vector<Oid> indclass;       // OID of opclass for each column
    std::vector<int16_t> indoption;  // per-column options (AM-specific bits)
    Oid indpred = kInvalidOid;       // OID of predicate expression tree (pg_node_tree)
    Oid indexprs = kInvalidOid;      // OID of expression tree for index cols (pg_node_tree)
};

using Form_pg_index = FormData_pg_index*;

}  // namespace pgcpp::catalog
