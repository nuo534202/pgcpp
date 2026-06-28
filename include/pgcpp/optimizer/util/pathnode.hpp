// pathnode.h — Path construction factory functions.
//
// Converted from PostgreSQL 15's src/include/optimizer/pathnode.h and
// src/backend/optimizer/util/pathnode.c.
//
// Provides factory functions that allocate and initialize Path subclass
// objects (SeqScanPath, IndexPath, NestLoopPath, HashJoinPath, SortPath,
// AggPath, ResultPath) and utilities for managing a RelOptInfo's pathlist.
#pragma once

#include <vector>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/util/restrictinfo.hpp"
#include "pgcpp/parser/parsenodes.hpp"

namespace pgcpp::optimizer {

// Forward declaration — defined in pgcpp/optimizer/planner.hpp. Forward-
// declared here to avoid a circular include (planner.hpp → path.hpp → here).
struct PlannerInfo;

// create_seqscan_path — create a SeqScanPath for a base relation.
SeqScanPath* create_seqscan_path(PlannerInfo* root, RelOptInfo* rel);

// create_index_path — create an IndexPath for a base relation.
IndexPath* create_index_path(PlannerInfo* root, RelOptInfo* rel, pgcpp::catalog::Oid indexid,
                             std::vector<pgcpp::parser::Node*> indexclauses);

// create_nestloop_path — create a NestLoopPath for a join relation.
NestLoopPath* create_nestloop_path(PlannerInfo* root, RelOptInfo* joinrel, Path* outer, Path* inner,
                                   std::vector<RestrictInfo*> restrictlist);

// create_hashjoin_path — create a HashJoinPath for a join relation.
HashJoinPath* create_hashjoin_path(PlannerInfo* root, RelOptInfo* joinrel, Path* outer, Path* inner,
                                   std::vector<pgcpp::parser::Node*> hashclauses);

// create_mergejoin_path — create a MergeJoinPath for a join relation (Task 15.15).
// Caller is responsible for ensuring outer and inner paths are sorted on the
// merge clause's sort operator (typically by wrapping them in SortPaths).
MergeJoinPath* create_mergejoin_path(PlannerInfo* root, RelOptInfo* joinrel, Path* outer,
                                     Path* inner, std::vector<pgcpp::parser::Node*> mergeclauses,
                                     pgcpp::parser::JoinType jointype);

// create_subqueryscan_path — create a SubqueryScanPath wrapping a subquery's
// chosen path (Task 15.15). Used when a FROM-clause subquery cannot be
// flattened into the parent query.
SubqueryScanPath* create_subqueryscan_path(PlannerInfo* root, RelOptInfo* rel, Path* subpath,
                                           int scanrelid,
                                           std::vector<pgcpp::parser::TargetEntry*> tlist);

// create_sort_path — create a SortPath wrapping a subpath.
SortPath* create_sort_path(PlannerInfo* root, RelOptInfo* rel, Path* subpath,
                           std::vector<pgcpp::parser::SortGroupClause*> pathkeys);

// create_agg_path — create an AggPath wrapping a subpath.
AggPath* create_agg_path(PlannerInfo* root, RelOptInfo* rel, Path* subpath,
                         pgcpp::executor::Agg::Strategy aggstrategy,
                         std::vector<pgcpp::parser::Node*> group_clause, int num_groups);

// create_result_path — create a ResultPath (for no-FROM queries).
ResultPath* create_result_path(PlannerInfo* root, RelOptInfo* rel,
                               std::vector<pgcpp::parser::Node*> quals);

// add_path — add a candidate path to a relation's pathlist.
// Simplified: does not perform PG's dominator-based path pruning; just
// pushes the path and updates cheapest_path if appropriate.
void add_path(RelOptInfo* rel, Path* path);

// cheapest_path — return the cheapest path for a relation (by total_cost).
// Returns nullptr if the pathlist is empty.
Path* cheapest_path(RelOptInfo* rel);

}  // namespace pgcpp::optimizer
