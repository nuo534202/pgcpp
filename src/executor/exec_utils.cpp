// exec_utils.cpp — Shared executor utility helpers.
#include "mytoydb/executor/exec_utils.h"

#include <new>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/common/memory/memory_context.h"
#include "mytoydb/types/datum.h"

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
using mytoydb::types::kBoolOid;
using mytoydb::types::kFloat4Oid;
using mytoydb::types::kFloat8Oid;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kVarcharOid;

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

}  // namespace mytoydb::executor
