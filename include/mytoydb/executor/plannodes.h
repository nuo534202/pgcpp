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

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/parser/parsenodes.h"
#include "mytoydb/parser/primnodes.h"

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

}  // namespace mytoydb::executor
