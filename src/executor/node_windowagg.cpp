// node_windowagg.cpp — WindowAgg node implementation (window functions).
//
// Computes window aggregates over partitions. Assumes the child is
// sorted by PARTITION BY then ORDER BY columns so that rows in the
// same partition are contiguous.
//
// Execution model:
//   1. On the first ExecProcNode call, drain the child into wa_tuples.
//   2. For each output row, check whether the partition changed; if so,
//      reset the running aggregate state. Then accumulate the current
//      row into the running state and emit a result row.
//
// Window frame: ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
// (i.e., running aggregates from the start of the partition up to and
// including the current row). Other frame specifications are not yet
// supported.
//
// Aggregate detection: only Aggref nodes in the target list are
// recognized (COUNT/SUM/AVG/MIN/MAX). ROW_NUMBER / RANK / LAG / LEAD
// require a WindowFunc node type, which is not yet implemented.
#include "pgcpp/executor/node_windowagg.hpp"

#include <cstring>
#include <new>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_proc.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/executor/estate.hpp"
#include "pgcpp/executor/exec_expr.hpp"
#include "pgcpp/executor/exec_utils.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/executor/tupletable.hpp"
#include "pgcpp/parser/parse_node.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

namespace mytoydb::executor {

using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::memory::palloc;
using mytoydb::nodes::destroyPallocNode;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Aggref;
using mytoydb::parser::TargetEntry;
using mytoydb::types::Datum;
using mytoydb::types::DatumGetFloat4;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt16;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::Float8GetDatum;
using mytoydb::types::Int64GetDatum;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
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

void WindowAggState::CollectAggrefs() {
    auto* wplan = static_cast<WindowAgg*>(plan);
    int next_aggno = 0;
    for (TargetEntry* te : wplan->targetlist) {
        if (te == nullptr || te->expr == nullptr)
            continue;
        if (te->expr->GetTag() != NodeTag::kAggref)
            continue;
        auto* agg = static_cast<Aggref*>(te->expr);
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
        wa_agg_infos.push_back(info);
    }
}

void WindowAggState::InitRunningState() {
    int n = static_cast<int>(wa_agg_infos.size());
    wa_running.count.assign(n, 0);
    wa_running.sum_int.assign(n, 0);
    wa_running.sum_float.assign(n, 0.0);
    wa_running.min_val.assign(n, 0);
    wa_running.max_val.assign(n, 0);
    wa_running.minmax_init.assign(n, false);
    wa_running.minmax_null.assign(n, true);
    wa_running_init = true;
}

void WindowAggState::AccumulateRow(TupleTableSlot* row) {
    // Wire the expression context with the current row so Aggref args
    // (which reference the child's columns) evaluate correctly.
    ps_ExprContext->ecxt_scantuple = row;
    ResetExprContext(ps_ExprContext);

    for (int i = 0; i < static_cast<int>(wa_agg_infos.size()); i++) {
        const AggStateInfo& info = wa_agg_infos[i];
        bool isnull = false;
        Datum val = 0;
        if (!info.isstar && info.arg != nullptr) {
            val = ExecEvalExpr(info.arg, ps_ExprContext, &isnull);
        }

        if (info.kind == AggKind::kCount) {
            if (info.isstar || !isnull)
                wa_running.count[i]++;
            continue;
        }
        if (isnull)
            continue;
        wa_running.count[i]++;

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
                    wa_running.sum_int[i] += v;
                } else if (info.argtype == kFloat4Oid) {
                    wa_running.sum_float[i] += DatumGetFloat4(val);
                } else {
                    wa_running.sum_float[i] += DatumGetFloat8(val);
                }
                break;
            case AggKind::kAvg:
                if (info.argtype == kInt2Oid) {
                    wa_running.sum_float[i] += static_cast<double>(DatumGetInt16(val));
                } else if (info.argtype == kInt4Oid) {
                    wa_running.sum_float[i] += static_cast<double>(DatumGetInt32(val));
                } else if (info.argtype == kInt8Oid) {
                    wa_running.sum_float[i] += static_cast<double>(DatumGetInt64(val));
                } else if (info.argtype == kFloat4Oid) {
                    wa_running.sum_float[i] += DatumGetFloat4(val);
                } else {
                    wa_running.sum_float[i] += DatumGetFloat8(val);
                }
                break;
            case AggKind::kMin:
            case AggKind::kMax: {
                bool take = false;
                if (!wa_running.minmax_init[i]) {
                    take = true;
                } else if (info.argtype == kInt2Oid) {
                    int16_t v = DatumGetInt16(val);
                    int16_t cur = DatumGetInt16(info.kind == AggKind::kMin ? wa_running.min_val[i]
                                                                           : wa_running.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kInt4Oid) {
                    int32_t v = DatumGetInt32(val);
                    int32_t cur = DatumGetInt32(info.kind == AggKind::kMin ? wa_running.min_val[i]
                                                                           : wa_running.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kInt8Oid) {
                    int64_t v = DatumGetInt64(val);
                    int64_t cur = DatumGetInt64(info.kind == AggKind::kMin ? wa_running.min_val[i]
                                                                           : wa_running.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kFloat4Oid) {
                    float v = DatumGetFloat4(val);
                    float cur = DatumGetFloat4(info.kind == AggKind::kMin ? wa_running.min_val[i]
                                                                          : wa_running.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else if (info.argtype == kFloat8Oid) {
                    double v = DatumGetFloat8(val);
                    double cur = DatumGetFloat8(info.kind == AggKind::kMin ? wa_running.min_val[i]
                                                                           : wa_running.max_val[i]);
                    take = info.kind == AggKind::kMin ? (v < cur) : (v > cur);
                } else {
                    int cmp =
                        CompareTextValues(val, info.kind == AggKind::kMin ? wa_running.min_val[i]
                                                                          : wa_running.max_val[i]);
                    take = info.kind == AggKind::kMin ? (cmp < 0) : (cmp > 0);
                }
                if (take) {
                    if (info.kind == AggKind::kMin)
                        wa_running.min_val[i] = val;
                    else
                        wa_running.max_val[i] = val;
                    wa_running.minmax_init[i] = true;
                    wa_running.minmax_null[i] = false;
                }
                break;
            }
            case AggKind::kCount:
                break;
        }
    }
}

void WindowAggState::ResetForNewPartition() {
    int n = static_cast<int>(wa_agg_infos.size());
    for (int i = 0; i < n; i++) {
        wa_running.count[i] = 0;
        wa_running.sum_int[i] = 0;
        wa_running.sum_float[i] = 0.0;
        wa_running.min_val[i] = 0;
        wa_running.max_val[i] = 0;
        wa_running.minmax_init[i] = false;
        wa_running.minmax_null[i] = true;
    }
}

bool WindowAggState::SamePartition(TupleTableSlot* row) {
    if (!wa_has_last_part)
        return false;
    for (size_t i = 0; i < wa_partColIdx.size(); i++) {
        int attno = wa_partColIdx[i];
        if (attno < 1)
            continue;
        int idx = attno - 1;
        bool cur_null = (idx < row->Natts()) ? row->tts_isnull[idx] : true;
        bool last_null = (i < wa_last_part_null.size()) ? wa_last_part_null[i] : true;
        if (cur_null != last_null)
            return false;
        if (cur_null)
            continue;
        Datum cur_val = (idx < row->Natts()) ? row->tts_values[idx] : 0;
        Datum last_val = (i < wa_last_part_key.size()) ? wa_last_part_key[i] : 0;
        // Use the slot's tuple descriptor to determine the comparison type.
        Oid typid = kInt4Oid;
        if (row->tts_tupleDescriptor != nullptr && idx < row->tts_tupleDescriptor->natts)
            typid = row->tts_tupleDescriptor->attrs[idx].atttypid;
        int cmp = CompareDatumValues(cur_val, false, last_val, false, typid);
        if (cmp != 0)
            return false;
    }
    return true;
}

void WindowAggState::StoreAggregates() {
    if (wa_AggSlot == nullptr)
        return;
    int nagg = static_cast<int>(wa_agg_infos.size());
    for (int i = 0; i < nagg; i++) {
        const AggStateInfo& info = wa_agg_infos[i];
        Datum val = 0;
        bool isnull = false;
        switch (info.kind) {
            case AggKind::kCount:
                val = Int64GetDatum(wa_running.count[i]);
                break;
            case AggKind::kSum:
                if (wa_running.count[i] == 0) {
                    isnull = true;
                } else if (info.argtype == kInt2Oid || info.argtype == kInt4Oid ||
                           info.argtype == kInt8Oid) {
                    val = Int64GetDatum(wa_running.sum_int[i]);
                } else {
                    val = Float8GetDatum(wa_running.sum_float[i]);
                }
                break;
            case AggKind::kAvg:
                if (wa_running.count[i] == 0) {
                    isnull = true;
                } else {
                    val = Float8GetDatum(wa_running.sum_float[i] /
                                         static_cast<double>(wa_running.count[i]));
                }
                break;
            case AggKind::kMin:
                if (wa_running.minmax_null[i])
                    isnull = true;
                else
                    val = wa_running.min_val[i];
                break;
            case AggKind::kMax:
                if (wa_running.minmax_null[i])
                    isnull = true;
                else
                    val = wa_running.max_val[i];
                break;
        }
        wa_AggSlot->tts_values[info.aggno] = val;
        wa_AggSlot->tts_isnull[info.aggno] = isnull;
    }
    wa_AggSlot->tts_nvalid = true;
    wa_AggSlot->tts_isempty = false;
}

void WindowAggState::ExecInit() {
    auto* wplan = static_cast<WindowAgg*>(plan);
    wa_partColIdx = wplan->partColIdx;
    wa_ordColIdx = wplan->ordColIdx;
    wa_ordReverse = wplan->ordReverse;

    CollectAggrefs();

    // Result slot.
    auto* result_desc = BuildTupleDescFromTargetList(wplan->targetlist);
    ps_ResultTupleSlot = TupleTableSlot::Make(result_desc);
    state->es_tupleTable.push_back(ps_ResultTupleSlot);

    ps_ExprContext = CreateExprContext();
    if (leftps != nullptr && leftps->ps_ResultTupleSlot != nullptr) {
        ps_ExprContext->ecxt_scantuple = leftps->ps_ResultTupleSlot;
    }

    // Aggregates slot (one column per Aggref in target list).
    if (!wa_agg_infos.empty()) {
        std::vector<mytoydb::catalog::FormData_pg_attribute> agg_attrs;
        for (const auto& info : wa_agg_infos) {
            mytoydb::catalog::FormData_pg_attribute attr;
            attr.attnum = info.aggno + 1;
            attr.attname = "wagg" + std::to_string(info.aggno);
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
        wa_AggSlot = TupleTableSlot::Make(agg_desc);
        state->es_tupleTable.push_back(wa_AggSlot);
        ps_ExprContext->ecxt_aggregates = wa_AggSlot;
    }

    InitRunningState();
    wa_last_part_key.assign(wa_partColIdx.size(), 0);
    wa_last_part_null.assign(wa_partColIdx.size(), true);
    wa_has_last_part = false;
    wa_drained = false;
    wa_output_index = 0;
}

TupleTableSlot* WindowAggState::ExecProcNode() {
    if (!wa_drained) {
        // Phase 1: drain the child into wa_tuples.
        if (leftps != nullptr) {
            for (;;) {
                TupleTableSlot* child_slot = leftps->ExecProcNode();
                if (child_slot == nullptr)
                    break;
                TupleTableSlot* copy = TupleTableSlot::Make(child_slot->tts_tupleDescriptor);
                copy->StoreVirtual(child_slot->tts_values, child_slot->tts_isnull);
                wa_tuples.push_back(copy);
            }
        }
        wa_drained = true;
        wa_output_index = 0;
    }

    if (wa_output_index >= wa_tuples.size())
        return nullptr;

    TupleTableSlot* row = wa_tuples[wa_output_index++];

    // Detect partition change.
    if (!SamePartition(row)) {
        ResetForNewPartition();
        wa_has_last_part = true;
        for (size_t i = 0; i < wa_partColIdx.size(); i++) {
            int attno = wa_partColIdx[i];
            if (attno < 1 || attno > row->Natts()) {
                wa_last_part_key[i] = 0;
                wa_last_part_null[i] = true;
                continue;
            }
            wa_last_part_key[i] = row->tts_values[attno - 1];
            wa_last_part_null[i] = row->tts_isnull[attno - 1];
        }
    }

    // Accumulate this row's contribution to the running aggregate state.
    AccumulateRow(row);

    // Write aggregate values into the aggregates slot for projection.
    StoreAggregates();

    // Set up the expression context with the current row as the scan tuple
    // so Vars in the target list resolve to the row's columns.
    ps_ExprContext->ecxt_scantuple = row;
    ResetExprContext(ps_ExprContext);

    // Project target list into result slot.
    ExecProject(plan->targetlist, ps_ExprContext, ps_ResultTupleSlot);
    return ps_ResultTupleSlot;
}

void WindowAggState::ExecEnd() {
    for (TupleTableSlot* slot : wa_tuples) {
        destroyPallocNode(slot);
    }
    wa_tuples.clear();

    if (ps_ExprContext != nullptr) {
        FreeExprContext(ps_ExprContext);
        ps_ExprContext = nullptr;
    }
}

void WindowAggState::ExecReScan() {
    for (TupleTableSlot* slot : wa_tuples) {
        destroyPallocNode(slot);
    }
    wa_tuples.clear();
    wa_drained = false;
    wa_output_index = 0;
    wa_has_last_part = false;
    InitRunningState();
    if (leftps != nullptr)
        leftps->ExecReScan();
}

}  // namespace mytoydb::executor
