// copy_to.cpp — COPY TO implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/copyto.c.
// Scans a relation and writes rows as tab-delimited text to a file.
#include "pgcpp/commands/copy_to.hpp"

#include <cstdint>
#include <fstream>
#include <string>

#include "pgcpp/access/heapam.hpp"
#include "pgcpp/access/rel.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/storage/bufmgr.hpp"
#include "pgcpp/transaction/heap_tuple.hpp"
#include "pgcpp/transaction/snapshot.hpp"
#include "pgcpp/types/builtins.hpp"
#include "pgcpp/types/datum.hpp"

namespace pgcpp::commands {

using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_deform_tuple;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_getnext;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::RelationData;
using pgcpp::access::TupleDesc;
using pgcpp::memory::palloc;
using pgcpp::memory::pfree;
using pgcpp::transaction::GetTransactionSnapshot;
using pgcpp::transaction::HeapTuple;
using pgcpp::types::Datum;

int64_t CopyToText(RelationData* rel, const std::string& filename) {
    TupleDesc tupdesc = rel->rd_att;
    int natts = tupdesc->natts;

    Datum* values = static_cast<Datum*>(palloc(sizeof(Datum) * natts));
    bool* isnull = static_cast<bool*>(palloc(sizeof(bool) * natts));

    std::ofstream out(filename);
    if (!out.is_open()) {
        pfree(isnull);
        pfree(values);
        ereport(pgcpp::error::LogLevel::kError, "could not open file \"" + filename + "\"");
    }

    int64_t row_count = 0;
    pgcpp::transaction::Snapshot snap = GetTransactionSnapshot();
    HeapScanDesc scan = heap_beginscan(rel, snap);
    HeapTuple tup;
    while ((tup = heap_getnext(scan)) != nullptr) {
        heap_deform_tuple(tup, tupdesc, values, isnull);
        std::string line;
        for (int i = 0; i < natts; ++i) {
            if (i > 0)
                line.push_back('\t');
            if (isnull[i]) {
                // empty field represents NULL
            } else {
                line += pgcpp::types::int4_out(values[i]);  // placeholder; real impl uses type out
            }
        }
        out << line << "\n";
        row_count++;
    }
    heap_endscan(scan);

    pfree(isnull);
    pfree(values);
    return row_count;
}

}  // namespace pgcpp::commands
