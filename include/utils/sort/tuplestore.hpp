// tuplestore.h — Append-only tuple store with optional disk spill (P1-4).
//
// Converted from PostgreSQL 15's src/include/utils/tuplestore.h.
//
// Tuplestore collects tuples in memory. When the in-memory buffer exceeds
// work_mem, all buffered tuples are spilled to a temp file. Subsequent
// PutTuple calls append directly to the file. Tuples are read back
// sequentially via GetTuple(); Rewind() restarts the read from the
// beginning.
//
// Unlike TupleSort, Tuplestore does NOT sort — it preserves insertion order.
// It is used by HashJoin batch spilling and (future) HashAgg spill.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::sort {

// Tuplestore — append-only tuple storage with disk spill.
class Tuplestore {
public:
    Tuplestore(pgcpp::access::TupleDesc tupdesc, size_t work_mem,
               const std::string& temp_dir = "/tmp");
    ~Tuplestore();

    // Append a tuple. The slot's contents are copied (caller retains
    // ownership of the original slot).
    void PutTuple(const pgcpp::executor::TupleTableSlot* slot);

    // Read the next tuple, or nullptr at end. The returned slot is owned
    // by Tuplestore and valid until the next call.
    pgcpp::executor::TupleTableSlot* GetTuple();

    // Restart reading from the beginning.
    void Rewind();

    // Estimate current memory usage (bytes).
    size_t MemUsed() const { return mem_used_; }

    // True if tuples have been spilled to disk.
    bool IsOnDisk() const { return spilled_; }

    // Total number of tuples stored.
    int NumTuples() const { return num_tuples_; }

private:
    pgcpp::access::TupleDesc tupdesc_;
    size_t work_mem_;
    std::string temp_dir_;

    // In-memory buffer (used until spill, then cleared).
    std::vector<pgcpp::executor::TupleTableSlot*> mem_tuples_;
    size_t mem_used_ = 0;

    // Read position into mem_tuples_ (when not spilled).
    size_t read_index_ = 0;

    // Disk spill state.
    bool spilled_ = false;
    std::FILE* file_ = nullptr;
    bool writing_ = true;
    int num_tuples_ = 0;
    std::string path_;

    // Reusable output slot for GetTuple when reading from disk.
    pgcpp::executor::TupleTableSlot* read_slot_ = nullptr;

    void SpillToDisk();
    void WriteTupleToFile(const pgcpp::executor::TupleTableSlot* slot);
    pgcpp::executor::TupleTableSlot* ReadTupleFromFile();
    std::string MakeTempPath();
};

}  // namespace pgcpp::sort
