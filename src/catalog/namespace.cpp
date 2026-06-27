// namespace.cpp — schema/namespace resolution.
//
// Converted from PostgreSQL 15's src/backend/catalog/namespace.c.
//
// MyToyDB has no schema concept yet; all relations live in a single implicit
// "public" namespace. schemaname on RangeVar is ignored.
#include "mytoydb/catalog/namespace.hpp"

#include <string>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_class.hpp"
#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::catalog {

using mytoydb::nodes::makePallocNode;
using mytoydb::parser::RangeVar;

namespace {

// Static "public" namespace name. Returned by get_namespace_name. Lifetime
// matches the program; safe to expose as const char*.
constexpr const char* kPublicNamespaceName = "public";

}  // namespace

Oid RangeVarGetRelid(RangeVar* rangevar, bool failOK) {
    if (rangevar == nullptr) {
        if (failOK) {
            return kInvalidOid;
        }
        ereport(mytoydb::error::LogLevel::kError, "RangeVarGetRelid: rangevar is null");
    }
    // MyToyDB ignores schemaname — single-namespace model.
    const std::string& relname = rangevar->relname;
    return RelnameGetRelid(relname, failOK);
}

Oid RelnameGetRelid(const std::string& relname, bool failOK) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        if (failOK) {
            return kInvalidOid;
        }
        ereport(mytoydb::error::LogLevel::kError, "RelnameGetRelid: catalog is not initialized");
    }
    const FormData_pg_class* row = cat->GetClassByName(relname);
    if (row == nullptr) {
        if (failOK) {
            return kInvalidOid;
        }
        ereport(mytoydb::error::LogLevel::kError, "relation \"" + relname + "\" does not exist");
    }
    return row->oid;
}

RangeVar* makeRangeVarFromNameList(const std::vector<std::string>& names) {
    if (names.empty()) {
        return nullptr;
    }
    auto* rv = makePallocNode<RangeVar>();
    // PG: last element is relname; if 2 elements, first is schemaname; if 3,
    // first is catalogname. MyToyDB only consumes relname.
    rv->relname = names.back();
    if (names.size() == 2) {
        rv->schemaname = names[0];
    } else if (names.size() >= 3) {
        rv->catalogname = names[0];
        rv->schemaname = names[1];
    }
    return rv;
}

const char* get_namespace_name(Oid /*nspoid*/) {
    // MyToyDB has a single implicit namespace; the OID is ignored.
    return kPublicNamespaceName;
}

}  // namespace mytoydb::catalog
