// node_agg.cpp — Aggregate node implementation.
//
// Converted from PostgreSQL 15's src/backend/executor/nodeAgg.c.
//
// Supports COUNT, SUM, AVG, MIN, MAX with optional GROUP BY.
// Uses a hash table for grouped aggregation; plain aggregation uses
// a single group state.
//
// Execution is two-phase:
//   1. Consume all child tuples, accumulating per-group state.
//   2. Output one tuple per group (or a single tuple for plain agg).
#include "mytoydb/executor/node_agg.h"

#include <cstring>
#include <new>

#include "mytoydb/catalog/catalog.h"
#include "mytoydb/catalog/pg_proc.h"
#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/error/elog.h"
#include "mytoydb/executor/estate.h"
#include "mytoydb/executor/exec_expr.h"
#include "mytoydb/executor/exec_utils.h"
#include "mytoydb/executor/plannodes.h"
#include "mytoydb/executor/tupletable.h"
#include "mytoydb/parser/parse_node.h"
#include "mytoydb/parser/primnodes.h"
#include "mytoydb/types/datum.h"

namespace mytoydb::executor {

using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Aggref;
using mytoydb::parser::TargetEntry;
using mytoydb::types::BoolGetDatum;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetFloat4;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt16;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int32GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::kBoolOid;
using mytoydb::types::kDateOid;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kTimestampOid;
using mytoydb::types::TextPGetDatum;
using mytoydb::types::VARDATA;
using mytoydb::types::VARSIZE_DATA;

namespace {

// Determine the aggregate kind from the aggfnoid by looking up the proc name.
AggKind AggKindFromOid(Oid aggfnoid) {
    const auto* proc = GetCatalog()->GetProcByOid(aggfnoid);
    if (proc == nullptr)
        return AggKind::kCount;
    const std::string& name = proc->proname;
    if (name == "count")
        return AggKind::kCount;
    if (name == "sum")
        return AggKind::kSum;
    if (name == "avg")
        return AggKind::kAvg;
    if (name == "min")
        return AggKind::kMin;
    if (name == "max")
        return AggKind::kMax;
    return AggKind::kCount;
}

// Compare two text Datums lexicographically. Returns -1, 0, or 1.
int CompareTextValues(Datum a, Datum b) {
    const char* pa = DatumGetTextP(a);
    const char* pb = DatumGetTextP(b);
    int la = VARSIZE_DATA(pa);
    int lb = VARSIZE_DATA(pb);
    int min_len = la < lb ? la : lb;
    int cmp = std::memcmp(VARDATA(pa), VARDATA(pb), min_len);
    if (cmp != 0)
        return cmp < 0 ? -1 : 1;
    if (la < lb)
        return -1;
    if (la > lb)
        return 1;
    return 0;
}

}  // namespace

// Recursively find all Aggref nodes in an expression tree and register them.
// This handles Aggrefs nested inside OpExpr/FuncExpr/BoolExpr (e.g., the
// HAVING clause `COUNT(*) > 100000` has an Aggref inside an OpExpr).
static void CollectAggrefsRecursive(mytoydb::parser::Node* node,
                                    std::vector<AggStateInfo>& agg_infos, int& next_aggno) {
    if (node == nullptr)
        return;
    switch (node->GetTag()) {
        case NodeTag::kAggref: {
            auto* agg = static_cast<Aggref*>(node);
            // Skip if already assigned (shared between target list and HAVING).
            if (agg->aggno >= 0)
                return;
            agg->aggno = next_aggno++;
            AggStateInfo info;
            info.kind = AggKindFromOid(agg->aggfnoid);
            info.restype = agg->aggtype;
            info.aggno = agg->aggno;
            info.isstar = agg->aggstar;
            if (!agg->aggstar && !agg->args.empty()) {
                mytoydb::parser::Node* arg = agg->args[0];
                if (arg != nullptr && arg->GetTag() == NodeTag::kTargetEntry) {
                    info.arg = static_cast<TargetEntry*>(arg)->expr;
                } else {
                    info.arg = arg;
                }
            }
            if (info.arg != nullptr) {
                info.argtype = mytoydb::parser::exprType(info.arg);
            }
            agg_infos.push_back(info);
            return;
        }
        case NodeTag::kOpExpr: {
            auto* op = static_cast<mytoydb::parser::OpExpr*>(node);
            for (auto* arg : op->args)
                CollectAggrefsRecursive(arg, agg_infos, next_aggno);
            return;
        }
        case NodeTag::kFuncExpr: {
            auto* fn = static_cast<mytoydb::parser::FuncExpr*>(node);
            for (auto* arg : fn->args)
                CollectAggrefsRecursive(arg, agg_infos, next_aggno);
            return;
        }
        case NodeTag::kBoolExpr: {
            auto* be = static_cast<mytoydb::parser::BoolExpr*>(node);
            for (auto* arg : be->args)
                CollectAggrefsRecursive(arg, agg_infos, next_aggno);
            return;
        }
        case NodeTag::kRelabelType:
            CollectAggrefsRecursive(static_cast<mytoydb::parser::RelabelType*>(node)->arg,
                                    agg_infos, next_aggno);
            return;
        case NodeTag::kTargetEntry:
            CollectAggrefsRecursive(static_cast<TargetEntry*>(node)->expr, agg_infos, next_aggno);
            return;
        default:
            return;
    }
}

void AggState::CollectAggrefs() {
    auto* aggplan = static_cast<Agg*>(plan);
    int next_aggno = 0;

    // First pass: collect Aggrefs from the target list (top-level only —
    // target list entries are either Aggref or Var).
    for (TargetEntry* te : aggplan->targetlist) {
        if (te->expr == nullptr)
            continue;
        if (te->expr->GetTag() == NodeTag::kAggref) {
            auto* agg = static_cast<Aggref*>(te->expr);
            // Assign a unique aggno (matches PostgreSQL's exec_assign_aggno).
            agg->aggno = next_aggno++;
            AggStateInfo info;
            info.kind = AggKindFromOid(agg->aggfnoid);
            info.restype = agg->aggtype;
            info.aggno = agg->aggno;
            info.isstar = agg->aggstar;
            // The argument is the first element of agg->args (if any).
            // agg->args is a list of TargetEntry wrapping the real arg.
            if (!agg->aggstar && !agg->args.empty()) {
                mytoydb::parser::Node* arg = agg->args[0];
                if (arg != nullptr && arg->GetTag() == NodeTag::kTargetEntry) {
                    info.arg = static_cast<TargetEntry*>(arg)->expr;
                } else {
                    info.arg = arg;
                }
            }
            // Determine arg type from the argument expression.
            if (info.arg != nullptr) {
                info.argtype = mytoydb::parser::exprType(info.arg);
            }
            agg_infos.push_back(info);
        }
    }

    // Second pass: collect Aggrefs from the HAVING clause (recursively,
    // since HAVING expressions like `COUNT(*) > 100000` nest Aggref inside
    // OpExpr). Aggrefs already assigned from the target list are skipped.
    if (aggplan->having_qual != nullptr) {
        CollectAggrefsRecursive(aggplan->having_qual, agg_infos, next_aggno);
    }
}

void AggState::InitGroupState(AggGroupState& gs) {
    int n = static_cast<int>(agg_infos.size());
    gs.count.assign(n, 0);
    gs.sum_int.assign(n, 0);
    gs.sum_float.assign(n, 0.0);
    gs.min_val.assign(n, 0);
    gs.max_val.assign(n, 0);
    gs.minmax_init.assign(n, false);
    gs.minmax_null.assign(n, true);
}

void AggState::Accumulate(AggGroupState& gs, ExprContext* econtext) {
    for (int i = 0; i < static_cast<int>(agg_infos.size()); i++) {
        const AggStateInfo& info = agg_infos[i];
        bool isnull = false;
        Datum val = 0;

        if (!info.isstar && info.arg != nullptr) {
            val = ExecEvalExpr(info.arg, econtext, &isnull);
        }

        // COUNT(*) always counts; COUNT(expr) skips nulls.
        if (info.kind == AggKind::kCount) {
            if (info.isstar || !isnull) {
                gs.count[i]++;
            }
            continue;
        }

        // Other aggregates skip null inputs.
        if (isnull)
            continue;

        gs.count[i]++;

        switch (info.kind) {
            case AggKind::kSum:
                if (info.argtype == kInt2Oid || info.argtype == kInt4Oid ||
                    info.argtype == kInt8Oid) {
                    int64_t v = 0;
                    if (info.argtype == kInt2Oid)
                        v = DatumGetInt16(val);
                    else if (info.argtype == kInt4Oid)
                        v = DatumGetInt32(val);
                    else
                        v = DatumGetInt64(val);
                    gs.sum_int[i] += v;
                } else if (info.argtype == kFloat4Oid) {
                    gs.sum_float[i] += DatumGetFloat4(val);
                } else {
                    gs.sum_float[i] += DatumGetFloat8(val);
                }
                break;
            case AggKind::kAvg:
                // AVG accumulates in double (sum_float) to avoid int64 overflow
                // for large int8 values. PostgreSQL uses numeric; MyToyDB uses
                // float8 since numeric is not implemented.
                if (info.argtype == kInt2Oid) {
                    gs.sum_float[i] += static_cast<double>(DatumGetInt16(val));
                } else if (info.argtype == kInt4Oid) {
                    gs.sum_float[i] += static_cast<double>(DatumGetInt32(val));
                } else if (info.argtype == kInt8Oid) {
                    gs.sum_float[i] += static_cast<double>(DatumGetInt64(val));
                } else if (info.argtype == kFloat4Oid) {
                    gs.sum_float[i] += DatumGetFloat4(val);
                } else {
                    gs.sum_float[i] += DatumGetFloat8(val);
                }
                break;
            case AggKind::kMin:
            case AggKind::kMax: {
                bool take = false;
                if (!gs.minmax_init[i]) {
                    take = true;
                } else if (info.argtype == kInt2Oid) {
                    int16_t v = DatumGetInt16(val);
                    int16_t cur =
                        DatumGetInt16(info.kind == AggKind::kMin ? gs.min_val[i] : gs.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kInt4Oid || info.argtype == kDateOid) {
                    int32_t v = DatumGetInt32(val);
                    int32_t cur =
                        DatumGetInt32(info.kind == AggKind::kMin ? gs.min_val[i] : gs.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kInt8Oid || info.argtype == kTimestampOid) {
                    int64_t v = DatumGetInt64(val);
                    int64_t cur =
                        DatumGetInt64(info.kind == AggKind::kMin ? gs.min_val[i] : gs.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kFloat4Oid) {
                    float v = DatumGetFloat4(val);
                    float cur =
                        DatumGetFloat4(info.kind == AggKind::kMin ? gs.min_val[i] : gs.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kFloat8Oid) {
                    double v = DatumGetFloat8(val);
                    double cur =
                        DatumGetFloat8(info.kind == AggKind::kMin ? gs.min_val[i] : gs.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kTextOid) {
                    int cmp = CompareTextValues(
                        val, info.kind == AggKind::kMin ? gs.min_val[i] : gs.max_val[i]);
                    take = info.kind == AggKind::kMin ? (cmp < 0) : (cmp > 0);
                }
                if (take) {
                    if (info.kind == AggKind::kMin) {
                        gs.min_val[i] = val;
                    } else {
                        gs.max_val[i] = val;
                    }
                    gs.minmax_init[i] = true;
                    gs.minmax_null[i] = false;
                }
                break;
            }
            case AggKind::kCount:
                break;  // handled above
        }
    }
}

TupleTableSlot* AggState::BuildOutputSlot(const AggGroupKey& key, const AggGroupState& gs) {
    // Compute final aggregate values and store in the aggregates slot.
    int nagg = static_cast<int>(agg_infos.size());
    for (int i = 0; i < nagg; i++) {
        const AggStateInfo& info = agg_infos[i];
        Datum val = 0;
        bool isnull = false;

        switch (info.kind) {
            case AggKind::kCount:
                val = Int64GetDatum(gs.count[i]);
                break;
            case AggKind::kSum:
                if (gs.count[i] == 0) {
                    isnull = true;
                } else if (info.argtype == kInt2Oid || info.argtype == kInt4Oid ||
                           info.argtype == kInt8Oid) {
                    val = Int64GetDatum(gs.sum_int[i]);
                } else {
                    val = Float8GetDatum(gs.sum_float[i]);
                }
                break;
            case AggKind::kAvg:
                if (gs.count[i] == 0) {
                    isnull = true;
                } else {
                    // AVG uses sum_float (double) for all types to avoid overflow.
                    val = Float8GetDatum(gs.sum_float[i] / static_cast<double>(gs.count[i]));
                }
                break;
            case AggKind::kMin:
                if (gs.minmax_null[i]) {
                    isnull = true;
                } else {
                    val = gs.min_val[i];
                }
                break;
            case AggKind::kMax:
                if (gs.minmax_null[i]) {
                    isnull = true;
                } else {
                    val = gs.max_val[i];
                }
                break;
        }

        // Store at position aggno+1 (1-based) in the aggregates slot.
        ps_ExprContext->ecxt_aggregates->tts_values[info.aggno] = val;
        ps_ExprContext->ecxt_aggregates->tts_isnull[info.aggno] = isnull;
    }
    ps_ExprContext->ecxt_aggregates->tts_nvalid = true;
    ps_ExprContext->ecxt_aggregates->tts_isempty = false;

    // For GROUP BY: fill the group key slot with the group's key values.
    if (has_group_by) {
        TupleTableSlot* group_slot = ps_ExprContext->ecxt_scantuple;
        for (size_t i = 0; i < group_col_idx.size(); i++) {
            int attno = group_col_idx[i];  // 1-based
            if (attno >= 1 && attno <= group_slot->Natts()) {
                group_slot->tts_isnull[attno - 1] = key.isnull[i];
                if (key.isnull[i]) {
                    group_slot->tts_values[attno - 1] = 0;
                } else if (i < group_col_byref.size() && group_col_byref[i]) {
                    // Reconstruct a varlena buffer from the serialized string
                    // content. The original Datum pointed into a child tuple
                    // buffer that no longer exists. Allocate in the current
                    // (per-query) memory context.
                    const std::string& s = key.str_values[i];
                    std::size_t total = sizeof(int32_t) + s.size();
                    char* buf = static_cast<char*>(palloc(total));
                    int32_t header = static_cast<int32_t>(total);
                    std::memcpy(buf, &header, sizeof(header));
                    if (!s.empty()) {
                        std::memcpy(buf + sizeof(int32_t), s.data(), s.size());
                    }
                    group_slot->tts_values[attno - 1] = TextPGetDatum(buf);
                } else {
                    group_slot->tts_values[attno - 1] = key.values[i];
                }
            }
        }
        group_slot->tts_nvalid = true;
        group_slot->tts_isempty = false;
    }

    // Project the target list into the result slot.
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

void AggState::ExecInit() {
    auto* aggplan = static_cast<Agg*>(plan);
    has_group_by = !aggplan->groupColIdx.empty();
    group_col_idx = aggplan->groupColIdx;

    // Determine which GROUP BY columns are pass-by-reference (text/varchar).
    // For those, the group key must hold the string content, not the Datum
    // pointer, so that rows with identical text at different addresses hash
    // and compare equal.
    if (has_group_by && leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        auto* child_desc = leftps->ps_ResultTupleSlot->tts_tupleDescriptor;
        group_col_byref.reserve(group_col_idx.size());
        for (int attno : group_col_idx) {
            bool byref = false;
            if (child_desc != nullptr && attno >= 1 && attno <= child_desc->natts) {
                const auto& attr = child_desc->attrs[attno - 1];
                byref = !attr.attbyval;
            }
            group_col_byref.push_back(byref);
        }
    } else {
        group_col_byref.assign(group_col_idx.size(), false);
    }

    // Collect aggregate info from the target list.
    CollectAggrefs();

    // Initialize plain group state.
    InitGroupState(plain_group);
    plain_group_init = true;

    // Create the result slot from the target list.
    auto* result_desc = BuildTupleDescFromTargetList(aggplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    // Create the expression context.
    ps_ExprContext = CreateExprContext();

    // Create the aggregates slot (one attribute per aggregate).
    if (!agg_infos.empty()) {
        // Build a tuple descriptor for the aggregates slot.
        std::vector<mytoydb::catalog::FormData_pg_attribute> agg_attrs;
        for (const auto& info : agg_infos) {
            mytoydb::catalog::FormData_pg_attribute attr;
            attr.attnum = info.aggno + 1;
            attr.attname = "agg" + std::to_string(info.aggno);
            attr.atttypid = info.restype;
            int16_t len;
            bool byval;
            mytoydb::catalog::AttAlign align;
            FillTypeAttrs(info.restype, &len, &byval, &align);
            attr.attlen = len;
            attr.attbyval = byval;
            attr.attalign = align;
            agg_attrs.push_back(attr);
        }
        auto* agg_desc = mytoydb::access::CreateTupleDesc(agg_attrs);
        ps_ExprContext->ecxt_aggregates = TupleTableSlot::Make(agg_desc);
        state->es_tupleTable.push_back(ps_ExprContext->ecxt_aggregates);
    }

    // For GROUP BY: create a group key slot matching the child's output.
    if (has_group_by && leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple =
            TupleTableSlot::Make(leftps->ps_ResultTupleSlot->tts_tupleDescriptor);
        state->es_tupleTable.push_back(ps_ExprContext->ecxt_scantuple);
    } else if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        // Plain aggregation: use the child's result slot for any non-aggregate
        // expressions in the target list (shouldn't happen, but just in case).
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }
}

TupleTableSlot* AggState::ExecProcNode() {
    if (!output_started) {
        // Phase 1: consume all child tuples.
        for (;;) {
            TupleTableSlot* child_slot = leftps->ExecProcNode();
            if (child_slot == nullptr)
                break;

            // Set up the expression context with the child tuple for
            // evaluating aggregate arguments and group keys.
            ps_ExprContext->ecxt_scantuple = child_slot;
            ResetExprContext(ps_ExprContext);

            if (has_group_by) {
                // Compute the group key.
                AggGroupKey key;
                key.values.resize(group_col_idx.size());
                key.str_values.resize(group_col_idx.size());
                key.isnull.resize(group_col_idx.size());
                for (size_t i = 0; i < group_col_idx.size(); i++) {
                    int attno = group_col_idx[i];
                    bool isnull = false;
                    Datum val = 0;
                    if (child_slot != nullptr && attno >= 1 && attno <= child_slot->Natts()) {
                        val = child_slot->tts_values[attno - 1];
                        isnull = child_slot->tts_isnull[attno - 1];
                    }
                    key.isnull[i] = isnull;
                    if (isnull) {
                        key.values[i] = 0;
                        continue;
                    }
                    if (i < group_col_byref.size() && group_col_byref[i]) {
                        // Pass-by-reference: serialize the varlena content so
                        // identical strings at different addresses compare equal.
                        const char* text = DatumGetTextP(val);
                        int len = VARSIZE_DATA(text);
                        key.str_values[i].assign(VARDATA(text), len);
                        key.values[i] = 0;
                    } else {
                        key.values[i] = val;
                    }
                }

                // Find or create the group.
                auto it = groups.find(key);
                if (it == groups.end()) {
                    AggGroupState gs;
                    InitGroupState(gs);
                    it = groups.emplace(key, std::move(gs)).first;
                }
                Accumulate(it->second, ps_ExprContext);
            } else {
                Accumulate(plain_group, ps_ExprContext);
            }
        }
        output_started = true;
        if (has_group_by) {
            group_iter = groups.begin();
        }
    }

    // Phase 2: output.
    auto* aggplan = static_cast<Agg*>(plan);
    if (has_group_by) {
        while (group_iter != groups.end()) {
            TupleTableSlot* slot = BuildOutputSlot(group_iter->first, group_iter->second);
            ++group_iter;
            // Evaluate HAVING qual: skip groups that don't satisfy it.
            if (aggplan->having_qual != nullptr) {
                if (!ExecQual(aggplan->having_qual, ps_ExprContext)) {
                    continue;
                }
            }
            return slot;
        }
        return nullptr;
    } else {
        if (!plain_group_init) {
            return nullptr;
        }
        plain_group_init = false;  // Only output once.
        TupleTableSlot* slot = BuildOutputSlot(AggGroupKey{}, plain_group);
        // Evaluate HAVING qual for plain aggregation.
        if (aggplan->having_qual != nullptr) {
            if (!ExecQual(aggplan->having_qual, ps_ExprContext)) {
                return nullptr;
            }
        }
        return slot;
    }
}

void AggState::ExecEnd() {
    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
    groups.clear();
}

void AggState::ExecReScan() {
    groups.clear();
    InitGroupState(plain_group);
    plain_group_init = true;
    output_started = false;
    if (leftps != nullptr) {
        leftps->ExecReScan();
    }
}

}  // namespace mytoydb::executor
