// cost.h — Cost estimation API for the optimizer.
//
// Converted from PostgreSQL 15's src/include/optimizer/cost.h.
//
// Provides functions to estimate the execution cost of different plan
// operations (sequential scan, index scan, sort, aggregate). The optimizer
// uses these estimates to select the cheapest access path.
#pragma once

#include "mytoydb/optimizer/path.hpp"

namespace mytoydb::optimizer {

// Cost constants (mirrors PostgreSQL's costsize.c defaults).
constexpr Cost kSeqPageCost = 1.0;          // cost of a sequential page fetch
constexpr Cost kRandomPageCost = 4.0;       // cost of a random page fetch
constexpr Cost kCpuTupleCost = 0.01;        // cost of processing a tuple
constexpr Cost kCpuIndexTupleCost = 0.005;  // cost of processing an index tuple
constexpr Cost kOperatorCost = 0.0025;      // cost of executing an operator
constexpr Cost kParallelTupleCost = 0.1;
constexpr int kDefaultPageRows = 100;  // rows per page (heuristic)

// Cost estimation functions.
// Each takes the relevant parameters and fills in the Path's cost fields.

// Estimate cost of a sequential scan.
void CostSeqScan(SeqScanPath* path, int pages, int tuples);

// Estimate cost of an index scan.
void CostIndexScan(IndexPath* path, int tuples, Selectivity selectivity);

// Estimate cost of a sort.
Cost CostSort(int tuples, int width, int64_t limit);

// Estimate cost of an aggregate (plain or grouped).
Cost CostAgg(int input_rows, int num_groups, int width);

// Clamp a row estimate to a reasonable range.
Cardinality ClampRowEst(Cardinality rows);

// Estimate selectivity of a simple equality qual (col = const).
// Returns a fraction between 0.0 and 1.0.
Selectivity EstimateSelectivity(const mytoydb::parser::Node* qual, int total_rows);

}  // namespace mytoydb::optimizer
