// copy_from.cpp — COPY FROM implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/copyfrom.c.
// Reads tab-delimited text rows from a file and inserts them into a
// relation using heap_insert.
#include "mytoydb/commands/copy_from.hpp"

#include <cstdint>
#include <fstream>
#include <string>

#include "mytoydb/access/heapam.hpp"
#include "mytoydb/access/rel.hpp"
#include "mytoydb/catalog/catalog.hpp"
#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"
#include "mytoydb/storage/bufmgr.hpp"
#include "mytoydb/transaction/heap_tuple.hpp"
#include "mytoydb/types/builtins.hpp"
#include "mytoydb/types/datum.hpp"

namespace mytoydb::commands {

using mytoydb::access::heap_form_tuple;
using mytoydb::access::heap_freetuple;
using mytoydb::access::heap_insert;
using mytoydb::access::Relation;
using mytoydb::access::RelationData;
using mytoydb::access::TupleDesc;
using mytoydb::catalog::Oid;
using mytoydb::memory::palloc;
using mytoydb::memory::pfree;
using mytoydb::transaction::HeapTuple;
using mytoydb::types::Datum;
using mytoydb::types::kInt2Oid;
using mytoydb::types::kInt4Oid;
using mytoydb::types::kInt8Oid;
using mytoydb::types::kTextOid;
using mytoydb::types::kVarcharOid;

namespace {

// Convert a text value to a Datum based on the type OID (for COPY FROM).
Datum TextToDatum(const std::string& text, Oid type_oid) {
    switch (type_oid) {
        case kInt2Oid:
        case kInt4Oid:
            return mytoydb::types::int4_in(text.c_str());
        case kInt8Oid:
            return mytoydb::types::int8_in(text.c_str());
        case kTextOid:
        case kVarcharOid:
            return mytoydb::types::text_in(text.c_str());
        default:
            return mytoydb::types::int4_in(text.c_str());
    }
}

}  // namespace

int64_t CopyFromText(RelationData* rel, const std::string& filename) {
    TupleDesc tupdesc = rel->rd_att;
    int natts = tupdesc->natts;

    Datum* values = static_cast<Datum*>(palloc(sizeof(Datum) * natts));
    bool* isnull = static_cast<bool*>(palloc(sizeof(bool) * natts));

    std::ifstream in(filename);
    if (!in.is_open()) {
        pfree(isnull);
        pfree(values);
        ereport(mytoydb::error::LogLevel::kError, "could not open file \"" + filename + "\"");
    }

    int64_t row_count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        for (int i = 0; i < natts; ++i) {
            values[i] = 0;
            isnull[i] = false;
        }
        std::size_t pos = 0;
        for (int i = 0; i < natts; ++i) {
            std::size_t tab = line.find('\t', pos);
            std::string field =
                (tab == std::string::npos) ? line.substr(pos) : line.substr(pos, tab - pos);
            if (field.empty()) {
                isnull[i] = true;
            } else {
                values[i] = TextToDatum(field, tupdesc->attrs[i].atttypid);
            }
            if (tab == std::string::npos)
                break;
            pos = tab + 1;
        }
        HeapTuple tup = heap_form_tuple(tupdesc, values, isnull);
        heap_insert(rel, tup);
        heap_freetuple(tup);
        row_count++;
    }

    pfree(isnull);
    pfree(values);
    return row_count;
}

}  // namespace mytoydb::commands
