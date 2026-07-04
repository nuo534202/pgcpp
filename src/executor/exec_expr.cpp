// exec_expr.cpp — Expression evaluation for the executor.
//
// Converted from PostgreSQL 15's src/backend/executor/execExpr*.c.
//
// Implements ExecEvalExpr (dispatch on NodeTag), ExecQual (WHERE clause
// evaluation), and ExecProject (target list evaluation).
#include "executor/exec_expr.hpp"

#include <cstring>
#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_operator.hpp"
#include "catalog/pg_proc.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "parser/primnodes.hpp"
#include "types/datetime.hpp"
#include "types/datum.hpp"
#include "types/string_funcs.hpp"

namespace pgcpp::executor {
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;

using pgcpp::catalog::Catalog;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::memory::palloc;
using pgcpp::nodes::NodeTag;
using pgcpp::parser::AArrayExpr;
using pgcpp::parser::Aggref;
using pgcpp::parser::BoolExpr;
using pgcpp::parser::BoolExprType;
using pgcpp::parser::CaseExpr;
using pgcpp::parser::CaseWhen;
using pgcpp::parser::Const;
using pgcpp::parser::FuncExpr;
using pgcpp::parser::Node;
using pgcpp::parser::NullTest;
using pgcpp::parser::NullTestType;
using pgcpp::parser::OpExpr;
using pgcpp::parser::Param;
using pgcpp::parser::RelabelType;
using pgcpp::parser::ScalarArrayOpExpr;
using pgcpp::parser::TargetEntry;
using pgcpp::parser::Var;
using pgcpp::types::BoolGetDatum;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::Float8GetDatum;
using pgcpp::types::Int32GetDatum;
using pgcpp::types::Int64GetDatum;
using pgcpp::types::kBoolOid;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;
using pgcpp::types::like;
using pgcpp::types::not_like;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE;
using pgcpp::types::VARSIZE_DATA;

namespace {

// Special varno values for join evaluation (from primnodes.h).
using pgcpp::parser::kInnerVar;
using pgcpp::parser::kOuterVar;

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

    if (lefttype == kInt4Oid || lefttype == kDateOid) {
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
    } else if (lefttype == kInt8Oid || lefttype == kTimestampOid) {
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
            // LIKE (~~) and NOT LIKE (!~~): text pattern matching.
            // The operands are text/varchar Datums (pass-by-reference).
            if (opname == "~~" || opname == "!~~") {
                if (left_null || right_null) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                if (opname == "~~") {
                    return like(left, right);
                }
                return not_like(left, right);
            }
            *isNull = true;
            return 0;
        }

        case NodeTag::kScalarArrayOpExpr: {
            // Scalar OP array, e.g. x IN (1,2,3) (use_or=true) or
            // x OP ALL(...) (use_or=false). pgcpp represents the IN list
            // directly as an AArrayExpr in args[1] (no ArrayExpr coercion).
            auto* saop = static_cast<ScalarArrayOpExpr*>(expr);
            if (saop->args.size() != 2) {
                *isNull = true;
                return 0;
            }
            const auto* oprow = GetCatalog()->GetOperatorByOid(saop->opno);
            if (oprow == nullptr) {
                *isNull = true;
                return 0;
            }
            const std::string& opname = oprow->oprname;
            Oid lefttype = oprow->oprleft;

            // Evaluate the scalar operand.
            bool scalar_null = false;
            Datum scalar = ExecEvalExpr(saop->args[0], econtext, &scalar_null);
            if (scalar_null) {
                // NULL IN (...) → NULL per SQL.
                *isNull = true;
                return 0;
            }

            // args[1] must be an AArrayExpr (pgcpp's IN-list representation).
            if (saop->args[1]->GetTag() != NodeTag::kAArrayExpr) {
                *isNull = true;
                return 0;
            }
            auto* arr = static_cast<AArrayExpr*>(saop->args[1]);

            bool any_null = false;
            for (Node* elem_node : arr->elements) {
                bool elem_null = false;
                Datum elem_val = ExecEvalExpr(elem_node, econtext, &elem_null);
                if (elem_null) {
                    any_null = true;
                    continue;
                }
                Datum cmp = ApplyComparison(opname, lefttype, scalar, false, elem_val, false);
                bool matched = DatumGetBool(cmp);
                if (saop->use_or) {
                    // ANY/IN: short-circuit on true.
                    if (matched) {
                        *isNull = false;
                        return BoolGetDatum(true);
                    }
                } else {
                    // ALL/NOT IN: short-circuit on false.
                    if (!matched) {
                        *isNull = false;
                        return BoolGetDatum(false);
                    }
                }
            }

            if (saop->use_or) {
                // No match. If any element was NULL, result is NULL per SQL.
                if (any_null) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return BoolGetDatum(false);
            }
            // ALL: all elements matched (no false encountered). NULL → NULL.
            if (any_null) {
                *isNull = true;
                return 0;
            }
            *isNull = false;
            return BoolGetDatum(true);
        }

        case NodeTag::kFuncExpr: {
            auto* fn = static_cast<FuncExpr*>(expr);
            const auto* proc = GetCatalog()->GetProcByOid(fn->funcid);
            if (proc == nullptr) {
                *isNull = true;
                return 0;
            }
            const std::string& fname = proc->proname;

            // Evaluate all arguments.
            std::vector<Datum> arg_values;
            std::vector<bool> arg_nulls;
            arg_values.reserve(fn->args.size());
            arg_nulls.reserve(fn->args.size());
            for (Node* arg : fn->args) {
                bool arg_null = false;
                arg_values.push_back(ExecEvalExpr(arg, econtext, &arg_null));
                arg_nulls.push_back(arg_null);
            }

            // extract(field, timestamp) — extract a component from a timestamp.
            if (fname == "extract") {
                if (arg_nulls[0] || arg_nulls[1]) {
                    *isNull = true;
                    return 0;
                }
                const char* text = DatumGetTextP(arg_values[0]);
                int len = VARSIZE_DATA(text);
                std::string field(VARDATA(text), len);
                *isNull = false;
                return pgcpp::types::extract(field.c_str(), arg_values[1]);
            }

            // date_trunc(field, timestamp) — truncate a timestamp to the
            // specified precision. Symmetric with extract: arg[0] is the field
            // name as text, arg[1] is the timestamp Datum.
            if (fname == "date_trunc") {
                if (arg_nulls[0] || arg_nulls[1]) {
                    *isNull = true;
                    return 0;
                }
                const char* text = DatumGetTextP(arg_values[0]);
                int len = VARSIZE_DATA(text);
                std::string field(VARDATA(text), len);
                *isNull = false;
                return pgcpp::types::date_trunc(field.c_str(), arg_values[1]);
            }

            // length(text) — return the number of bytes in a text value.
            if (fname == "length") {
                if (arg_nulls[0]) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return pgcpp::types::text_length(arg_values[0]);
            }

            // regexp_replace(source, pattern, replacement) — regex replacement.
            if (fname == "regexp_replace") {
                if (arg_nulls[0] || arg_nulls[1] || arg_nulls[2]) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return pgcpp::types::regexp_replace(arg_values[0], arg_values[1], arg_values[2]);
            }

            // substring(text, pattern) — regex substring extraction.
            if (fname == "substring") {
                if (arg_nulls[0] || arg_nulls[1]) {
                    *isNull = true;
                    return 0;
                }
                *isNull = false;
                return pgcpp::types::substring(arg_values[0], arg_values[1]);
            }

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

        case NodeTag::kCaseExpr: {
            auto* ce = static_cast<CaseExpr*>(expr);
            // F-4d: simple CASE form (CASE arg WHEN v THEN r ...) is not
            // implemented — fail explicitly rather than silently returning
            // the ELSE clause for every row.
            if (ce->arg != nullptr) {
                ereport(pgcpp::error::LogLevel::kError,
                        "simple CASE form (CASE arg WHEN v THEN r) is not supported");
            }
            // Searched form: CASE WHEN cond THEN result [WHEN ...]
            // [ELSE defresult] END. Each CaseWhen::expr is a boolean
            // condition.
            for (Node* when_node : ce->args) {
                auto* cw = static_cast<CaseWhen*>(when_node);
                bool cond_null = false;
                Datum cond_val = ExecEvalExpr(cw->expr, econtext, &cond_null);
                if (!cond_null && DatumGetBool(cond_val)) {
                    return ExecEvalExpr(cw->result, econtext, isNull);
                }
            }
            // No WHEN matched — return defresult (ELSE clause), or NULL.
            if (ce->defresult != nullptr) {
                return ExecEvalExpr(ce->defresult, econtext, isNull);
            }
            *isNull = true;
            return 0;
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
            // F-4e: Unsupported expression type — fail explicitly rather
            // than silently returning NULL (which produces wrong results).
            ereport(pgcpp::error::LogLevel::kError, "unsupported expression type in ExecEvalExpr");
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
    auto* econtext = makePallocNode<ExprContext>();
    // Create a per-tuple memory context as a child of the current context.
    econtext->ecxt_per_tuple_memory = pgcpp::memory::AllocSetContext::Create("ExprContext");
    return econtext;
}

void FreeExprContext(ExprContext* econtext) {
    if (econtext == nullptr)
        return;
    if (econtext->ecxt_per_tuple_memory != nullptr) {
        econtext->ecxt_per_tuple_memory->Delete();
        econtext->ecxt_per_tuple_memory = nullptr;
    }
    destroyPallocNode(econtext);
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

ExprContext::~ExprContext() {
    // The per-tuple memory context is allocated via `new AllocSetContext`
    // (not palloc), so it is not reclaimed by the owning MemoryContext's
    // block cleanup. Delete it explicitly here. FreeExprContext() sets the
    // pointer to nullptr after deleting, preventing double-delete.
    if (ecxt_per_tuple_memory != nullptr) {
        ecxt_per_tuple_memory->Delete();
        ecxt_per_tuple_memory = nullptr;
    }
}

}  // namespace pgcpp::executor
