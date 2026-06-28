// copy_from.cpp — COPY FROM implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/copyfrom.c.
// Reads tab-delimited text rows from a file and inserts them into a
// relation using heap_insert.
#include "pgcpp/commands/copy_from.hpp"

#include <cstdint>
#include <fstream>
#include <string>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/storage/bufmgr.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/types/builtins.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::commands {

using pgcpp::access::heap_form_tuple;
using pgcpp::access::heap_freetuple;
using pgcpp::access::heap_insert;
using pgcpp::access::Relation;
using pgcpp::access::RelationData;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::Oid;
using pgcpp::memory::palloc;
using pgcpp::memory::pfree;
using pgcpp::transaction::HeapTuple;
using pgcpp::types::Datum;
using pgcpp::types::kInt2Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kVarcharOid;

namespace {

// Convert a text value to a Datum based on the type OID (for COPY FROM).
Datum TextToDatum(const std::string& text, Oid type_oid) {
    switch (type_oid) {
        case kInt2Oid:
        case kInt4Oid:
            return pgcpp::types::int4_in(text.c_str());
        case kInt8Oid:
            return pgcpp::types::int8_in(text.c_str());
        case kTextOid:
        case kVarcharOid:
            return pgcpp::types::text_in(text.c_str());
        default:
            return pgcpp::types::int4_in(text.c_str());
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
        ereport(pgcpp::error::LogLevel::kError, "could not open file \"" + filename + "\"");
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

}  // namespace pgcpp::commands
