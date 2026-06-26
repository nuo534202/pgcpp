// path.h — Path types and RelOptInfo for the optimizer.
//
// Converted from PostgreSQL 15's src/include/nodes/relation.h.
//
// A Path represents a candidate way to access or combine data (sequential
// scan, index scan, join, etc.). The optimizer generates multiple Paths for
// each relation, estimates their cost, and selects the cheapest. The chosen
// Path is then converted into a Plan tree for the executor.
//
// RelOptInfo holds per-relation optimizer state: the range table entry,
// candidate path list, estimated row count, and the cheapest path selected.
#pragma once

#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::optimizer {

// Cost and cardinality types (mirrors PostgreSQL's Cost/Cardinality/Selectivity).
using Cost = double;
using Cardinality = double;
using Selectivity = double;

// PathType — identifies the kind of access path.
enum class PathType {
    kSeqScan,
    kIndexScan,
};

// Path — base class for all access paths.
//
// Each Path estimates the cost of producing rows: startup_cost is the cost
// before the first tuple can be returned; total_cost is the cost to return
// all rows. The optimizer selects the Path with the lowest total_cost
// (or lowest startup_cost among equal-cost paths).
struct Path {
    PathType type;
    int relid = 0;            // relation OID this path accesses
    Cost startup_cost = 0.0;  // cost before first tuple
    Cost total_cost = 0.0;    // total cost for all rows
    Cardinality rows = 0.0;   // estimated number of output rows
    int width = 0;            // estimated average row width in bytes
    virtual ~Path() = default;
};

// SeqScanPath — sequential heap scan path.
struct SeqScanPath : Path {
    SeqScanPath() { type = PathType::kSeqScan; }
};

// IndexPath — B-tree index scan path.
struct IndexPath : Path {
    IndexPath() { type = PathType::kIndexScan; }
    mytoydb::catalog::Oid indexid = 0;              // index relation OID
    std::vector<mytoydb::parser::Node*> indexqual;  // index scan qualifiers
};

// RelOptInfo — per-relation optimizer state.
//
// Mirrors PostgreSQL's RelOptInfo. Holds the range table entry, candidate
// paths, and the cheapest path selected. For MyToyDB's single-table
// ClickBench workload, join optimization is minimal.
struct RelOptInfo {
    int relid = 0;                                  // relation OID
    int relindex = 0;                               // 1-based range table index
    mytoydb::parser::RangeTblEntry* rte = nullptr;  // range table entry
    std::vector<Path*> pathlist;                    // candidate paths
    Path* cheapest_path = nullptr;                  // cheapest path selected
    Cardinality rows = 0.0;                         // estimated row count
    int width = 0;                                  // estimated row width
};

}  // namespace mytoydb::optimizer
