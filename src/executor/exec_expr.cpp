// exec_expr.cpp — Expression evaluation for the executor.
//
// Converted from PostgreSQL 15's src/backend/executor/execExpr*.c.
//
// Implements ExecEvalExpr (dispatch on NodeTag), ExecQual (WHERE clause
// evaluation), and ExecProject (target list evaluation).
#include "mytoydb/executor/exec_expr.h"

#include <cstring>
#include <string>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_operator.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/common/memory/alloc_set.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/parser/primnodes.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::executor {

using mytoydb::catalog::Catalog;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::kInvalidOid;
using mytoydb::catalog::Oid;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Aggref;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::Const;
using mytoydb::parser::FuncExpr;
using mytoydb::parser::Node;
using mytoydb::parser::NullTest;
using mytoydb::parser::NullTestType;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Param;
using mytoydb::parser::RelabelType;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::BoolGetDatum;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::kBoolOid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::VARDATA;
using mytoydb::types::VARSIZE;
using mytoydb::types::VARSIZE_DATA;

namespace {

// Special varno values for join evaluation (from primnodes.h).
using mytoydb::parser::kInnerVar;
using mytoydb::parser::kOuterVar;

// Current bound parameter values for extended query protocol execution.
// Set by SetExecParams() and cleared by ClearExecParams().
std::vector<Datum> g_param_values;
std::vector<bool> g_param_isnull;

// Get a Datum value from a slot at the given 1-based attribute number.
Datum GetSlotAttr(TupleTableSlot* slot, int attno, bool* isnull) {
    if (slot == nullptr || slot->tts_values == nullptr) {
        *isnull = true;
        return 0;
    }
    if (attno <= 0 || attno > slot->Natts()) {
        *isnull = true;
        return 0;
    }
    *isnull = slot->tts_isnull[attno - 1];
    return slot->tts_values[attno - 1];
}

// Compare two text Datums lexicographically.
int CompareText(Datum a, bool a_null, Datum b, bool b_null) {
    if (a_null && b_null)
        return 0;
    if (a_null)
        return -1;
    if (b_null)
        return 1;
    const char* pa = DatumGetTextP(a);
    const char* pb = DatumGetTextP(b);
    int len_a = VARSIZE_DATA(pa);
    int len_b = VARSIZE_DATA(pb);
    int min_len = len_a < len_b ? len_a : len_b;
    int cmp = std::memcmp(VARDATA(pa), VARDATA(pb), min_len);
    if (cmp != 0)
        return cmp;
    return len_a - len_b;
}

// Apply a comparison operator given the operator name and argument types.
// Returns a bool Datum.
Datum ApplyComparison(const std::string& opname, Oid lefttype, Datum left, bool left_null,
                      Datum right, bool right_null) {
    // SQL three-valued logic: if either operand is NULL, result is NULL (false).
    if (left_null || right_null) {
        return 0;  // Will be filtered by isnull check in caller.
    }

    bool result = false;

    if (lefttype == kInt4Oid) {
        int32_t l = DatumGetInt32(left);
        int32_t r = DatumGetInt32(right);
        if (opname == "=")
            result = (l == r);
        else if (opname == "<>")
            result = (l != r);
        else if (opname == "<")
            result = (l < r);
        else if (opname == "<=")
            result = (l <= r);
        else if (opname == ">")
            result = (l > r);
        else if (opname == ">=")
            result = (l >= r);
    } else if (lefttype == kInt8Oid) {
        int64_t l = DatumGetInt64(left);
        int64_t r = DatumGetInt64(right);
        if (opname == "=")
            result = (l == r);
        else if (opname == "<>")
            result = (l != r);
        else if (opname == "<")
            result = (l < r);
        else if (opname == "<=")
            result = (l <= r);
        else if (opname == ">")
            result = (l > r);
        else if (opname == ">=")
            result = (l >= r);
    } else if (lefttype == kFloat8Oid) {
        double l = DatumGetFloat8(left);
        double r = DatumGetFloat8(right);
        if (opname == "=")
            result = (l == r);
        else if (opname == "<>")
            result = (l != r);
        else if (opname == "<")
            result = (l < r);
        else if (opname == "<=")
            result = (l <= r);
        else if (opname == ">")
            result = (l > r);
        else if (opname == ">=")
            result = (l >= r);
    } else if (lefttype == kTextOid) {
        int cmp = CompareText(left, false, right, false);
        if (opname == "=")
            result = (cmp == 0);
        else if (opname == "<>")
            result = (cmp != 0);
        else if (opname == "<")
            result = (cmp < 0);
        else if (opname == "<=")
            result = (cmp <= 0);
        else if (opname == ">")
            result = (cmp > 0);
        else if (opname == ">=")
            result = (cmp >= 0);
    } else if (lefttype == kBoolOid) {
        bool l = DatumGetBool(left);
        bool r = DatumGetBool(right);
        if (opname == "=")
            result = (l == r);
        else if (opname == "<>")
            result = (l != r);
    }

    return BoolGetDatum(result);
}

// Apply an arithmetic operator given the operator name and argument types.
Datum ApplyArithmetic(const std::string& opname, Oid type, Datum left, bool left_null, Datum right,
                      bool right_null) {
    if (left_null || right_null) {
        return 0;
    }

    if (type == kInt4Oid) {
        int32_t l = DatumGetInt32(left);
        int32_t r = DatumGetInt32(right);
        if (opname == "+")
            return Int32GetDatum(l + r);
        if (opname == "-")
            return Int32GetDatum(l - r);
        if (opname == "*")
            return Int32GetDatum(l * r);
        if (opname == "/")
            return Int32GetDatum(r != 0 ? l / r : 0);
    } else if (type == kInt8Oid) {
        int64_t l = DatumGetInt64(left);
        int64_t r = DatumGetInt64(right);
        if (opname == "+")
            return Int64GetDatum(l + r);
        if (opname == "-")
            return Int64GetDatum(l - r);
        if (opname == "*")
            return Int64GetDatum(l * r);
        if (opname == "/")
            return Int64GetDatum(r != 0 ? l / r : 0);
    } else if (type == kFloat8Oid) {
        double l = DatumGetFloat8(left);
        double r = DatumGetFloat8(right);
        if (opname == "+")
            return Float8GetDatum(l + r);
        if (opname == "-")
            return Float8GetDatum(l - r);
        if (opname == "*")
            return Float8GetDatum(l * r);
        if (opname == "/")
            return Float8GetDatum(r != 0.0 ? l / r : 0.0);
    }

    return 0;
}

}  // namespace

Datum ExecEvalExpr(Node* expr, ExprContext* econtext, bool* isNull) {
    if (expr == nullptr) {
        *isNull = true;
        return 0;
    }

    NodeTag tag = expr->GetTag();

    switch (tag) {
        case NodeTag::kVar: {
            auto* var = static_cast<Var*>(expr);
            TupleTableSlot* slot;
            if (var->varno == kInnerVar) {
                slot = econtext->ecxt_innertuple;
            } else if (var->varno == kOuterVar) {
                slot = econtext->ecxt_outertuple;
            } else {
                slot = econtext->ecxt_scantuple;
            }
            return GetSlotAttr(slot, var->varattno, isNull);
        }

        case NodeTag::kConst: {
            auto* c = static_cast<Const*>(expr);
            *isNull = c->constisnull;
            return c->constvalue;
        }

        case NodeTag::kParam: {
            auto* p = static_cast<Param*>(expr);
            int idx = p->paramid - 1;  // 1-based to 0-based
            if (idx < 0 || idx >= static_cast<int>(g_param_values.size())) {
                *isNull = true;
                return 0;
            }
            *isNull = g_param_isnull[idx];
            return g_param_values[idx];
        }

        case NodeTag::kOpExpr: {
            auto* op = static_cast<OpExpr*>(expr);
            if (op->args.size() != 2) {
                *isNull = true;
                return 0;
            }
            bool left_null = false, right_null = false;
            Datum left = ExecEvalExpr(op->args[0], econtext, &left_null);
            Datum right = ExecEvalExpr(op->args[1], econtext, &right_null);

            // Look up operator name from catalog.
            const auto* oprow = GetCatalog()->GetOperatorByOid(op->opno);
            if (oprow == nullptr) {
                *isNull = true;
                return 0;
            }
            const std::string& opname = oprow->oprname;

            // Determine if this is a comparison or arithmetic operator.
            if (opname == "=" || opname == "<>" || opname == "<" || opname == "<=" ||
                opname == ">" || opname == ">=") {
                if (left_null || right_null) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return ApplyComparison(opname, oprow->oprleft, left, left_null, right, right_null);
            }
            if (opname == "+" || opname == "-" || opname == "*" || opname == "/") {
                if (left_null || right_null) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return ApplyArithmetic(opname, oprow->oprleft, left, left_null, right, right_null);
            }
            *isNull = true;
            return 0;
        }

        case NodeTag::kFuncExpr: {
            // FuncExpr evaluation: support a few built-in functions.
            // For now, just return NULL for unsupported functions.
            auto* fn = static_cast<FuncExpr*>(expr);
            (void)fn;
            *isNull = true;
            return 0;
        }

        case NodeTag::kAggref: {
            // Aggref: look up pre-computed value in the aggregates slot.
            auto* agg = static_cast<Aggref*>(expr);
            if (econtext->ecxt_aggregates == nullptr || agg->aggno < 0) {
                *isNull = true;
                return 0;
            }
            return GetSlotAttr(econtext->ecxt_aggregates, agg->aggno + 1, isNull);
        }

        case NodeTag::kBoolExpr: {
            auto* boolexpr = static_cast<BoolExpr*>(expr);
            BoolExprType op = boolexpr->boolop;

            if (op == BoolExprType::kAnd) {
                // AND: short-circuit on false.
                for (auto* arg : boolexpr->args) {
                    bool arg_null = false;
                    Datum result = ExecEvalExpr(arg, econtext, &arg_null);
                    if (!arg_null && !DatumGetBool(result)) {
                        *isNull = false;
                        return BoolGetDatum(false);
                    }
                }
                *isNull = false;
                return BoolGetDatum(true);
            }

            if (op == BoolExprType::kOr) {
                // OR: short-circuit on true.
                for (auto* arg : boolexpr->args) {
                    bool arg_null = false;
                    Datum result = ExecEvalExpr(arg, econtext, &arg_null);
                    if (!arg_null && DatumGetBool(result)) {
                        *isNull = false;
                        return BoolGetDatum(true);
                    }
                }
                *isNull = false;
                return BoolGetDatum(false);
            }

            if (op == BoolExprType::kNot) {
                // NOT: negate single argument.
                if (boolexpr->args.empty()) {
                    *isNull = true;
                    return 0;
                }
                bool arg_null = false;
                Datum result = ExecEvalExpr(boolexpr->args[0], econtext, &arg_null);
                if (arg_null) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return BoolGetDatum(!DatumGetBool(result));
            }
            *isNull = true;
            return 0;
        }

        case NodeTag::kNullTest: {
            auto* nt = static_cast<NullTest*>(expr);
            bool arg_null = false;
            ExecEvalExpr(nt->arg, econtext, &arg_null);
            *isNull = false;
            if (nt->nulltesttype == NullTestType::kIsNull) {
                return BoolGetDatum(arg_null);
            } else {
                return BoolGetDatum(!arg_null);
            }
        }

        case NodeTag::kRelabelType: {
            // RelabelType: type cast (no-op for our purposes).
            auto* rt = static_cast<RelabelType*>(expr);
            return ExecEvalExpr(rt->arg, econtext, isNull);
        }

        case NodeTag::kTargetEntry: {
            // TargetEntry: evaluate the underlying expression.
            auto* te = static_cast<TargetEntry*>(expr);
            return ExecEvalExpr(te->expr, econtext, isNull);
        }

        default:
            // Unsupported expression type.
            *isNull = true;
            return 0;
    }
}

bool ExecQual(Node* qual, ExprContext* econtext) {
    if (qual == nullptr) {
        return true;  // No qual = all tuples pass.
    }
    bool isNull = false;
    Datum result = ExecEvalExpr(qual, econtext, &isNull);
    if (isNull) {
        return false;  // NULL qual = tuple doesn't pass.
    }
    return DatumGetBool(result);
}

void ExecProject(const std::vector<TargetEntry*>& targetlist, ExprContext* econtext,
                 TupleTableSlot* result_slot) {
    int natts = result_slot->Natts();
    for (int i = 0; i < natts && i < static_cast<int>(targetlist.size()); i++) {
        bool isNull = false;
        Datum value = ExecEvalExpr(targetlist[i]->expr, econtext, &isNull);
        result_slot->tts_values[i] = value;
        result_slot->tts_isnull[i] = isNull;
    }
    result_slot->tts_nvalid = true;
    result_slot->tts_isempty = false;
    result_slot->tts_tuple = nullptr;  // Virtual tuple.
    result_slot->tts_shouldFree = false;
}

// --- ExprContext lifecycle ---

ExprContext* CreateExprContext() {
    void* mem = palloc(sizeof(ExprContext));
    auto* econtext = new (mem) ExprContext();
    // Create a per-tuple memory context as a child of the current context.
    econtext->ecxt_per_tuple_memory = mytoydb::memory::AllocSetContext::Create("ExprContext");
    return econtext;
}

void FreeExprContext(ExprContext* econtext) {
    if (econtext == nullptr)
        return;
    if (econtext->ecxt_per_tuple_memory != nullptr) {
        econtext->ecxt_per_tuple_memory->Delete();
        econtext->ecxt_per_tuple_memory = nullptr;
    }
    econtext->~ExprContext();
    mytoydb::memory::pfree(econtext);
}

void ResetExprContext(ExprContext* econtext) {
    if (econtext == nullptr)
        return;
    if (econtext->ecxt_per_tuple_memory != nullptr) {
        econtext->ecxt_per_tuple_memory->Reset();
    }
}

void SetExecParams(const std::vector<Datum>& values, const std::vector<bool>& isnull) {
    g_param_values = values;
    g_param_isnull = isnull;
}

void ClearExecParams() {
    g_param_values.clear();
    g_param_isnull.clear();
}

ExprContext::~ExprContext() = default;

}  // namespace mytoydb::executor
