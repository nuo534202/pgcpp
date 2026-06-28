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
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::optimizer {

// Forward declaration — defined in util/restrictinfo.hpp. RestrictInfo wraps a
// qual clause with optimizer metadata. Forward-declared here so Path subclasses
// can hold vectors of RestrictInfo pointers without a full include dependency.
struct RestrictInfo;

// Forward declaration — defined below in this file. RelOptInfo holds
// per-relation optimizer state. Forward-declared here so Path can hold a
// back-pointer to its parent RelOptInfo.
struct RelOptInfo;

// Cost and cardinality types (mirrors PostgreSQL's Cost/Cardinality/Selectivity).
using Cost = double;
using Cardinality = double;
using Selectivity = double;

// Relids — a simplified bitmap of relation indexes (PG uses Bitmapset).
// Each element is a 1-based range table index. Defined here (rather than in
// util/restrictinfo.hpp) because RelOptInfo below needs it, and restrictinfo.hpp
// includes this file.
using Relids = std::vector<int>;

// PathType — identifies the kind of access path.
enum class PathType {
    kSeqScan,
    kIndexScan,
    kNestLoop,
    kHashJoin,
    kSort,
    kAgg,
    kResult,
    // --- Task 15.15: join + subquery path types ---
    kMergeJoin,     // merge join on sorted inputs
    kSubqueryScan,  // scan a subquery RTE in FROM
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
    // Back-pointer to the RelOptInfo this path belongs to (PG's `parent`).
    // Used by create_plan to look up the relindex and baserestrictinfo.
    RelOptInfo* parent_rel = nullptr;
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

// JoinPath — base class for join paths (NestLoop, HashJoin).
// Holds outer/inner subpaths and the join restrictlist.
struct JoinPath : Path {
    JoinPath() = default;
    Path* outer = nullptr;  // outer (left) subpath
    Path* inner = nullptr;  // inner (right) subpath
};

// NestLoopPath — nested-loop join path.
struct NestLoopPath : JoinPath {
    NestLoopPath() { type = PathType::kNestLoop; }
    std::vector<RestrictInfo*> restrictlist;  // join conditions
};

// HashJoinPath — hash join path.
struct HashJoinPath : JoinPath {
    HashJoinPath() { type = PathType::kHashJoin; }
    std::vector<mytoydb::parser::Node*> hashclauses;  // hash join clauses
};

// MergeJoinPath — merge join on sorted inputs (Task 15.15).
// Both outer and inner subpaths must be sorted on the merge clause's
// sort operator. The optimizer wraps each subpath in a SortPath if its
// pathkeys do not already satisfy the merge clause's ordering.
struct MergeJoinPath : JoinPath {
    MergeJoinPath() { type = PathType::kMergeJoin; }
    std::vector<mytoydb::parser::Node*> mergeclauses;  // merge join clauses
    mytoydb::parser::JoinType jointype = mytoydb::parser::JoinType::kInner;
};

// SubqueryScanPath — scan a subquery RTE in FROM (Task 15.15).
// Wraps the subquery's chosen path; used when a FROM-clause subquery
// cannot be flattened into the parent query.
struct SubqueryScanPath : Path {
    SubqueryScanPath() { type = PathType::kSubqueryScan; }
    Path* subpath = nullptr;                           // path for the subquery
    int scanrelid = 0;                                 // 1-based range table index
    std::vector<mytoydb::parser::TargetEntry*> tlist;  // output target list
};

// SortPath — sort path (wraps a subpath with a sort).
struct SortPath : Path {
    SortPath() { type = PathType::kSort; }
    Path* subpath = nullptr;                                  // the path to sort
    std::vector<mytoydb::parser::SortGroupClause*> pathkeys;  // sort keys
};

// AggPath — aggregate path (wraps a subpath with aggregation).
struct AggPath : Path {
    AggPath() { type = PathType::kAgg; }
    Path* subpath = nullptr;  // input path
    mytoydb::executor::Agg::Strategy aggstrategy =
        mytoydb::executor::Agg::Strategy::kPlain;      // plain/sorted/hashed
    std::vector<mytoydb::parser::Node*> group_clause;  // GROUP BY clauses
    int num_groups = 0;                                // estimated group count
};

// ResultPath — for queries with no FROM clause (e.g., SELECT 1).
struct ResultPath : Path {
    ResultPath() { type = PathType::kResult; }
    std::vector<mytoydb::parser::Node*> quals;  // one-time filter quals
};

// RelOptInfo — per-relation optimizer state.
//
// Mirrors PostgreSQL's RelOptInfo. Holds the range table entry, candidate
// paths, and the cheapest path selected. For MyToyDB's single-table
// ClickBench workload, join optimization is minimal.
//
// Task 15.15 adds `relids` (a multi-rel bitmap for join rels) and `pathkeys`
// (canonical pathkey list for the cheapest input path, used for merge join).
struct RelOptInfo {
    int relid = 0;                                  // relation OID
    int relindex = 0;                               // 1-based range table index
    mytoydb::parser::RangeTblEntry* rte = nullptr;  // range table entry
    std::vector<Path*> pathlist;                    // candidate paths
    Path* cheapest_path = nullptr;                  // cheapest path selected
    Cardinality rows = 0.0;                         // estimated row count
    int width = 0;                                  // estimated row width
    // --- P0 extensions (Task 15.3): PG-style restrictinfo and statistics ---
    std::vector<RestrictInfo*> baserestrictinfo;  // base-restriction clauses (WHERE)
    std::vector<RestrictInfo*> joininfo;          // join clauses involving this rel
    int pages = 0;                                // estimated relation pages
    int tuples = 0;                               // estimated relation tuples
    bool consider_startup = false;                // consider startup cost?
    // --- Task 15.15: relids bitmap for join rels (1-based RT indexes) ---
    Relids relids;  // base rels: {relindex}; join rels: union of children's relids
    // Pathkeys for the cheapest_path's sort order (empty = unsorted).
    std::vector<struct PathKey*> pathkeys;
};

}  // namespace mytoydb::optimizer
