// indxpath.h — Index path generation for the optimizer.
//
// Converted from PostgreSQL 15's src/include/optimizer/path.h (index path
// generation section) and src/backend/optimizer/path/indxpath.c.
//
// Declares CreateIndexPaths, which generates IndexPath candidates for a
// base relation by matching the relation's B-tree indexes against the
// indexable clauses in the relation's baserestrictinfo.
#pragma once

#include "optimizer/path.hpp"

namespace pgcpp::optimizer {

// Forward declaration — defined in pgcpp/optimizer/planner.hpp.
struct PlannerInfo;

// CreateIndexPaths — generate IndexPath candidates for a base relation.
//
// For each B-tree index on `rel`, scans `rel->baserestrictinfo` for clauses
// of the form (Var op Const) where the Var's varattno matches the index's
// first key column (indkey[0]). For each matching clause, creates an
// IndexPath via create_index_path, estimates its selectivity and cost via
// CostIndexScan, and adds it to `rel->pathlist` via add_path.
//
// The cheapest path is updated automatically by add_path.
void CreateIndexPaths(PlannerInfo* root, RelOptInfo* rel);

}  // namespace pgcpp::optimizer
