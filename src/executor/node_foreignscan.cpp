// node_foreignscan.cpp — ForeignScan node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeForeignscan.c.
//
// Drives the FDW scan lifecycle: resolves the FDW handler from the foreign
// table's server, calls BeginForeignScan in ExecInit, IterateForeignScan in
// ExecProcNode (with qual + projection), and EndForeignScan in ExecEnd.
#include "executor/node_foreignscan.hpp"

#include <cstdio>
#include <vector>

#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "common/error/elog.hpp"
#include "executor/estate.hpp"
#include "executor/exec_expr.hpp"
#include "executor/exec_utils.hpp"
#include "executor/plannodes.hpp"
#include "executor/tupletable.hpp"
#include "foreign/foreign.hpp"

namespace pgcpp::executor {

using pgcpp::access::CreateTupleDesc;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::GetCatalog;
using pgcpp::foreign::LookupFdw;
using pgcpp::foreign::LookupForeignServerByOid;
using pgcpp::foreign::LookupForeignTable;

void ForeignScanState::ExecInit() {
    auto* fsplan = static_cast<ForeignScan*>(this->plan);

    // Look up the foreign table in the FDW catalog.
    const auto* ft = LookupForeignTable(fsplan->fs_relid);
    if (ft == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "ForeignScan: foreign table OID %u not found",
                      fsplan->fs_relid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }

    // Resolve the FDW handler: foreign table → server → fdwname → FdwRoutine.
    const auto* server = LookupForeignServerByOid(ft->serverid);
    if (server == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "ForeignScan: server OID %u not found", ft->serverid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    fs_routine = LookupFdw(server->fdwname);
    if (fs_routine == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "ForeignScan: FDW handler \"%s\" not registered",
                      server->fdwname.c_str());
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }

    // Build the scan tuple slot from the foreign table's pg_attribute rows.
    // The FDW handler fills this slot with each foreign row's values.
    auto attrs = GetCatalog()->GetAttributes(fsplan->fs_relid);
    std::vector<pgcpp::catalog::FormData_pg_attribute> attr_vec;
    attr_vec.reserve(attrs.size());
    for (const auto* a : attrs) {
        attr_vec.push_back(*a);
    }
    TupleDesc scan_desc = CreateTupleDesc(attr_vec);
    fs_ScanTupleSlot = TupleTableSlot::Make(scan_desc);
    state->es_tupleTable.push_back(fs_ScanTupleSlot);

    // Build the result tuple slot from the target list (for projection).
    TupleDesc result_desc = BuildTupleDescFromTargetList(fsplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();
    ps_ExprContext->ecxt_scantuple = fs_ScanTupleSlot;

    // Let the FDW handler initialize (open the data source, etc.).
    fs_routine->BeginForeignScan(this, fsplan->fs_relid);
}

TupleTableSlot* ForeignScanState::ExecProcNode() {
    for (;;) {
        // Ask the FDW handler for the next foreign row.
        TupleTableSlot* slot = fs_routine->IterateForeignScan(this);
        if (slot == nullptr) {
            return nullptr;  // scan exhausted
        }

        // The FDW handler filled fs_ScanTupleSlot; use it as the scan tuple.
        ResetExprContext(ps_ExprContext);
        ps_ExprContext->ecxt_scantuple = fs_ScanTupleSlot;

        // Evaluate the qual (WHERE clause).
        if (plan->qual != nullptr && !ExecQual(plan->qual, ps_ExprContext)) {
            continue;  // tuple doesn't pass the filter
        }

        // Project the target list into the result slot.
        ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
        return ps_ResultTupleSlot;
    }
}

void ForeignScanState::ExecEnd() {
    if (fs_routine != nullptr) {
        fs_routine->EndForeignScan(this);
        fs_routine = nullptr;
    }
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void ForeignScanState::ExecReScan() {
    if (fs_routine != nullptr) {
        fs_routine->ReScanForeignScan(this);
    }
}

}  // namespace pgcpp::executor
