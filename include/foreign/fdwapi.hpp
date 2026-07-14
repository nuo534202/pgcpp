// fdwapi.h — Foreign Data Wrapper (FDW) routine interface.
//
// Converted from PostgreSQL 15's src/include/foreign/fdwapi.h.
//
// An FDW is a plugin that provides access to data stored in external sources
// (files, other databases, web services, etc.). The plugin implements a set
// of callbacks collected in an FdwRoutine struct. The core code calls these
// callbacks during planning and execution of queries on foreign tables.
//
// pgcpp simplifications (single-process, in-memory):
//   - Only the scan lifecycle callbacks are required (Begin/Iterate/ReScan/End).
//   - Planner callbacks (GetForeignRelSize/Paths/Plan) are omitted because
//     pgcpp builds ForeignScan plans directly in tests rather than via the
//     optimizer. A future task can wire FDW into the planner.
//   - No tuple-routing / write-side callbacks (INSERT/UPDATE/DELETE).
#pragma once

#include <cstdint>
#include <string>

namespace pgcpp::executor {

// Forward declarations — FdwRoutine callbacks reference these types.
class ForeignScanState;
class TupleTableSlot;

}  // namespace pgcpp::executor

namespace pgcpp::foreign {

// FdwRoutine — the callback table implemented by every FDW handler.
//
// Mirrors PostgreSQL's FdwRoutine but keeps only the read-side scan callbacks.
// Each callback receives the ForeignScanState* so the handler can store and
// retrieve its own private state via ForeignScanState::fdw_state.
struct FdwRoutine {
    // BeginForeignScan — initialize the scan. Called once before the first
    // IterateForeignScan. The handler should open the foreign data source
    // (e.g., open a file, connect to a remote server) and store any private
    // state in state->fdw_state.
    void (*BeginForeignScan)(pgcpp::executor::ForeignScanState* state, uint32_t foreigntableid);

    // IterateForeignScan — return the next tuple as a TupleTableSlot*, or
    // nullptr when the scan is exhausted. The handler should fill
    // state->ps_ResultTupleSlot with the next row's values.
    pgcpp::executor::TupleTableSlot* (*IterateForeignScan)(
        pgcpp::executor::ForeignScanState* state);

    // ReScanForeignScan — rewind the scan to the beginning. Called when the
    // parent node needs to re-scan (e.g., nested-loop inner side).
    void (*ReScanForeignScan)(pgcpp::executor::ForeignScanState* state);

    // EndForeignScan — clean up. Called once after the last IterateForeignScan
    // (or after ReScanForeignScan if the parent decides to stop). The handler
    // should close the data source and free any private state.
    void (*EndForeignScan)(pgcpp::executor::ForeignScanState* state);
};

// FdwRoutineFactory — function pointer that returns a pointer to a static
// FdwRoutine struct. Each FDW handler provides one of these and registers
// it via RegisterFdw().
using FdwRoutineFactory = const FdwRoutine* (*)();

// RegisterFdw — register an FDW handler under the given name (e.g.,
// "file_fdw"). The factory must return a pointer to a static FdwRoutine.
// Throws ereport(ERROR) if the name is already registered.
void RegisterFdw(const std::string& fdwname, FdwRoutineFactory factory);

// LookupFdw — look up an FDW handler by name. Returns the FdwRoutine
// pointer, or nullptr if no handler is registered under that name.
const FdwRoutine* LookupFdw(const std::string& fdwname);

// ClearFdwRegistry — remove all registered FDW handlers (for testing).
void ClearFdwRegistry();

// NumRegisteredFdws — return the number of registered FDW handlers.
std::size_t NumRegisteredFdws();

}  // namespace pgcpp::foreign
