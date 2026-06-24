// exec_main.h — Top-level executor API.
//
// Converted from PostgreSQL 15's src/backend/executor/execMain.c.
//
// The executor lifecycle:
//   1. ExecutorStart(queryDesc) — create EState, init plan tree
//   2. ExecutorRun(queryDesc)   — execute, returning one tuple at a time
//   3. ExecutorFinish(queryDesc) — post-run cleanup (for DML)
//   4. ExecutorEnd(queryDesc)   — tear down everything
#pragma once

#include "mytoydb/executor/estate.h"
#include "mytoydb/executor/node_exec.h"

namespace mytoydb::executor {

// ExecutorStart — initialize the executor for a query.
// Creates the EState, sets up the snapshot, and calls ExecInitNode.
void ExecutorStart(QueryDesc* queryDesc);

// ExecutorRun — execute the query, returning the next result tuple.
// Returns nullptr when no more tuples.
TupleTableSlot* ExecutorRun(QueryDesc* queryDesc);

// ExecutorFinish — post-run cleanup (e.g., fire AFTER triggers).
// For SELECT this is a no-op; for DML it finalizes modifications.
void ExecutorFinish(QueryDesc* queryDesc);

// ExecutorEnd — tear down the executor.
// Calls ExecEndNode and frees the EState.
void ExecutorEnd(QueryDesc* queryDesc);

}  // namespace mytoydb::executor
