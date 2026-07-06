// tuplesort.h — External merge sort for tuples (P1-3).
//
// Converted from PostgreSQL 15's src/include/utils/tuplesort.h.
//
// TupleSort collects tuples from a child plan node, sorts them by the
// specified sort keys, and returns them one at a time. When the input
// exceeds work_mem, tuples are spilled to disk using tape-based external
// merge sort (initial runs + k-way merge).
//
// MVP scope:
// - In-memory sort when data fits in work_mem (std::sort, same as before).
// - External merge sort when data exceeds work_mem (initial runs written
//   to temp files, k-way merge via priority queue during output).
// - Top-N optimization is NOT implemented (LIMIT applied after sort, as before).
// - Tuplestore is NOT implemented (P1-4 will add it for HashAgg/HashJoin).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "executor/tupletable.hpp"

namespace pgcpp::sort {

// SortKey — describes one column to sort by.
struct SortKey {
    int attnum = 0;                 // 1-based attribute number in the tuple
    pgcpp::catalog::Oid typid = 0;  // type OID for comparison
    bool reverse = false;           // DESC?
    bool nulls_first = false;       // NULLS FIRST?
};

// LogicalTape — sequential-access tape backed by a temp file.
//
// Tuples are serialized and written sequentially during the "write" phase.
// After Rewind(), tuples can be read back sequentially during the "read"
// phase. Each tape holds one sorted run. The tape needs the tuple
// descriptor to know how to serialize/deserialize each attribute
// (byval vs byref, attlen).
class LogicalTape {
public:
    LogicalTape(const std::string& path, pgcpp::access::TupleDesc tupdesc);
    ~LogicalTape();

    // Write a tuple to the tape (write phase).
    void WriteTuple(const pgcpp::executor::TupleTableSlot* slot);

    // Rewind the tape for reading.
    void Rewind();

    // Read the next tuple from the tape (read phase).
    // Returns nullptr at end-of-tape. The returned slot is palloc'd and
    // owned by the caller.
    pgcpp::executor::TupleTableSlot* ReadTuple();

    // Number of tuples written to this tape.
    int NumTuples() const { return num_tuples_; }

    // Delete the backing file (called on destruction or explicit cleanup).
    void Close();

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    bool writing_ = true;
    int num_tuples_ = 0;
    pgcpp::access::TupleDesc tupdesc_;
};

// TupleSort — external merge sort for tuples.
//
// Usage:
//   1. Construct with tuple descriptor, sort keys, and work_mem.
//   2. Call PutTuple() for each input tuple.
//   3. Call PerformSort() once all tuples are fed.
//   4. Call GetTuple() repeatedly until nullptr.
class TupleSort {
public:
    // Construct a TupleSort.
    //   tupdesc:   descriptor for the tuples to be sorted.
    //   keys:      sort key specifications (attnum, typid, reverse, nulls_first).
    //   work_mem:  maximum bytes of tuples to keep in memory before spilling.
    //   temp_dir:  directory for temp files when spilling (default: /tmp).
    TupleSort(pgcpp::access::TupleDesc tupdesc, std::vector<SortKey> keys, size_t work_mem,
              const std::string& temp_dir = "/tmp");
    ~TupleSort();

    // Phase 1: add a tuple. The slot's contents are copied (caller retains
    // ownership of the original slot).
    void PutTuple(pgcpp::executor::TupleTableSlot* slot);

    // Phase 2: finalize the sort. After this, GetTuple() can be called.
    void PerformSort();

    // Phase 3: return the next sorted tuple, or nullptr at end.
    // The returned slot is owned by TupleSort and valid until the next call.
    pgcpp::executor::TupleTableSlot* GetTuple();

    // Estimate the current memory usage (bytes).
    size_t MemUsed() const { return mem_used_; }

    // Number of runs spilled to disk.
    int NumRuns() const { return static_cast<int>(tapes_.size()); }

private:
    // State machine.
    enum class State { kInitial, kSorted };
    State state_ = State::kInitial;

    // Configuration.
    pgcpp::access::TupleDesc tupdesc_;
    std::vector<SortKey> keys_;
    size_t work_mem_;
    std::string temp_dir_;

    // In-memory buffer for the current (last) run.
    std::vector<pgcpp::executor::TupleTableSlot*> mem_tuples_;
    size_t mem_used_ = 0;

    // Spilled runs (tapes). Each tape holds a sorted run.
    std::vector<std::unique_ptr<LogicalTape>> tapes_;

    // Output state.
    // When no tapes exist: output from mem_tuples_ (sorted in place).
    // When tapes exist: k-way merge using a priority queue.
    size_t mem_output_index_ = 0;

    // For k-way merge: one buffered tuple per tape + the remaining mem run.
    struct MergeSlot {
        int source_id;                          // 0 = mem run, 1..N = tape N-1
        pgcpp::executor::TupleTableSlot* slot;  // current tuple from this source
    };
    std::vector<MergeSlot> merge_heap_;
    // The output slot for GetTuple (reused across calls).
    pgcpp::executor::TupleTableSlot* output_slot_ = nullptr;

    // Counter for generating unique temp file names.
    int tape_counter_ = 0;

    // --- Helper methods ---

    // Compare two slots by the sort keys. Returns -1, 0, or 1.
    int Compare(const pgcpp::executor::TupleTableSlot* a,
                const pgcpp::executor::TupleTableSlot* b) const;

    // Sort the in-memory buffer in place.
    void SortInMemory();

    // Dump the current in-memory buffer as a sorted run to a new tape.
    void DumpRun();

    // K-way merge of all tapes + remaining in-memory buffer.
    // Results are read on-the-fly via GetTuple().
    void BeginMerge();

    // Pull the next tuple from the merge heap (or mem run if no tapes).
    pgcpp::executor::TupleTableSlot* GetTupleFromMerge();

    // Estimate the memory footprint of a single slot.
    size_t EstimateSlotSize(const pgcpp::executor::TupleTableSlot* slot) const;

    // Create a unique temp file path.
    std::string MakeTempPath();
};

}  // namespace pgcpp::sort
