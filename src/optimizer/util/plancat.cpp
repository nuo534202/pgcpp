// plancat.cpp — Catalog lookup for the optimizer.
//
// Converted from PostgreSQL 15's src/backend/optimizer/util/plancat.c.
//
// Reads pg_class (relpages, reltuples) and pg_attribute (column count/width)
// to populate a RelOptInfo's size estimates. When the catalog has no data
// (e.g., synthetic OIDs in unit tests), conservative defaults are used.
#include "mytoydb/optimizer/util/plancat.hpp"

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/catalog/pg_attribute.hpp"
#include "mytoydb/catalog/pg_class.hpp"

namespace mytoydb::optimizer {
using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;

void estimate_rel_size(Oid relation_oid, int* pages, int* tuples, int* allvisfrac) {
    if (allvisfrac != nullptr)
        *allvisfrac = 0;  // simplified: assume no all-visible pages
    if (pages != nullptr)
        *pages = 10;  // default heuristic
    if (tuples != nullptr)
        *tuples = 1000;  // default heuristic
    if (relation_oid == 0 || GetCatalog() == nullptr)
        return;
    const mytoydb::catalog::FormData_pg_class* cls = GetCatalog()->GetClassByOid(relation_oid);
    if (cls == nullptr)
        return;
    if (pages != nullptr && cls->relpages > 0)
        *pages = cls->relpages;
    if (tuples != nullptr && cls->reltuples > 0.0F)
        *tuples = static_cast<int>(cls->reltuples);
}

void get_relation_info(PlannerInfo* root, Oid relation_oid, bool inhparent, RelOptInfo* rel) {
    (void)root;
    (void)inhparent;  // inheritance not supported in the simplified model
    int pages = 0;
    int tuples = 0;
    estimate_rel_size(relation_oid, &pages, &tuples, nullptr);
    rel->pages = pages;
    rel->tuples = tuples;
    // Estimate width from the sum of attribute lengths (if catalog has them).
    int width = 0;
    if (relation_oid != 0 && GetCatalog() != nullptr) {
        auto attrs = GetCatalog()->GetAttributes(relation_oid);
        for (const mytoydb::catalog::FormData_pg_attribute* attr : attrs) {
            if (attr->attnum < 1)
                continue;  // skip system columns
            // Use attlen if positive (fixed-length), else a small default.
            width += (attr->attlen > 0) ? attr->attlen : 8;
        }
    }
    rel->width = (width > 0) ? width : 24;  // default width if no attributes
    // Inherit rows estimate from tuples.
    rel->rows = static_cast<Cardinality>(tuples);
}

}  // namespace mytoydb::optimizer
