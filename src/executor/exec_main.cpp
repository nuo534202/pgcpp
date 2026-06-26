// exec_main.cpp — Top-level executor API.
//
// Converted from PostgreSQL 15's src/backend/executor/execMain.c.
//
// Implements the executor lifecycle:
//   ExecutorStart  — create EState, set up snapshot, init plan tree
//   ExecutorRun    — fetch the next result tuple
//   ExecutorFinish — post-run cleanup (no-op for SELECT)
//   ExecutorEnd    — tear down plan tree and EState
#include "mytoydb/executor/exec_main.hpp"

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/memory/alloc_set.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/executor/estate.hpp"
#include "mytoydb/executor/node_exec.hpp"
#include "mytoydb/parser/parsenodes.hpp"
#include "mytoydb/transaction/snapshot.hpp"
#include "mytoydb/transaction/xact.hpp"

namespace mytoydb::executor {

using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::makePallocNode;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::transaction::GetCurrentCommandId;
using mytoydb::transaction::GetTransactionSnapshot;

void ExecutorStart(QueryDesc* queryDesc) {
    if (queryDesc == nullptr || queryDesc->plan == nullptr)
        return;

    // Create the EState.
    auto* estate = makePallocNode<EState>();
    queryDesc->estate = estate;

    // Copy the range table from the Query.
    if (queryDesc->query != nullptr) {
        for (mytoydb::parser::Node* node : queryDesc->query->rtable) {
            if (node != nullptr && node->GetTag() == mytoydb::nodes::NodeTag::kRangeTblEntry) {
                estate->es_range_table.push_back(static_cast<RangeTblEntry*>(node));
            }
        }
    }

    // Set up the snapshot for visibility checks.
    estate->es_snapshot = GetTransactionSnapshot();

    // Set the command ID for DML.
    estate->es_output_cid = GetCurrentCommandId(false);

    // Create a per-query memory context.
    estate->es_query_cxt = mytoydb::memory::AllocSetContext::Create("ExecutorState");

    // Initialize the plan tree.
    queryDesc->planstate = ExecInitNode(queryDesc->plan, estate);
}

TupleTableSlot* ExecutorRun(QueryDesc* queryDesc) {
    if (queryDesc == nullptr || queryDesc->planstate == nullptr) {
        return nullptr;
    }
    return ExecProcNode(queryDesc->planstate);
}

void ExecutorFinish(QueryDesc* queryDesc) {
    // For SELECT this is a no-op.
    // For DML, this would fire AFTER triggers and finalize modifications.
    // MyToyDB does not yet implement triggers.
    (void)queryDesc;
}

void ExecutorEnd(QueryDesc* queryDesc) {
    if (queryDesc == nullptr)
        return;

    // Tear down the plan tree.
    if (queryDesc->planstate != nullptr) {
        ExecEndNode(queryDesc->planstate);
        queryDesc->planstate = nullptr;
    }

    // Free the EState.
    if (queryDesc->estate != nullptr) {
        EState* estate = queryDesc->estate;

        // Delete the per-query memory context.
        if (estate->es_query_cxt != nullptr) {
            estate->es_query_cxt->Delete();
            estate->es_query_cxt = nullptr;
        }

        // The EState destructor closes opened relations and frees slots.
        destroyPallocNode(estate);
        queryDesc->estate = nullptr;
    }
}

}  // namespace mytoydb::executor
