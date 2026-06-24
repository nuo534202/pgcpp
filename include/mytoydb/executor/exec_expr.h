// exec_expr.h — Expression evaluation context and evaluation API.
//
// Converted from PostgreSQL 15's src/include/executor/execExpr.h.
//
// The executor evaluates expressions (Var, Const, OpExpr, etc.) using
// ExecEvalExpr, which dispatches on the expression's NodeTag.
//
// ExprContext holds the tuple slots needed for evaluation:
//   - ecxt_scantuple: the current scan tuple (for Var references)
//   - ecxt_innertuple / ecxt_outertuple: for join evaluation
//   - ecxt_aggregates: pre-computed aggregate values (for Aggref)
#pragma once

#include <cstdint>
#include <vector>

#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/executor/tupletable.h"
#include "mytoydb/parser/primnodes.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::executor {

// ExprContext — holds the context for expression evaluation.
struct ExprContext {
    TupleTableSlot* ecxt_scantuple = nullptr;   // current scan tuple
    TupleTableSlot* ecxt_innertuple = nullptr;  // inner side of join
    TupleTableSlot* ecxt_outertuple = nullptr;  // outer side of join
    TupleTableSlot* ecxt_aggregates = nullptr;  // aggregate results (for Aggref)

    // Per-tuple memory context: reset between tuples to avoid leaks.
    mytoydb::memory::MemoryContext* ecxt_per_tuple_memory = nullptr;

    ExprContext() = default;
    ~ExprContext();
};

// ExprState — wrapper for an expression to be evaluated.
// In PostgreSQL this carries execution-time info; in MyToyDB it's
// minimal since we dispatch on NodeTag at evaluation time.
struct ExprState {
    mytoydb::parser::Node* expr = nullptr;
    explicit ExprState(mytoydb::parser::Node* e) : expr(e) {}
};

// ExecEvalExpr — evaluate an expression in the given context.
// Returns the resulting Datum; sets *isNull to true if the result is NULL.
mytoydb::types::Datum ExecEvalExpr(mytoydb::parser::Node* expr, ExprContext* econtext,
                                   bool* isNull);

// ExecQual — evaluate a qual (WHERE clause) predicate.
// Returns true if the tuple satisfies the qual (or if qual is null).
bool ExecQual(mytoydb::parser::Node* qual, ExprContext* econtext);

// ExecProject — evaluate a target list, producing a result tuple slot.
// The result is stored in result_slot.
void ExecProject(const std::vector<mytoydb::parser::TargetEntry*>& targetlist,
                 ExprContext* econtext, TupleTableSlot* result_slot);

// CreateExprContext — create a new ExprContext with a per-tuple memory context.
ExprContext* CreateExprContext();

// FreeExprContext — free an ExprContext and its per-tuple memory context.
void FreeExprContext(ExprContext* econtext);

// ResetExprContext — reset the per-tuple memory context.
void ResetExprContext(ExprContext* econtext);

// SetExecParams — set the bound parameter values for the current query
// execution. Used by the extended query protocol to pass Bind parameter
// values to ExecEvalExpr for Param node evaluation.
void SetExecParams(const std::vector<mytoydb::types::Datum>& values,
                   const std::vector<bool>& isnull);

// ClearExecParams — clear the bound parameter values after query execution.
void ClearExecParams();

}  // namespace mytoydb::executor
