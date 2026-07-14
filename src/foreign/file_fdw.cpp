// file_fdw.cpp — file_fdw handler implementation (CSV reader).
//
// Converted from PostgreSQL 15's contrib/file_fdw/file_fdw.c.
//
// Implements the FdwRoutine callbacks for file_fdw: reads CSV files and
// converts each line into a tuple. The foreign table's "filename" option
// specifies the file path. Each comma-separated field is converted to the
// column's type using the appropriate *_in function.
#include "foreign/file_fdw.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/error/elog.hpp"
#include "executor/node_foreignscan.hpp"
#include "executor/tupletable.hpp"
#include "foreign/fdwapi.hpp"
#include "foreign/foreign.hpp"
#include "types/builtins.hpp"
#include "types/datum.hpp"

namespace pgcpp::foreign {

namespace {

// FileFdwState — file_fdw's private state, stored in ForeignScanState::fdw_state.
struct FileFdwState {
    std::ifstream file;
    std::string filename;
};

// Split a CSV line on commas into fields. No quoting/escaping support
// (documented limitation).
std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream iss(line);
    while (std::getline(iss, field, ',')) {
        fields.push_back(field);
    }
    // Handle trailing empty field (e.g., "a,b," → ["a", "b", ""]).
    if (!line.empty() && line.back() == ',') {
        fields.push_back("");
    }
    // Handle empty line → one empty field.
    if (fields.empty()) {
        fields.push_back("");
    }
    return fields;
}

// Convert a string field to a Datum based on the column's type OID.
// Returns the Datum and sets *isnull. An empty string is treated as NULL.
pgcpp::types::Datum ConvertField(const std::string& field, pgcpp::catalog::Oid atttypid,
                                 bool* isnull) {
    if (field.empty()) {
        *isnull = true;
        return 0;
    }
    *isnull = false;
    using namespace pgcpp::types;
    switch (atttypid) {
        case kBoolOid:
            return bool_in(field.c_str());
        case kInt2Oid:
            return int2_in(field.c_str());
        case kInt4Oid:
            return int4_in(field.c_str());
        case kInt8Oid:
            return int8_in(field.c_str());
        case kFloat4Oid:
            return float8_in(field.c_str());  // reuse float8_in for float4
        case kFloat8Oid:
            return float8_in(field.c_str());
        case kTextOid:
        case kVarcharOid:
            return text_in(field.c_str());
        default:
            // Unknown type: treat as text.
            return text_in(field.c_str());
    }
}

// --- FdwRoutine callbacks ---

void FileBeginForeignScan(pgcpp::executor::ForeignScanState* state, uint32_t foreigntableid) {
    // Look up the foreign table to get the filename option.
    const auto* ft = LookupForeignTable(foreigntableid);
    if (ft == nullptr) {
        char errbuf[256];
        std::snprintf(errbuf, sizeof(errbuf), "file_fdw: foreign table OID %u not found",
                      foreigntableid);
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    const std::string* filename = GetOption(ft->options, "filename");
    if (filename == nullptr || filename->empty()) {
        ereport(pgcpp::error::LogLevel::kError, "file_fdw: \"filename\" option is required");
    }

    auto* fstate = new FileFdwState();
    fstate->filename = *filename;
    fstate->file.open(fstate->filename);
    if (!fstate->file.is_open()) {
        char errbuf[512];
        std::snprintf(errbuf, sizeof(errbuf), "file_fdw: could not open file \"%s\"",
                      fstate->filename.c_str());
        delete fstate;
        ereport(pgcpp::error::LogLevel::kError, errbuf);
    }
    state->fdw_state = fstate;
}

pgcpp::executor::TupleTableSlot* FileIterateForeignScan(pgcpp::executor::ForeignScanState* state) {
    auto* fstate = static_cast<FileFdwState*>(state->fdw_state);
    if (fstate == nullptr || !fstate->file.is_open()) {
        return nullptr;
    }

    std::string line;
    if (!std::getline(fstate->file, line)) {
        return nullptr;  // EOF
    }

    // Split the line into CSV fields.
    std::vector<std::string> fields = SplitCsv(line);

    // Get the scan slot and its descriptor.
    auto* slot = state->fs_ScanTupleSlot;
    int natts = slot->Natts();

    // Convert each field to a Datum based on the column type.
    // Use unique_ptr<bool[]> instead of std::vector<bool> (which doesn't
    // support &isnull[i] due to its bit-packing specialization).
    std::vector<pgcpp::types::Datum> values(natts, 0);
    auto isnull = std::make_unique<bool[]>(natts);
    for (int i = 0; i < natts; i++) {
        isnull[i] = true;  // default to NULL
        if (i < static_cast<int>(fields.size())) {
            bool null_flag = false;
            values[i] =
                ConvertField(fields[i], slot->tts_tupleDescriptor->attrs[i].atttypid, &null_flag);
            isnull[i] = null_flag;
        }
    }

    slot->StoreVirtual(values.data(), isnull.get());
    return slot;
}

void FileReScanForeignScan(pgcpp::executor::ForeignScanState* state) {
    auto* fstate = static_cast<FileFdwState*>(state->fdw_state);
    if (fstate != nullptr && fstate->file.is_open()) {
        fstate->file.clear();  // clear EOF flag
        fstate->file.seekg(0);
    }
}

void FileEndForeignScan(pgcpp::executor::ForeignScanState* state) {
    auto* fstate = static_cast<FileFdwState*>(state->fdw_state);
    if (fstate != nullptr) {
        if (fstate->file.is_open()) {
            fstate->file.close();
        }
        delete fstate;
        state->fdw_state = nullptr;
    }
}

// The static FdwRoutine returned by the factory.
const FdwRoutine kFileFdwRoutine = {
    FileBeginForeignScan,
    FileIterateForeignScan,
    FileReScanForeignScan,
    FileEndForeignScan,
};

const FdwRoutine* FileFdwFactory() {
    return &kFileFdwRoutine;
}

}  // namespace

void RegisterFileFdw() {
    RegisterFdw("file_fdw", &FileFdwFactory);
}

}  // namespace pgcpp::foreign
