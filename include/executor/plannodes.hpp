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
// pgcpp we use a PlanType enum field for identification and C++
// inheritance for structure sharing. The executor dispatches on
// PlanType in ExecInitNode to create the matching PlanState.
#pragma once

#include <cstdint>
#include <vector>

#include "catalog/catalog.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "transaction/heap_tuple.hpp"  // ItemPointerData
#include "transaction/lock.hpp"        // RowLockStrength

namespace pgcpp::executor {

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
    // --- P1-7 additions ---
    kGroup,            // GROUP BY without aggregates (sorted input)
    kSetOp,            // INTERSECT / EXCEPT
    kMergeAppend,      // merge multiple sorted children (ORDER BY over UNION ALL)
    kBitmapIndexScan,  // build a TID bitmap from an index scan
    kBitmapHeapScan,   // fetch heap tuples by TID bitmap
    // --- P1-8 additions ---
    kLockRows,  // row-level locking (SELECT FOR UPDATE/SHARE)
    // --- P2-2 additions ---
    kValuesScan,       // scan a VALUES list
    kTidScan,          // scan by specific TIDs
    kFunctionScan,     // scan rows from a set-returning function
    kProjectSet,       // project target list with set-returning functions
    kMemoize,          // cache inner-side results keyed by parameters
    kIncrementalSort,  // sort by full keys, input presorted on prefix
    kRecursiveUnion,   // WITH RECURSIVE (seed ∪ recursive until fixpoint)
    kWorkTableScan,    // scan the recursive CTE working table
    kGather,           // parallel query: gather tuples from workers
    kGatherMerge,      // parallel query: gather and merge sorted tuples
    // --- P3-5 additions ---
    kForeignScan,  // scan a foreign table via FDW callbacks
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
    std::vector<pgcpp::parser::TargetEntry*> targetlist;
    pgcpp::parser::Node* qual = nullptr;
    Plan* lefttree = nullptr;
    Plan* righttree = nullptr;
    double startup_cost = 0.0;  // cost before first tuple (PG Cost)
    double total_cost = 0.0;    // total cost for all tuples (PG Cost)
    int plan_rows = 0;          // estimated number of output rows
    int plan_width = 0;         // estimated average row width

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
    int scanrelid = 0;                            // 1-based range table index
    pgcpp::catalog::Oid indexid = 0;              // index relation OID
    std::vector<pgcpp::parser::Node*> indexqual;  // index scan qualifiers
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
    std::vector<int> groupColIdx;                // 1-based attr numbers of GROUP BY columns
    pgcpp::parser::Node* having_qual = nullptr;  // HAVING filter (evaluated per group)
};

// Sort — sort node with optional Top-N optimization.
//
// When limit > 0, only the top N rows are kept (Top-N heapsort).
// When offset > 0, the first `offset` sorted rows are skipped before output.
struct Sort : Plan {
    Sort() { type = PlanType::kSort; }
    std::vector<int> sortColIdx;                     // 1-based attr numbers to sort by
    std::vector<pgcpp::catalog::Oid> sortOperators;  // comparison operator OIDs
    std::vector<bool> nullsFirst;                    // NULLS FIRST/LAST per column
    std::vector<bool> reverse;                       // DESC per column
    int64_t limit = -1;                              // Top-N limit (-1 = no limit)
    int64_t offset = 0;                              // rows to skip after sort (0 = no skip)
};

// NestLoop — nested-loop join.
struct NestLoop : Plan {
    NestLoop() { type = PlanType::kNestLoop; }
    pgcpp::parser::JoinType jointype = pgcpp::parser::JoinType::kInner;
};

// HashJoin — hash join.
struct HashJoin : Plan {
    HashJoin() { type = PlanType::kHashJoin; }
    pgcpp::parser::JoinType jointype = pgcpp::parser::JoinType::kInner;
    std::vector<pgcpp::parser::Node*> hashclauses;  // hash join condition
};

// Hash — hash table build node (inner child of HashJoin).
struct Hash : Plan {
    Hash() { type = PlanType::kHash; }
};

// ModifyTable — INSERT/UPDATE/DELETE.
struct ModifyTable : Plan {
    ModifyTable() { type = PlanType::kModifyTable; }
    pgcpp::parser::CmdType operation = pgcpp::parser::CmdType::kInsert;
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
    pgcpp::parser::JoinType jointype = pgcpp::parser::JoinType::kInner;
    std::vector<pgcpp::parser::Node*> mergeclauses;  // merge join condition
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

// --- P1-7: new plan node types ---

// Group — GROUP BY without aggregate functions.
//
// Assumes the child produces tuples already sorted on groupColIdx columns
// so identical groups are adjacent. Emits the first tuple of each group;
// subsequent tuples in the same group are skipped. This is the simple-form
// analogue of Agg with Strategy::kSorted and no Aggref in the target list.
struct Group : Plan {
    Group() { type = PlanType::kGroup; }
    std::vector<int> groupColIdx;  // 1-based attr numbers of GROUP BY columns
};

// SetOp — INTERSECT / EXCEPT execution.
//
// Takes a single sorted child (typically an Append of the two inputs with a
// flag column appended). The flag column (flagColIdx, 1-based) is 0 for rows
// from the left input and 1 for rows from the right input. Groups on
// colIdx columns and applies the set operation:
//   - INTERSECT: emit one row per group present in both inputs (DISTINCT: once;
//     ALL: min(leftCount, rightCount) copies).
//   - EXCEPT: emit one row per group present in left but not right (DISTINCT:
//     once if leftCount > 0 and rightCount == 0; ALL: leftCount - rightCount).
struct SetOp : Plan {
    enum class Cmd { kIntersect, kExcept };
    enum class Strategy { kSorted, kHashed };
    SetOp() { type = PlanType::kSetOp; }
    Cmd cmd = Cmd::kIntersect;
    Strategy strategy = Strategy::kSorted;
    bool all = false;         // ALL (preserve duplicates) vs DISTINCT
    std::vector<int> colIdx;  // 1-based attr numbers to compare on
    int flagColIdx = 0;       // 1-based attr number of the flag column
    int firstFlag = 0;        // flag value of the first (left) input
};

// MergeAppend — merge multiple sorted children into one sorted stream.
//
// Like Append, but all children must produce tuples sorted on the merge keys.
// A k-way merge (using a heap) combines them into a single sorted output.
// Used for ORDER BY over UNION ALL when each child is already sorted.
struct MergeAppend : Plan {
    MergeAppend() { type = PlanType::kMergeAppend; }
    std::vector<Plan*> merge_plans;                  // child plans (sorted)
    std::vector<int> sortColIdx;                     // 1-based attr numbers to sort by
    std::vector<pgcpp::catalog::Oid> sortOperators;  // comparison operator OIDs
    std::vector<bool> nullsFirst;                    // NULLS FIRST/LAST per column
    std::vector<bool> reverse;                       // DESC per column
};

// BitmapIndexScan — build a TID bitmap from an index scan.
//
// Scans a B-tree index using the index quals and collects all matching TIDs
// into a bitmap (vector of ItemPointerData). The bitmap is consumed by the
// parent BitmapHeapScan. This node produces no tuples itself; its output is
// the shared TID list.
struct BitmapIndexScan : Plan {
    BitmapIndexScan() { type = PlanType::kBitmapIndexScan; }
    int scanrelid = 0;                            // 1-based range table index
    pgcpp::catalog::Oid indexid = 0;              // index relation OID
    std::vector<pgcpp::parser::Node*> indexqual;  // index scan qualifiers
};

// BitmapHeapScan — fetch heap tuples by TID bitmap.
//
// The lefttree must be a BitmapIndexScan. This node reads the TID bitmap
// from the child, fetches each heap tuple by TID, applies the residual qual,
// and projects the target list.
struct BitmapHeapScan : Plan {
    BitmapHeapScan() { type = PlanType::kBitmapHeapScan; }
    int scanrelid = 0;  // 1-based range table index of the heap relation
};

// --- P1-8: row-level locking ---

// LockRows — row-level locking for SELECT FOR UPDATE/SHARE.
//
// Wraps a child plan. For each tuple produced by the child, acquires a
// row lock of the specified strength on the underlying heap tuple by
// calling heap_lock_tuple. The tuple is then passed through to the parent
// unchanged.
//
// In single-process pgcpp, the lock always succeeds (no blocking).
struct LockRows : Plan {
    LockRows() { type = PlanType::kLockRows; }
    int lockRelid = 0;  // 1-based range table index of the relation to lock
    pgcpp::transaction::RowLockStrength lockStrength =
        pgcpp::transaction::RowLockStrength::kForUpdate;
};

// --- P2-2: new plan node types ---

// ValuesScan — scan a VALUES list.
//
// Each row in `rows` is a list of expression nodes (one per output column).
// The executor evaluates each row's expressions to produce output tuples.
struct ValuesScan : Plan {
    ValuesScan() { type = PlanType::kValuesScan; }
    std::vector<std::vector<pgcpp::parser::Node*>> rows;  // one expr-list per row
    int scanrelid = 0;                                    // 1-based range table index
};

// TidScan — scan a heap relation by specific TIDs.
//
// Fetches each tuple at the listed TIDs directly (by block+offset), applies
// the qual filter, and projects the target list.
struct TidScan : Plan {
    TidScan() { type = PlanType::kTidScan; }
    int scanrelid = 0;  // 1-based range table index
    std::vector<pgcpp::transaction::ItemPointerData> tids;
};

// FunctionScan — scan rows produced by set-returning functions.
//
// Each entry in `functions` is a FuncExpr that returns a set (e.g.
// generate_series). When funcordinality is true, a row number column is
// appended (pgcpp simplification: ordinality not implemented).
struct FunctionScan : Plan {
    FunctionScan() { type = PlanType::kFunctionScan; }
    int scanrelid = 0;                            // 1-based range table index
    std::vector<pgcpp::parser::Node*> functions;  // FuncExpr nodes (SRFs)
    bool funcordinality = false;
};

// ProjectSet — project a target list containing set-returning functions.
//
// For each input tuple (from lefttree), evaluates the target list. SRF
// expressions may produce multiple values; the node returns one output row
// per combination. When a target entry is not an SRF, its value repeats for
// each SRF-produced row.
struct ProjectSet : Plan {
    ProjectSet() { type = PlanType::kProjectSet; }
};

// Memoize — cache inner-side lookup results keyed by parameter values.
//
// Used above a parameterized inner scan in a Nested Loop. `param_exprs` are
// evaluated in the outer tuple's context to form the lookup key. On a hit,
// cached rows are replayed; on a miss, the child is executed and its output
// cached under the new key.
struct Memoize : Plan {
    Memoize() { type = PlanType::kMemoize; }
    std::vector<pgcpp::parser::Node*> param_exprs;  // key expressions (outer ctx)
};

// IncrementalSort — sort by full sort keys, exploiting an existing prefix sort.
//
// `presortedColIdx` lists the 1-based attribute numbers already sorted in the
// input. The node reads runs of tuples sharing the same prefix values, fully
// sorts each run on the remaining keys, and emits the sorted runs in order.
struct IncrementalSort : Plan {
    IncrementalSort() { type = PlanType::kIncrementalSort; }
    std::vector<int> presortedColIdx;                // already-sorted prefix keys
    std::vector<int> sortColIdx;                     // full sort keys (1-based)
    std::vector<pgcpp::catalog::Oid> sortOperators;  // comparison operator OIDs
    std::vector<bool> nullsFirst;                    // NULLS FIRST/LAST per column
    std::vector<bool> reverse;                       // DESC per column
};

// RecursiveUnion — WITH RECURSIVE execution.
//
// lefttree is the seed (non-recursive) term; righttree is the recursive term.
// On each iteration the recursive term is re-scanned with the current working
// table as input. New rows are added to the result and become the next
// working table. Iteration stops when the recursive term produces no new rows.
// `wtParam` identifies the working table that WorkTableScan nodes inside the
// recursive term read from.
struct RecursiveUnion : Plan {
    RecursiveUnion() { type = PlanType::kRecursiveUnion; }
    int wtParam = 0;               // working-table id (links to WorkTableScan)
    std::vector<int> regenColIdx;  // 1-based attr numbers for duplicate removal
};

// WorkTableScan — scan the working table of a RecursiveUnion.
//
// Reads tuples from the working table registered under `wtParam` on the
// EState. The RecursiveUnion owning that wtParam populates the working table
// before each recursive iteration.
struct WorkTableScan : Plan {
    WorkTableScan() { type = PlanType::kWorkTableScan; }
    int wtParam = 0;    // working-table id (matches a RecursiveUnion)
    int scanrelid = 0;  // 1-based range table index
};

// Gather — parallel query leader node.
//
// Runs the child plan (lefttree) and returns its tuples. In pgcpp's
// single-process model (std::thread is forbidden), workers are not launched;
// the leader executes the child directly (nworkers = 0 path), matching
// PostgreSQL's serial fallback.
struct Gather : Plan {
    Gather() { type = PlanType::kGather; }
    int num_workers = 0;  // requested workers (0 = serial)
};

// GatherMerge — parallel query leader node that merges sorted worker output.
//
// Like Gather, but assumes each worker produces sorted output and merges the
// streams. In single-process pgcpp the child is executed directly and its
// output is returned (already sorted by the child).
struct GatherMerge : Plan {
    GatherMerge() { type = PlanType::kGatherMerge; }
    int num_workers = 0;                             // requested workers (0 = serial)
    std::vector<int> sortColIdx;                     // 1-based attr numbers to sort by
    std::vector<pgcpp::catalog::Oid> sortOperators;  // comparison operator OIDs
    std::vector<bool> nullsFirst;                    // NULLS FIRST/LAST per column
    std::vector<bool> reverse;                       // DESC per column
};

// --- P3-5: Foreign Data Wrapper scan ---

// ForeignScan — scan a foreign table via FDW callbacks.
//
// The executor looks up the foreign table's server OID in the FDW catalog,
// resolves the FDW handler name (e.g., "file_fdw"), and calls the handler's
// BeginForeignScan / IterateForeignScan / ReScanForeignScan / EndForeignScan
// callbacks. The handler stores its private state in ForeignScanState::fdw_state.
struct ForeignScan : Plan {
    ForeignScan() { type = PlanType::kForeignScan; }
    pgcpp::catalog::Oid fs_relid = pgcpp::catalog::kInvalidOid;  // foreign table pg_class OID
    int scanrelid = 0;                                           // 1-based range table index
};

}  // namespace pgcpp::executor
