// tupdesc.cpp — Tuple descriptor construction utilities.
//
// Converted from PostgreSQL 15's src/backend/access/common/tupdesc.c.
//
// Provides CreateTemplateTupleDesc, CreateTupleDescCopy/CopyConstr,
// TupleDescCopyEntry, FreeTupleDesc, equalTupleDescs, and TupleDescInitEntry.
// Type metadata for TupleDescInitEntry is resolved via the catalog's pg_type
// lookup, with a hardcoded fallback for common built-in types so that tests
// can construct descriptors without bootstrapping the full catalog.

#include <string>

#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/catalog/pg_type.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::access {
using pgcpp::catalog::AttAlign;
using pgcpp::catalog::AttStorage;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::TypeAlign;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::nodes::makePallocNode;
using pgcpp::types::kBoolOid;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat4Oid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt2Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;
using pgcpp::types::kTimestamptzOid;
using pgcpp::types::kVarcharOid;

namespace {

struct TypeMeta {
    int16_t typlen = 0;
    bool typbyval = false;
    AttAlign attalign = AttAlign::kInt;
};

AttAlign MapTypeAlign(TypeAlign a) {
    switch (a) {
        case TypeAlign::kChar:
            return AttAlign::kChar;
        case TypeAlign::kShort:
            return AttAlign::kShort;
        case TypeAlign::kInt:
            return AttAlign::kInt;
        case TypeAlign::kDouble:
            return AttAlign::kDouble;
    }
    return AttAlign::kInt;
}

// Resolve type metadata (typlen/typbyval/attalign) for the given type OID.
// Returns true on success. Falls back to hardcoded values for common built-in
// types when the catalog is not populated (e.g. in unit tests).
bool LookupTypeMeta(Oid type_oid, TypeMeta* out) {
    Catalog* cat = GetCatalog();
    if (cat != nullptr) {
        const FormData_pg_type* t = cat->GetTypeByOid(type_oid);
        if (t != nullptr) {
            out->typlen = t->typlen;
            out->typbyval = t->typbyval;
            out->attalign = MapTypeAlign(t->typalign);
            return true;
        }
    }
    switch (type_oid) {
        case kBoolOid:
            out->typlen = 1;
            out->typbyval = true;
            out->attalign = AttAlign::kChar;
            return true;
        case kInt2Oid:
            out->typlen = 2;
            out->typbyval = true;
            out->attalign = AttAlign::kShort;
            return true;
        case kInt4Oid:
        case kDateOid:
        case kFloat4Oid:
            out->typlen = 4;
            out->typbyval = true;
            out->attalign = AttAlign::kInt;
            return true;
        case kInt8Oid:
        case kTimestampOid:
        case kTimestamptzOid:
        case kFloat8Oid:
            out->typlen = 8;
            out->typbyval = true;
            out->attalign = AttAlign::kDouble;
            return true;
        case kTextOid:
        case kVarcharOid:
            out->typlen = -1;
            out->typbyval = false;
            out->attalign = AttAlign::kInt;
            return true;
        default:
            return false;
    }
}

}  // namespace

TupleDesc CreateTemplateTupleDesc(int natts) {
    TupleDesc desc = makePallocNode<TupleDescData>();
    desc->natts = natts;
    desc->attrs.resize(natts);
    return desc;
}

TupleDesc CreateTupleDescCopy(TupleDesc tupdesc) {
    if (tupdesc == nullptr)
        return nullptr;
    TupleDesc desc = makePallocNode<TupleDescData>();
    desc->natts = tupdesc->natts;
    desc->attrs = tupdesc->attrs;
    desc->tdtypeid = tupdesc->tdtypeid;
    desc->tdtypmod = tupdesc->tdtypmod;
    desc->tdhasoid = tupdesc->tdhasoid;
    // Constraints are NOT copied (PG semantics: CreateTupleDescCopy drops them).
    return desc;
}

TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc) {
    if (tupdesc == nullptr)
        return nullptr;
    TupleDesc desc = CreateTupleDescCopy(tupdesc);
    desc->constr = tupdesc->constr;
    return desc;
}

void TupleDescCopyEntry(TupleDesc dst, int dst_attnum, TupleDesc src, int src_attnum) {
    if (dst == nullptr || src == nullptr)
        return;
    if (dst_attnum < 1 || dst_attnum > dst->natts)
        return;
    if (src_attnum < 1 || src_attnum > src->natts)
        return;
    dst->attrs[dst_attnum - 1] = src->attrs[src_attnum - 1];
    // Update the attnum to match the destination slot.
    dst->attrs[dst_attnum - 1].attnum = static_cast<int16_t>(dst_attnum);
}

void FreeTupleDesc(TupleDesc tupdesc) {
    if (tupdesc == nullptr)
        return;
    // Refcount > 0 means the descriptor is still shared; decrement and keep
    // it alive (mirrors PG's DecrTupleDescRefCount + FreeTupleDesc split:
    // only when the refcount is already <= 0 do we actually free).
    if (tupdesc->tdrefcount > 0) {
        tupdesc->tdrefcount--;
        return;
    }
    destroyPallocNode(tupdesc);
}

bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2) {
    if (tupdesc1 == tupdesc2)
        return true;
    if (tupdesc1 == nullptr || tupdesc2 == nullptr)
        return false;
    if (tupdesc1->natts != tupdesc2->natts)
        return false;
    if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
        return false;
    if (tupdesc1->tdtypmod != tupdesc2->tdtypmod)
        return false;
    if (tupdesc1->tdhasoid != tupdesc2->tdhasoid)
        return false;

    for (int i = 0; i < tupdesc1->natts; i++) {
        const auto& a1 = tupdesc1->attrs[i];
        const auto& a2 = tupdesc2->attrs[i];
        if (a1.attname != a2.attname)
            return false;
        if (a1.atttypid != a2.atttypid)
            return false;
        if (a1.attlen != a2.attlen)
            return false;
        if (a1.attnum != a2.attnum)
            return false;
        if (a1.attbyval != a2.attbyval)
            return false;
        if (a1.attalign != a2.attalign)
            return false;
        if (a1.attstorage != a2.attstorage)
            return false;
        if (a1.attisdropped != a2.attisdropped)
            return false;
        if (a1.attcollation != a2.attcollation)
            return false;
        if (a1.attnotnull != a2.attnotnull)
            return false;
        if (a1.atttypmod != a2.atttypmod)
            return false;
    }

    if (tupdesc1->constr.has_not_null != tupdesc2->constr.has_not_null)
        return false;
    if (tupdesc1->constr.defval.size() != tupdesc2->constr.defval.size())
        return false;
    if (tupdesc1->constr.check.size() != tupdesc2->constr.check.size())
        return false;
    return true;
}

void TupleDescInitEntry(TupleDesc desc, int attnum, const std::string& name, Oid type_oid,
                        int32_t typmod, int attdim) {
    if (desc == nullptr)
        return;
    if (attnum < 1 || attnum > desc->natts) {
        ereport(pgcpp::error::LogLevel::kError,
                "TupleDescInitEntry: attnum " + std::to_string(attnum) +
                    " out of range (natts=" + std::to_string(desc->natts) + ")");
    }
    FormData_pg_attribute& attr = desc->attrs[attnum - 1];
    attr.attrelid = kInvalidOid;
    attr.attname = name;
    attr.atttypid = type_oid;
    attr.attstattarget = -1;
    attr.attlen = 0;
    attr.attnum = static_cast<int16_t>(attnum);
    attr.attndims = static_cast<int16_t>(attdim);
    attr.attcacheoff = -1;
    attr.atttypmod = typmod;
    attr.attbyval = false;
    attr.attstorage = AttStorage::kPlain;
    attr.attalign = AttAlign::kInt;
    attr.attnotnull = false;
    attr.atthasdef = false;
    attr.atthasmissing = false;
    attr.attidentity = '\0';
    attr.attgenerated = '\0';
    attr.attisdropped = false;
    attr.attislocal = true;
    attr.attinhcount = 0;
    attr.attcollation = kInvalidOid;

    TypeMeta meta;
    if (!LookupTypeMeta(type_oid, &meta)) {
        ereport(pgcpp::error::LogLevel::kWarning, "TupleDescInitEntry: type OID " +
                                                      std::to_string(type_oid) +
                                                      " not in catalog; using varlena defaults");
        meta.typlen = -1;
        meta.typbyval = false;
        meta.attalign = AttAlign::kInt;
    }
    attr.attlen = meta.typlen;
    attr.attbyval = meta.typbyval;
    attr.attalign = meta.attalign;
}

void TupleDescInitEntryCollation(TupleDesc desc, int attnum, Oid collation) {
    if (desc == nullptr)
        return;
    if (attnum < 1 || attnum > desc->natts)
        return;
    desc->attrs[attnum - 1].attcollation = collation;
}

}  // namespace pgcpp::access
