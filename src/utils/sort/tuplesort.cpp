// tuplesort.cpp — External merge sort for tuples (P1-3).
//
// Converted from PostgreSQL 15's src/backend/utils/sort/tuplesort.c.
//
// TupleSort collects tuples from a child plan node, sorts them by the
// specified sort keys, and returns them one at a time. When the input
// exceeds work_mem, tuples are spilled to disk using tape-based external
// merge sort (initial runs + k-way merge).
//
// MVP scope:
// - In-memory sort when data fits in work_mem (std::sort, same as before).
// - External merge sort when data exceeds work_mem (initial runs written
//   to temp files, k-way merge via linear scan during output).
// - Top-N optimization is NOT implemented here (LIMIT applied after sort).
// - Tuplestore is NOT implemented (P1-4 will add it for HashAgg/HashJoin).
#include "utils/sort/tuplesort.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"

namespace pgcpp::sort {

using pgcpp::access::TupleDesc;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::Oid;
using pgcpp::error::LogLevel;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::palloc;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetBool;
using pgcpp::types::DatumGetFloat8;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::DatumGetInt64;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::kBoolOid;
using pgcpp::types::kDateOid;
using pgcpp::types::kFloat8Oid;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kInt8Oid;
using pgcpp::types::kTextOid;
using pgcpp::types::kTimestampOid;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE;
using pgcpp::types::VARSIZE_DATA;

namespace {

// Compare two Datum values of the given type.
// Returns -1 if a < b, 0 if a == b, 1 if a > b.
// Mirrors the CompareValues() in node_sort.cpp (kept duplicated for now;
// P1-4 will factor this out into a shared comparator).
int CompareValues(Datum a, Datum b, Oid typid) {
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
        case kTextOid: {
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

// Write a POD value to a FILE*. Aborts with ereport on short write.
template<typename T>
void WritePod(std::FILE* f, const T& value) {
    if (std::fwrite(&value, sizeof(T), 1, f) != 1) {
        ereport(LogLevel::kError, std::string("tuplesort: write failed: ") + std::strerror(errno));
    }
}

// Read a POD value from a FILE*. Returns false on EOF (no bytes read).
template<typename T>
bool ReadPod(std::FILE* f, T& value) {
    size_t n = std::fread(&value, sizeof(T), 1, f);
    if (n == 1)
        return true;
    if (n == 0 && std::feof(f))
        return false;
    ereport(LogLevel::kError, std::string("tuplesort: read failed: ") + std::strerror(errno));
}

}  // namespace

// =========================================================================
// LogicalTape implementation
// =========================================================================

LogicalTape::LogicalTape(const std::string& path, TupleDesc tupdesc)
    : path_(path), tupdesc_(tupdesc) {
    file_ = std::fopen(path.c_str(), "w+b");
    if (file_ == nullptr) {
        ereport(LogLevel::kError, std::string("tuplesort: cannot create temp file ") + path + ": " +
                                      std::strerror(errno));
    }
}

LogicalTape::~LogicalTape() {
    Close();
}

void LogicalTape::WriteTuple(const TupleTableSlot* slot) {
    int natts = slot->Natts();
    WritePod(file_, natts);
    for (int i = 0; i < natts; i++) {
        bool is_null = slot->tts_isnull[i];
        WritePod(file_, is_null);
        if (is_null)
            continue;
        const FormData_pg_attribute* attr = tupdesc_ ? tupdesc_->Attr(i + 1) : nullptr;
        Datum value = slot->tts_values[i];
        if (attr != nullptr && attr->attbyval) {
            // Pass-by-value: write attlen bytes (1, 2, 4, or 8).
            int16_t len = attr->attlen;
            WritePod(file_, len);
            switch (len) {
                case 1: {
                    uint8_t v = static_cast<uint8_t>(value);
                    WritePod(file_, v);
                    break;
                }
                case 2: {
                    uint16_t v = static_cast<uint16_t>(value);
                    WritePod(file_, v);
                    break;
                }
                case 4: {
                    uint32_t v = static_cast<uint32_t>(value);
                    WritePod(file_, v);
                    break;
                }
                case 8:
                default: {
                    uint64_t v = static_cast<uint64_t>(value);
                    WritePod(file_, v);
                    break;
                }
            }
        } else {
            // Pass-by-reference (text/varchar): Datum is a pointer to a
            // varlena structure (4-byte length prefix + data).
            const char* ptr = DatumGetTextP(value);
            int32_t total_len = VARSIZE(ptr);
            WritePod(file_, total_len);
            if (std::fwrite(VARDATA(ptr), 1, static_cast<size_t>(total_len - sizeof(int32_t)),
                            file_) != static_cast<size_t>(total_len - sizeof(int32_t))) {
                ereport(LogLevel::kError,
                        std::string("tuplesort: write data failed: ") + std::strerror(errno));
            }
        }
    }
    num_tuples_++;
}

void LogicalTape::Rewind() {
    if (std::fseek(file_, 0, SEEK_SET) != 0) {
        ereport(LogLevel::kError, std::string("tuplesort: rewind failed: ") + std::strerror(errno));
    }
    writing_ = false;
}

TupleTableSlot* LogicalTape::ReadTuple() {
    int natts;
    if (!ReadPod(file_, natts)) {
        return nullptr;  // end-of-tape
    }
    TupleTableSlot* slot = TupleTableSlot::Make(tupdesc_);
    for (int i = 0; i < natts; i++) {
        bool is_null;
        if (!ReadPod(file_, is_null)) {
            ereport(LogLevel::kError, std::string("tuplesort: unexpected EOF reading tuple"));
        }
        slot->tts_isnull[i] = is_null;
        if (is_null) {
            slot->tts_values[i] = 0;
            continue;
        }
        const FormData_pg_attribute* attr = tupdesc_ ? tupdesc_->Attr(i + 1) : nullptr;
        if (attr != nullptr && attr->attbyval) {
            // byval: read attlen bytes into the Datum.
            int16_t len;
            if (!ReadPod(file_, len)) {
                ereport(LogLevel::kError, std::string("tuplesort: unexpected EOF reading length"));
            }
            Datum value = 0;
            size_t got = std::fread(&value, 1, static_cast<size_t>(len), file_);
            if (got != static_cast<size_t>(len)) {
                ereport(LogLevel::kError,
                        std::string("tuplesort: unexpected EOF reading byval datum"));
            }
            slot->tts_values[i] = value;
        } else {
            // byref: read the total varlena length, allocate a buffer,
            // set the length prefix, and read the data.
            int32_t total_len;
            if (!ReadPod(file_, total_len)) {
                ereport(LogLevel::kError,
                        std::string("tuplesort: unexpected EOF reading varlena length"));
            }
            char* buf = static_cast<char*>(palloc(total_len));
            int32_t len = total_len;
            std::memcpy(buf, &len, sizeof(int32_t));
            size_t want = static_cast<size_t>(total_len - sizeof(int32_t));
            size_t got = std::fread(buf + sizeof(int32_t), 1, want, file_);
            if (got != want) {
                ereport(LogLevel::kError,
                        std::string("tuplesort: unexpected EOF reading byref datum"));
            }
            slot->tts_values[i] = reinterpret_cast<Datum>(buf);
        }
    }
    slot->tts_nvalid = true;
    slot->tts_isempty = false;
    return slot;
}

void LogicalTape::Close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        std::remove(path_.c_str());
        path_.clear();
    }
}

// =========================================================================
// TupleSort implementation
// =========================================================================

TupleSort::TupleSort(TupleDesc tupdesc, std::vector<SortKey> keys, size_t work_mem,
                     const std::string& temp_dir)
    : tupdesc_(tupdesc), keys_(std::move(keys)), work_mem_(work_mem), temp_dir_(temp_dir) {
    if (tupdesc_ != nullptr) {
        output_slot_ = TupleTableSlot::Make(tupdesc_);
    }
}

TupleSort::~TupleSort() {
    for (TupleTableSlot* slot : mem_tuples_) {
        destroyPallocNode(slot);
    }
    mem_tuples_.clear();
    tapes_.clear();  // unique_ptr auto-closes LogicalTape (deletes temp file)
    if (output_slot_ != nullptr) {
        destroyPallocNode(output_slot_);
        output_slot_ = nullptr;
    }
}

void TupleSort::PutTuple(TupleTableSlot* slot) {
    // Copy the slot so the caller can reuse/free the original. For byref
    // types, deep-copy the varlena buffer so the copy is independent.
    TupleTableSlot* copy = TupleTableSlot::Make(slot->tts_tupleDescriptor);
    copy->StoreVirtual(slot->tts_values, slot->tts_isnull);
    int natts = copy->Natts();
    for (int i = 0; i < natts; i++) {
        if (copy->tts_isnull[i])
            continue;
        const FormData_pg_attribute* attr =
            copy->tts_tupleDescriptor ? copy->tts_tupleDescriptor->Attr(i + 1) : nullptr;
        if (attr != nullptr && !attr->attbyval) {
            const char* ptr = DatumGetTextP(copy->tts_values[i]);
            int32_t total_len = VARSIZE(ptr);
            char* buf = static_cast<char*>(palloc(total_len));
            std::memcpy(buf, ptr, total_len);
            copy->tts_values[i] = reinterpret_cast<Datum>(buf);
        }
    }
    mem_tuples_.push_back(copy);
    mem_used_ += EstimateSlotSize(copy);

    // Spill to disk when work_mem is exceeded.
    if (mem_used_ > work_mem_) {
        DumpRun();
    }
}

size_t TupleSort::EstimateSlotSize(const TupleTableSlot* slot) const {
    size_t size = sizeof(TupleTableSlot);
    int natts = slot->Natts();
    size += sizeof(Datum) * natts;
    size += sizeof(bool) * natts;
    for (int i = 0; i < natts; i++) {
        if (slot->tts_isnull[i])
            continue;
        const FormData_pg_attribute* attr =
            slot->tts_tupleDescriptor ? slot->tts_tupleDescriptor->Attr(i + 1) : nullptr;
        if (attr != nullptr && !attr->attbyval) {
            const char* ptr = DatumGetTextP(slot->tts_values[i]);
            size += VARSIZE(ptr);
        }
    }
    return size;
}

int TupleSort::Compare(const TupleTableSlot* a, const TupleTableSlot* b) const {
    for (const auto& key : keys_) {
        int attno = key.attnum;
        bool a_null = (attno >= 1 && attno <= a->Natts()) ? a->tts_isnull[attno - 1] : true;
        bool b_null = (attno >= 1 && attno <= b->Natts()) ? b->tts_isnull[attno - 1] : true;

        if (a_null && b_null)
            continue;
        if (a_null) {
            return key.nulls_first ? -1 : 1;
        }
        if (b_null) {
            return key.nulls_first ? 1 : -1;
        }

        int cmp = CompareValues(a->tts_values[attno - 1], b->tts_values[attno - 1], key.typid);
        if (cmp == 0)
            continue;
        if (key.reverse)
            cmp = -cmp;
        return cmp;
    }
    return 0;
}

void TupleSort::SortInMemory() {
    std::sort(mem_tuples_.begin(), mem_tuples_.end(),
              [this](TupleTableSlot* a, TupleTableSlot* b) { return Compare(a, b) < 0; });
}

void TupleSort::DumpRun() {
    if (mem_tuples_.empty())
        return;
    SortInMemory();
    auto tape = std::make_unique<LogicalTape>(MakeTempPath(), tupdesc_);
    for (TupleTableSlot* slot : mem_tuples_) {
        tape->WriteTuple(slot);
    }
    tapes_.push_back(std::move(tape));
    for (TupleTableSlot* slot : mem_tuples_) {
        destroyPallocNode(slot);
    }
    mem_tuples_.clear();
    mem_used_ = 0;
}

void TupleSort::PerformSort() {
    if (state_ == State::kSorted) {
        ereport(LogLevel::kError, std::string("tuplesort: PerformSort called twice"));
    }
    if (tapes_.empty()) {
        // All tuples fit in memory — sort in place.
        SortInMemory();
    } else {
        // Spilled at least once. Dump the remaining in-memory tuples as
        // the last run, then begin the k-way merge.
        if (!mem_tuples_.empty()) {
            SortInMemory();
            auto tape = std::make_unique<LogicalTape>(MakeTempPath(), tupdesc_);
            for (TupleTableSlot* slot : mem_tuples_) {
                tape->WriteTuple(slot);
            }
            tapes_.push_back(std::move(tape));
            for (TupleTableSlot* slot : mem_tuples_) {
                destroyPallocNode(slot);
            }
            mem_tuples_.clear();
            mem_used_ = 0;
        }
        BeginMerge();
    }
    state_ = State::kSorted;
}

void TupleSort::BeginMerge() {
    for (auto& tape : tapes_) {
        tape->Rewind();
    }
    merge_heap_.clear();
    for (size_t i = 0; i < tapes_.size(); i++) {
        TupleTableSlot* slot = tapes_[i]->ReadTuple();
        if (slot != nullptr) {
            merge_heap_.push_back(MergeSlot{static_cast<int>(i + 1), slot});
        }
    }
}

TupleTableSlot* TupleSort::GetTupleFromMerge() {
    if (merge_heap_.empty()) {
        return nullptr;
    }
    // Find the minimum MergeSlot (linear scan — N tapes is typically small).
    size_t min_idx = 0;
    for (size_t i = 1; i < merge_heap_.size(); i++) {
        if (Compare(merge_heap_[i].slot, merge_heap_[min_idx].slot) < 0) {
            min_idx = i;
        }
    }
    int source_id = merge_heap_[min_idx].source_id;
    TupleTableSlot* min_slot = merge_heap_[min_idx].slot;

    // Copy the values into the reusable output slot.
    int natts = output_slot_->Natts();
    int src_natts = min_slot->Natts();
    int ncopy = natts < src_natts ? natts : src_natts;
    for (int i = 0; i < ncopy; i++) {
        output_slot_->tts_values[i] = min_slot->tts_values[i];
        output_slot_->tts_isnull[i] = min_slot->tts_isnull[i];
    }
    output_slot_->tts_nvalid = true;
    output_slot_->tts_isempty = false;

    // Advance the source: read the next tuple from the same tape.
    TupleTableSlot* next = tapes_[source_id - 1]->ReadTuple();
    destroyPallocNode(min_slot);
    if (next == nullptr) {
        merge_heap_[min_idx] = merge_heap_.back();
        merge_heap_.pop_back();
    } else {
        merge_heap_[min_idx].slot = next;
    }
    return output_slot_;
}

TupleTableSlot* TupleSort::GetTuple() {
    if (state_ != State::kSorted) {
        ereport(LogLevel::kError, std::string("tuplesort: GetTuple called before PerformSort"));
    }
    if (tapes_.empty()) {
        // In-memory case: return directly from mem_tuples_.
        if (mem_output_index_ >= mem_tuples_.size()) {
            return nullptr;
        }
        return mem_tuples_[mem_output_index_++];
    }
    return GetTupleFromMerge();
}

std::string TupleSort::MakeTempPath() {
    return temp_dir_ + "/pgcpp_tuplesort_" + std::to_string(getpid()) + "_" +
           std::to_string(tape_counter_++) + ".tmp";
}

}  // namespace pgcpp::sort
