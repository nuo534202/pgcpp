// create_plan.h — Path-to-Plan translation.
//
// Converted from PostgreSQL 15's src/backend/optimizer/plan/createplan.c.
//
// Translates a chosen Path tree (the optimizer's output) into an executor
// Plan tree. Each Path subclass maps to a Plan subclass: SeqScanPath →
// SeqScan, AggPath → Agg, SortPath → Sort, etc. Join paths (NestLoop,
// HashJoin) are skeleton implementations for compile/test correctness.
#pragma once

#include <vector>

#include "executor/plannodes.hpp"
#include "optimizer/path.hpp"
#include "optimizer/planner.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::optimizer {

// create_plan — top-level Path→Plan translation.
// Dispatches on best_path->type to the appropriate builder. For upper nodes
// (Agg/Sort), recursively translates the subpath first.
pgcpp::executor::Plan* create_plan(PlannerInfo* root, Path* best_path);

// --- Individual builders (public for unit testing) ---

// create_seqscan_plan — build a SeqScan plan from a SeqScanPath.
// `tlist` is the scan's target list; `scan_clauses` are the WHERE quals
// (extracted from RelOptInfo->baserestrictinfo).
pgcpp::executor::SeqScan* create_seqscan_plan(PlannerInfo* root, SeqScanPath* path,
                                              std::vector<pgcpp::parser::TargetEntry*> tlist,
                                              std::vector<pgcpp::parser::Node*> scan_clauses);

// create_indexscan_plan — build an IndexScan plan from an IndexPath.
pgcpp::executor::IndexScan* create_indexscan_plan(PlannerInfo* root, IndexPath* path,
                                                  std::vector<pgcpp::parser::TargetEntry*> tlist,
                                                  std::vector<pgcpp::parser::Node*> scan_clauses);

// create_nestloop_plan — build a NestLoop plan (skeleton).
pgcpp::executor::NestLoop* create_nestloop_plan(PlannerInfo* root, NestLoopPath* path);

// create_hashjoin_plan — build a HashJoin plan (skeleton).
pgcpp::executor::HashJoin* create_hashjoin_plan(PlannerInfo* root, HashJoinPath* path);

// create_mergejoin_plan — build a MergeJoin plan (Task 15.15).
pgcpp::executor::MergeJoin* create_mergejoin_plan(PlannerInfo* root, MergeJoinPath* path);

// create_subqueryscan_plan — build a SubqueryScan plan (Task 15.15).
pgcpp::executor::SubqueryScan* create_subqueryscan_plan(PlannerInfo* root, SubqueryScanPath* path);

// create_result_plan — build a Result plan (for no-FROM queries).
pgcpp::executor::Result* create_result_plan(PlannerInfo* root, ResultPath* path);

// create_agg_plan — build an Agg plan from an AggPath.
pgcpp::executor::Agg* create_agg_plan(PlannerInfo* root, AggPath* path);

// create_sort_plan — build a Sort plan from a SortPath.
pgcpp::executor::Sort* create_sort_plan(PlannerInfo* root, SortPath* path);

}  // namespace pgcpp::optimizer
