// estate.h — EState (per-query executor state) and QueryDesc.
//
// Converted from PostgreSQL 15's src/include/nodes/execnodes.h (EState)
// and src/include/executor/execdesc.h (QueryDesc).
//
// EState holds per-query state shared across all plan nodes:
//   - the range table (from the Query)
//   - the snapshot for visibility checks
//   - the command ID for DML
//   - opened relations (for cleanup at ExecutorEnd)
//   - a per-query memory context
//
// QueryDesc ties together a Query, its Plan, the EState, and the
// top-level PlanState.
#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/executor/plannodes.hpp"
#include "mytoydb/executor/tupletable.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/transaction/snapshot.hpp"
#include "mytoydb/transaction/xact.hpp"

namespace mytoydb::executor {

class PlanState;

// EState — per-query executor state.
struct EState {
    // Range table from the Query (1-based indexing: rtindex 1 = es_range_table[0]).
    std::vector<mytoydb::parser::RangeTblEntry*> es_range_table;

    // Snapshot for visibility checks (NULL = use transaction snapshot).
    mytoydb::transaction::Snapshot es_snapshot = nullptr;

    // Command ID for DML operations.
    mytoydb::transaction::CommandId es_output_cid = 0;

    // Per-query memory context (all plan state allocations go here).
    mytoydb::memory::MemoryContext* es_query_cxt = nullptr;

    // Opened relations (for cleanup at ExecutorEnd).
    std::vector<mytoydb::access::Relation> es_open_relations;

    // Tuple table slots allocated during execution (for cleanup).
    std::vector<TupleTableSlot*> es_tupleTable;

    EState() = default;
    ~EState();
};

// QueryDesc — ties together a Query, its Plan, and executor state.
struct QueryDesc {
    mytoydb::parser::Query* query = nullptr;
    Plan* plan = nullptr;
    EState* estate = nullptr;
    PlanState* planstate = nullptr;

    QueryDesc() = default;
    ~QueryDesc();
};

}  // namespace mytoydb::executor
