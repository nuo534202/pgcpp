// exec_utils.cpp — Shared executor utility helpers.
#include "mytoydb/executor/exec_utils.hpp"

#include <cstring>
#include <new>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::executor {
using mytoydb::nodes::makePallocNode;

using mytoydb::catalog::AttAlign;
using mytoydb::catalog::FormData_pg_attribute;
using mytoydb::catalog::Oid;
using mytoydb::memory::palloc;
using mytoydb::nodes::NodeTag;
using mytoydb::parser::Aggref;
using mytoydb::parser::Const;
using mytoydb::parser::OpExpr;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::DatumGetBool;
using mytoydb::types::DatumGetFloat8;
using mytoydb::types::DatumGetInt32;
using mytoydb::types::DatumGetInt64;
using mytoydb::types::DatumGetTextP;
using mytoydb::types::kBoolOid;
using mytoydb::types::kDateOid;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kTimestampOid;
using mytoydb::types::kVarcharOid;
using mytoydb::types::VARDATA;
using mytoydb::types::VARSIZE_DATA;

void FillTypeAttrs(Oid typid, int16_t* attlen, bool* attbyval, AttAlign* attalign) {
    switch (typid) {
        case kBoolOid:
            *attlen = 1;
            *attbyval = true;
            *attalign = AttAlign::kChar;
            return;
        case kInt2Oid:
            *attlen = 2;
            *attbyval = true;
            *attalign = AttAlign::kShort;
            return;
        case kInt4Oid:
        case kFloat4Oid:
            *attlen = 4;
            *attbyval = true;
            *attalign = AttAlign::kInt;
            return;
        case kInt8Oid:
        case kFloat8Oid:
            *attlen = 8;
            *attbyval = true;
            *attalign = AttAlign::kDouble;
            return;
        case kTextOid:
        case kVarcharOid:
            *attlen = -1;
            *attbyval = false;
            *attalign = AttAlign::kInt;
            return;
        default:
            // Default to int4.
            *attlen = 4;
            *attbyval = true;
            *attalign = AttAlign::kInt;
            return;
    }
}

mytoydb::access::TupleDesc BuildTupleDescFromTargetList(
    const std::vector<TargetEntry*>& targetlist) {
    auto* tupdesc = makePallocNode<mytoydb::access::TupleDescData>();
    tupdesc->natts = static_cast<int>(targetlist.size());

    int attno = 1;
    for (TargetEntry* te : targetlist) {
        FormData_pg_attribute attr;
        attr.attnum = attno;
        attr.attname = te->resname.empty() ? ("col" + std::to_string(attno)) : te->resname;

        // Determine the type from the expression.
        Oid typid = kInt4Oid;
        if (te->expr != nullptr) {
            NodeTag tag = te->expr->GetTag();
            switch (tag) {
                case NodeTag::kVar:
                    typid = static_cast<Var*>(te->expr)->vartype;
                    break;
                case NodeTag::kConst:
                    typid = static_cast<Const*>(te->expr)->consttype;
                    break;
                case NodeTag::kAggref:
                    typid = static_cast<Aggref*>(te->expr)->aggtype;
                    break;
                case NodeTag::kOpExpr:
                    typid = static_cast<OpExpr*>(te->expr)->opresulttype;
                    break;
                default:
                    typid = kInt4Oid;
                    break;
            }
        }
        attr.atttypid = typid;
        FillTypeAttrs(typid, &attr.attlen, &attr.attbyval, &attr.attalign);
        tupdesc->attrs.push_back(attr);
        attno++;
    }
    return tupdesc;
}

int CompareDatumValues(mytoydb::types::Datum a, bool a_null, mytoydb::types::Datum b, bool b_null,
                       Oid typid) {
    // NULLs sort before non-NULLs.
    if (a_null && b_null)
        return 0;
    if (a_null)
        return -1;
    if (b_null)
        return 1;

    switch (typid) {
        case kInt4Oid:
        case kDateOid: {
            int32_t va = DatumGetInt32(a);
            int32_t vb = DatumGetInt32(b);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case kInt8Oid:
        case kTimestampOid: {
            int64_t va = DatumGetInt64(a);
            int64_t vb = DatumGetInt64(b);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case kFloat8Oid: {
            double va = DatumGetFloat8(a);
            double vb = DatumGetFloat8(b);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
            return 0;
        }
        case kBoolOid: {
            bool va = DatumGetBool(a);
            bool vb = DatumGetBool(b);
            if (va == vb)
                return 0;
            return va ? 1 : -1;
        }
        case kTextOid:
        case kVarcharOid: {
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
        default:
            return 0;
    }
}

int CompareTuplesOnAttrs(const TupleTableSlot* a, const TupleTableSlot* b,
                         const std::vector<int>& attnos) {
    if (a == nullptr || b == nullptr)
        return 0;
    auto* tupdesc = a->tts_tupleDescriptor;
    for (int attno : attnos) {
        if (attno < 1)
            continue;
        int idx = attno - 1;
        bool a_null = (idx < a->Natts()) ? a->tts_isnull[idx] : true;
        bool b_null = (idx < b->Natts()) ? b->tts_isnull[idx] : true;
        Oid typid =
            (tupdesc != nullptr && idx < tupdesc->natts) ? tupdesc->attrs[idx].atttypid : kInt4Oid;
        int cmp = CompareDatumValues(a->tts_values[idx], a_null, b->tts_values[idx], b_null, typid);
        if (cmp != 0)
            return cmp;
    }
    return 0;
}

}  // namespace mytoydb::executor
