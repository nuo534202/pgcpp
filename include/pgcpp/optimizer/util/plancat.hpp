// plancat.h — Catalog lookup for the optimizer.
//
// Converted from PostgreSQL 15's src/include/optimizer/plancat.h and
// src/backend/optimizer/util/plancat.c.
//
// Provides the optimizer with relation size estimates (pages, tuples, width)
// by reading the system catalog (pg_class.relpages/reltuples and pg_attribute).
#pragma once

#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/planner.hpp"

namespace mytoydb::optimizer {

// get_relation_info — fill a RelOptInfo with catalog-derived statistics.
// Sets rel->pages, rel->tuples, and rel->width from pg_class and pg_attribute.
// If the relation is not in the catalog, conservative defaults are used.
void get_relation_info(PlannerInfo* root, mytoydb::catalog::Oid relation_oid, bool inhparent,
                       RelOptInfo* rel);

// estimate_rel_size — estimate a relation's page/tuple count.
// Simplified: uses pg_class.reltuples/relpages if available, else defaults.
void estimate_rel_size(mytoydb::catalog::Oid relation_oid, int* pages, int* tuples,
                       int* allvisfrac);

}  // namespace mytoydb::optimizer
