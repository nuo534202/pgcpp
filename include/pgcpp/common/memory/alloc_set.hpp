#pragma once

#include "pgcpp/common/memory/memory_context.hpp"

namespace pgcpp::memory {

// AllocSetContext — the default MemoryContext implementation.
// Faithful conversion of PostgreSQL's AllocSetContext (aset.c).
// Uses a block-chunk allocator: large blocks are allocated from the OS and
// carved into chunks; freed chunks go to a size-bucketed free list.
class AllocSetContext : public MemoryContext {
public:
    AllocSetContext(const char* name, std::size_t min_context_size, std::size_t init_block_size,
                    std::size_t max_block_size);
    ~AllocSetContext() override;

    // MemoryContext interface
    void* Alloc(std::size_t size) override;
    void Free(void* pointer) override;
    void* Realloc(void* pointer, std::size_t size) override;
    void Reset() override;
    void Delete() override;
    bool IsEmpty() const override;

    // Factory: create a new AllocSetContext as a child of parent.
    static AllocSetContext* Create(const char* name, MemoryContext* parent = nullptr,
                                   std::size_t min_context_size = 0,
                                   std::size_t init_block_size = 8192,
                                   std::size_t max_block_size = 8192 * 16);

    // Extract the owning MemoryContext from a palloc'd pointer by reading
    // the ChunkHeader stored immediately before the user data. This allows
    // pfree to work without relying on CurrentMemoryContext being set.
    static MemoryContext* GetPointerContext(void* pointer);

private:
    // Internal block structure (linked list of large OS allocations)
    struct Block;
    struct ChunkHeader;

    Block* blocks_ = nullptr;         // head of block list
    Block* current_block_ = nullptr;  // block for next allocation
    char* free_start_ = nullptr;      // next free byte in current_block_
    char* free_end_ = nullptr;        // end of allocatable region in current_block_

    // Free list: buckets indexed by power-of-2 chunk size.
    // Following PostgreSQL's ALLOCSET_NUM_FREELISTS approach.
    static constexpr int kNumFreeLists = 11;
    static constexpr std::size_t kMinFreeListChunkSize = 1 << 8;  // 256 bytes
    ChunkHeader* free_lists_[kNumFreeLists] = {};

    std::size_t init_block_size_;
    std::size_t max_block_size_;
    std::size_t next_block_size_;

    // Helpers
    int FreeListIndex(std::size_t size) const;
    void AllocNewBlock(std::size_t block_size);
    void AddToFreeList(ChunkHeader* chunk);
    void FreeBlocks();
};

}  // namespace pgcpp::memory
