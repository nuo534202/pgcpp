// tuplestore.cpp — Append-only tuple store with disk spill (P1-4).
//
// Converted from PostgreSQL 15's src/backend/utils/sort/tuplestore.c.
//
// The tuplestore keeps tuples in memory until work_mem is exceeded, then
// spills all buffered tuples to a temp file and appends directly to the
// file thereafter. Tuples are read back sequentially via GetTuple();
// Rewind() restarts the read.
#include "utils/sort/tuplestore.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include "catalog/pg_attribute.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"
#include "executor/tupletable.hpp"
#include "types/datum.hpp"

namespace pgcpp::sort {

using pgcpp::access::TupleDesc;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::error::LogLevel;
using pgcpp::executor::TupleTableSlot;
using pgcpp::memory::palloc;
using pgcpp::nodes::destroyPallocNode;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetTextP;
using pgcpp::types::VARDATA;
using pgcpp::types::VARSIZE;

namespace {

// Write a POD value to a FILE*. Aborts with ereport on short write.
template<typename T>
void WritePod(std::FILE* f, const T& value) {
    if (std::fwrite(&value, sizeof(T), 1, f) != 1) {
        ereport(LogLevel::kError, std::string("tuplestore: write failed: ") + std::strerror(errno));
    }
}

// Read a POD value from a FILE*. Returns false on EOF.
template<typename T>
bool ReadPod(std::FILE* f, T& value) {
    size_t n = std::fread(&value, sizeof(T), 1, f);
    if (n == 1)
        return true;
    if (n == 0 && std::feof(f))
        return false;
    ereport(LogLevel::kError, std::string("tuplestore: read failed: ") + std::strerror(errno));
}

}  // namespace

Tuplestore::Tuplestore(TupleDesc tupdesc, size_t work_mem, const std::string& temp_dir)
    : tupdesc_(tupdesc), work_mem_(work_mem), temp_dir_(temp_dir) {
    if (tupdesc_ != nullptr) {
        read_slot_ = TupleTableSlot::Make(tupdesc_);
    }
}

Tuplestore::~Tuplestore() {
    for (TupleTableSlot* slot : mem_tuples_) {
        destroyPallocNode(slot);
    }
    mem_tuples_.clear();
    if (read_slot_ != nullptr) {
        destroyPallocNode(read_slot_);
        read_slot_ = nullptr;
    }
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        std::remove(path_.c_str());
        path_.clear();
    }
}

void Tuplestore::PutTuple(const TupleTableSlot* slot) {
    // Deep-copy the slot so the caller can reuse/free the original.
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

    if (spilled_) {
        // Already spilled: write directly to file.
        WriteTupleToFile(copy);
        destroyPallocNode(copy);
    } else {
        mem_tuples_.push_back(copy);
        // Estimate memory: slot struct + Datum/isnull arrays + byref data.
        size_t sz = sizeof(TupleTableSlot) + sizeof(Datum) * natts + sizeof(bool) * natts;
        for (int i = 0; i < natts; i++) {
            if (copy->tts_isnull[i])
                continue;
            const FormData_pg_attribute* attr =
                copy->tts_tupleDescriptor ? copy->tts_tupleDescriptor->Attr(i + 1) : nullptr;
            if (attr != nullptr && !attr->attbyval) {
                const char* ptr = DatumGetTextP(copy->tts_values[i]);
                sz += VARSIZE(ptr);
            }
        }
        mem_used_ += sz;
        if (mem_used_ > work_mem_) {
            SpillToDisk();
        }
    }
    num_tuples_++;
}

void Tuplestore::SpillToDisk() {
    path_ = MakeTempPath();
    file_ = std::fopen(path_.c_str(), "w+b");
    if (file_ == nullptr) {
        ereport(LogLevel::kError, std::string("tuplestore: cannot create temp file ") + path_ +
                                      ": " + std::strerror(errno));
    }
    // Write all in-memory tuples to the file.
    for (TupleTableSlot* slot : mem_tuples_) {
        WriteTupleToFile(slot);
        destroyPallocNode(slot);
    }
    mem_tuples_.clear();
    mem_used_ = 0;
    spilled_ = true;
}

void Tuplestore::WriteTupleToFile(const TupleTableSlot* slot) {
    int natts = slot->Natts();
    WritePod(file_, natts);
    for (int i = 0; i < natts; i++) {
        bool is_null = slot->tts_isnull[i];
        WritePod(file_, is_null);
        if (is_null)
            continue;
        const FormData_pg_attribute* attr =
            slot->tts_tupleDescriptor ? slot->tts_tupleDescriptor->Attr(i + 1) : nullptr;
        Datum value = slot->tts_values[i];
        if (attr != nullptr && attr->attbyval) {
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
            const char* ptr = DatumGetTextP(value);
            int32_t total_len = VARSIZE(ptr);
            WritePod(file_, total_len);
            if (std::fwrite(VARDATA(ptr), 1, static_cast<size_t>(total_len - sizeof(int32_t)),
                            file_) != static_cast<size_t>(total_len - sizeof(int32_t))) {
                ereport(LogLevel::kError,
                        std::string("tuplestore: write data failed: ") + std::strerror(errno));
            }
        }
    }
}

TupleTableSlot* Tuplestore::ReadTupleFromFile() {
    int natts;
    if (!ReadPod(file_, natts)) {
        return nullptr;  // EOF
    }
    if (read_slot_ == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < natts; i++) {
        bool is_null;
        if (!ReadPod(file_, is_null)) {
            ereport(LogLevel::kError, std::string("tuplestore: unexpected EOF reading tuple"));
        }
        read_slot_->tts_isnull[i] = is_null;
        if (is_null) {
            read_slot_->tts_values[i] = 0;
            continue;
        }
        const FormData_pg_attribute* attr = tupdesc_ ? tupdesc_->Attr(i + 1) : nullptr;
        if (attr != nullptr && attr->attbyval) {
            int16_t len;
            if (!ReadPod(file_, len)) {
                ereport(LogLevel::kError, std::string("tuplestore: unexpected EOF reading length"));
            }
            Datum value = 0;
            size_t got = std::fread(&value, 1, static_cast<size_t>(len), file_);
            if (got != static_cast<size_t>(len)) {
                ereport(LogLevel::kError,
                        std::string("tuplestore: unexpected EOF reading byval datum"));
            }
            read_slot_->tts_values[i] = value;
        } else {
            int32_t total_len;
            if (!ReadPod(file_, total_len)) {
                ereport(LogLevel::kError,
                        std::string("tuplestore: unexpected EOF reading varlena length"));
            }
            char* buf = static_cast<char*>(palloc(total_len));
            int32_t len = total_len;
            std::memcpy(buf, &len, sizeof(int32_t));
            size_t want = static_cast<size_t>(total_len - sizeof(int32_t));
            size_t got = std::fread(buf + sizeof(int32_t), 1, want, file_);
            if (got != want) {
                ereport(LogLevel::kError,
                        std::string("tuplestore: unexpected EOF reading byref datum"));
            }
            read_slot_->tts_values[i] = reinterpret_cast<Datum>(buf);
        }
    }
    read_slot_->tts_nvalid = true;
    read_slot_->tts_isempty = false;
    return read_slot_;
}

TupleTableSlot* Tuplestore::GetTuple() {
    if (!spilled_) {
        // Read from in-memory buffer.
        if (read_index_ >= mem_tuples_.size()) {
            return nullptr;
        }
        return mem_tuples_[read_index_++];
    }
    // Read from file.
    if (writing_) {
        // Transition from writing to reading: flush and rewind.
        std::fflush(file_);
        Rewind();
    }
    return ReadTupleFromFile();
}

void Tuplestore::Rewind() {
    if (spilled_ && file_ != nullptr) {
        if (std::fseek(file_, 0, SEEK_SET) != 0) {
            ereport(LogLevel::kError,
                    std::string("tuplestore: rewind failed: ") + std::strerror(errno));
        }
        writing_ = false;
    }
    read_index_ = 0;
}

std::string Tuplestore::MakeTempPath() {
    return temp_dir_ + "/pgcpp_tuplestore_" + std::to_string(getpid()) + "_" +
           std::to_string(num_tuples_) + "_" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
           ".tmp";
}

}  // namespace pgcpp::sort
