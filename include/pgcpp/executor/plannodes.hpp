// plannodes.h — Plan node type definitions for the executor.
//
// Converted from PostgreSQL 15's src/include/nodes/plannodes.h.
//
// Plan nodes describe the execution strategy chosen by the planner.
// Each plan node corresponds to a physical operator (sequential scan,
// aggregate, sort, join, etc.) and carries the parameters needed to
// execute it.
//
// In PostgreSQL, Plan is a Node subclass identified by NodeTag. In
// MyToyDB we use a PlanType enum field for identification and C++
// inheritance for structure sharing. The executor dispatches on
// PlanType in ExecInitNode to create the matching PlanState.
#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/parser/primnodes.hpp"

namespace mytoydb::executor {

// PlanType — identifies the kind of plan node.
enum class PlanType {
    kResult,       // no FROM (e.g., SELECT 1)
    kSeqScan,      // sequential heap scan
    kIndexScan,    // B-tree index scan
    kAgg,          // aggregate (COUNT/SUM/AVG/MIN/MAX)
    kSort,         // sort with optional Top-N
    kNestLoop,     // nested-loop join
    kHashJoin,     // hash join
    kHash,         // hash table build (inner child of HashJoin)
    kModifyTable,  // INSERT/UPDATE/DELETE
    // --- Task 15.14 additions ---
    kLimit,         // LIMIT/OFFSET
    kAppend,        // UNION ALL (iterate over multiple children)
    kMaterial,      // materialize child output in memory
    kUnique,        // deduplicate sorted input (SELECT DISTINCT)
    kSubqueryScan,  // scan a subquery in FROM
    kMergeJoin,     // merge join on sorted inputs
    kCteScan,       // scan a CTE
    kWindowAgg,     // window aggregate (OVER clause)
};

// Plan — base struct for all plan nodes.
//
// Mirrors PostgreSQL's Plan struct. Each plan node has:
//   - targetlist: list of TargetEntry describing the output columns
//   - qual:       optional filter predicate (WHERE clause)
//   - lefttree:   left child plan (outer)
//   - righttree:  right child plan (inner)
struct Plan {
    PlanType type;
    std::vector<mytoydb::parser::TargetEntry*> targetlist;
    mytoydb::parser::Node* qual = nullptr;
    Plan* lefttree = nullptr;
    Plan* righttree = nullptr;
    int plan_rows = 0;   // estimated number of output rows
    int plan_width = 0;  // estimated average row width

    virtual ~Plan() = default;
};

// Result — for queries with no FROM clause (e.g., SELECT 1).
struct Result : Plan {
    Result() { type = PlanType::kResult; }
};

// SeqScan — sequential scan of a heap relation.
struct SeqScan : Plan {
    SeqScan() { type = PlanType::kSeqScan; }
    int scanrelid = 0;  // 1-based range table index
};

// IndexScan — B-tree index scan.
struct IndexScan : Plan {
    IndexScan() { type = PlanType::kIndexScan; }
    int scanrelid = 0;                              // 1-based range table index
    mytoydb::catalog::Oid indexid = 0;              // index relation OID
    std::vector<mytoydb::parser::Node*> indexqual;  // index scan qualifiers
};

// Agg — aggregate node.
//
// Supports plain aggregation (no GROUP BY), sorted aggregation, and
// hashed aggregation. The aggregate functions are referenced by
// Aggref nodes in the target list.
struct Agg : Plan {
    enum class Strategy { kPlain, kSorted, kHashed };
    Agg() { type = PlanType::kAgg; }
    Strategy aggstrategy = Strategy::kPlain;
    std::vector<int> groupColIdx;                  // 1-based attr numbers of GROUP BY columns
    mytoydb::parser::Node* having_qual = nullptr;  // HAVING filter (evaluated per group)
};

// Sort — sort node with optional Top-N optimization.
//
// When limit > 0, only the top N rows are kept (Top-N heapsort).
// When offset > 0, the first `offset` sorted rows are skipped before output.
struct Sort : Plan {
    Sort() { type = PlanType::kSort; }
    std::vector<int> sortColIdx;                       // 1-based attr numbers to sort by
    std::vector<mytoydb::catalog::Oid> sortOperators;  // comparison operator OIDs
    std::vector<bool> nullsFirst;                      // NULLS FIRST/LAST per column
    std::vector<bool> reverse;                         // DESC per column
    int64_t limit = -1;                                // Top-N limit (-1 = no limit)
    int64_t offset = 0;                                // rows to skip after sort (0 = no skip)
};

// NestLoop — nested-loop join.
struct NestLoop : Plan {
    NestLoop() { type = PlanType::kNestLoop; }
    mytoydb::parser::JoinType jointype = mytoydb::parser::JoinType::kInner;
};

// HashJoin — hash join.
struct HashJoin : Plan {
    HashJoin() { type = PlanType::kHashJoin; }
    mytoydb::parser::JoinType jointype = mytoydb::parser::JoinType::kInner;
    std::vector<mytoydb::parser::Node*> hashclauses;  // hash join condition
};

// Hash — hash table build node (inner child of HashJoin).
struct Hash : Plan {
    Hash() { type = PlanType::kHash; }
};

// ModifyTable — INSERT/UPDATE/DELETE.
struct ModifyTable : Plan {
    ModifyTable() { type = PlanType::kModifyTable; }
    mytoydb::parser::CmdType operation = mytoydb::parser::CmdType::kInsert;
    int resultRelid = 0;  // 1-based range table index of target relation
};

// --- Task 15.14: new plan node types ---

// Limit — LIMIT/OFFSET applied to a child plan.
// Wraps the child and returns at most `limit_count` rows after skipping
// `offset_count` rows. limit_count < 0 means no limit.
struct Limit : Plan {
    Limit() { type = PlanType::kLimit; }
    int64_t limit_count = -1;  // max rows to return (-1 = unlimited)
    int64_t offset_count = 0;  // rows to skip before output
};

// Append — iterate over multiple child plans sequentially (UNION ALL).
// Uses append_plans (not lefttree/righttree) for the child list.
struct Append : Plan {
    Append() { type = PlanType::kAppend; }
    std::vector<Plan*> append_plans;  // child plans to iterate over
};

// Material — cache child output in memory for repeated scans.
struct Material : Plan {
    Material() { type = PlanType::kMaterial; }
};

// Unique — deduplicate a sorted input (SELECT DISTINCT).
// Compares consecutive tuples on uniq_colIdx columns; drops duplicates.
struct Unique : Plan {
    Unique() { type = PlanType::kUnique; }
    std::vector<int> uniq_colIdx;  // 1-based attr numbers to compare on
};

// SubqueryScan — scan a subquery in FROM.
// The subquery's plan is stored in lefttree.
struct SubqueryScan : Plan {
    SubqueryScan() { type = PlanType::kSubqueryScan; }
    int scanrelid = 0;  // 1-based range table index of the subquery RTE
};

// MergeJoin — join two sorted inputs by merging.
struct MergeJoin : Plan {
    MergeJoin() { type = PlanType::kMergeJoin; }
    mytoydb::parser::JoinType jointype = mytoydb::parser::JoinType::kInner;
    std::vector<mytoydb::parser::Node*> mergeclauses;  // merge join condition
};

// CteScan — scan a CTE result.
struct CteScan : Plan {
    CteScan() { type = PlanType::kCteScan; }
    int cte_id = 0;     // index into the CTE list (0-based)
    int scanrelid = 0;  // 1-based range table index
};

// WindowAgg — window aggregate (OVER clause).
// Computes aggregates over sliding partitions.
struct WindowAgg : Plan {
    WindowAgg() { type = PlanType::kWindowAgg; }
    std::vector<int> partColIdx;   // 1-based attr numbers of PARTITION BY columns
    std::vector<int> ordColIdx;    // 1-based attribute numbers of ORDER BY columns
    std::vector<bool> ordReverse;  // DESC per ORDER BY column
};

}  // namespace mytoydb::executor
