#include "mytoydb/common/memory/alloc_set.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mytoydb::memory {

// Internal block structure: a single large OS allocation carved into chunks.
struct AllocSetContext::Block {
    Block* next;
    char* end;
    std::size_t alloc_size;
};

// Internal chunk header prepended to every allocation. The user receives the
// memory immediately after this header.
struct AllocSetContext::ChunkHeader {
    ChunkHeader* next_in_freelist;
    MemoryContext* context;
    std::size_t size;
};

// Alignment for all chunk sizes (matches PostgreSQL MAXALIGN on 64-bit).
constexpr std::size_t kAlignment = 8;

inline std::size_t AlignUp(std::size_t size) {
    return (size + kAlignment - 1) & ~(kAlignment - 1);
}

MemoryContext* AllocSetContext::GetPointerContext(void* pointer) {
    auto* chunk = reinterpret_cast<ChunkHeader*>(static_cast<char*>(pointer) - sizeof(ChunkHeader));
    return chunk->context;
}

AllocSetContext::AllocSetContext(const char* name, std::size_t min_context_size,
                                 std::size_t init_block_size, std::size_t max_block_size)
    : MemoryContext(name),
      init_block_size_(init_block_size),
      max_block_size_(max_block_size),
      next_block_size_(init_block_size) {
    (void)min_context_size;  // not used in this simplified version
}

AllocSetContext::~AllocSetContext() {
    CallRegisteredDestructors();
    FreeBlocks();
}

int AllocSetContext::FreeListIndex(std::size_t size) const {
    if (size <= kMinFreeListChunkSize)
        return 0;
    int idx = 0;
    std::size_t threshold = kMinFreeListChunkSize;
    while (idx < kNumFreeLists && size > threshold) {
        threshold <<= 1;
        ++idx;
    }
    return idx;  // Returns kNumFreeLists if size exceeds the largest bucket.
}

void AllocSetContext::AllocNewBlock(std::size_t block_size) {
    std::size_t alloc_size = std::max(next_block_size_, block_size);
    if (alloc_size > max_block_size_) {
        alloc_size = std::max(block_size, max_block_size_);
    }

    char* raw = static_cast<char*>(std::malloc(alloc_size));
    if (raw == nullptr) {
        std::fprintf(stderr, "AllocSetContext: out of memory (requested %zu)\n", alloc_size);
        std::abort();
    }

    auto* block = reinterpret_cast<Block*>(raw);
    block->next = blocks_;
    block->end = raw + alloc_size;
    block->alloc_size = alloc_size;

    blocks_ = block;
    current_block_ = block;
    free_start_ = raw + sizeof(Block);
    free_end_ = block->end;

    // Grow next_block_size_ geometrically, capped at max_block_size_.
    if (next_block_size_ < max_block_size_) {
        next_block_size_ = std::min(next_block_size_ * 2, max_block_size_);
    }
}

void AllocSetContext::AddToFreeList(ChunkHeader* chunk) {
    int idx = FreeListIndex(chunk->size);
    if (idx < kNumFreeLists) {
        chunk->next_in_freelist = free_lists_[idx];
        free_lists_[idx] = chunk;
    }
    // Otherwise, leak the chunk (PostgreSQL behavior: non-bucketed chunks
    // are not reused until Reset).
}

void* AllocSetContext::Alloc(std::size_t size) {
    is_reset_ = false;

    std::size_t chunk_size = AlignUp(sizeof(ChunkHeader) + size);

    // Check the free list first if the chunk fits a bucket.
    int idx = FreeListIndex(chunk_size);
    if (idx < kNumFreeLists) {
        // Round up to the bucket size so all chunks in a bucket are uniform.
        chunk_size = kMinFreeListChunkSize << idx;
        if (free_lists_[idx] != nullptr) {
            ChunkHeader* chunk = free_lists_[idx];
            free_lists_[idx] = chunk->next_in_freelist;
            chunk->next_in_freelist = nullptr;
            chunk->context = this;
            return reinterpret_cast<char*>(chunk) + sizeof(ChunkHeader);
        }
    }

    // Allocate from the current block.
    if (current_block_ == nullptr || free_start_ + chunk_size > free_end_) {
        AllocNewBlock(sizeof(Block) + chunk_size);
    }

    auto* chunk = reinterpret_cast<ChunkHeader*>(free_start_);
    free_start_ += chunk_size;

    chunk->next_in_freelist = nullptr;
    chunk->context = this;
    chunk->size = chunk_size;

    return reinterpret_cast<char*>(chunk) + sizeof(ChunkHeader);
}

void AllocSetContext::Free(void* pointer) {
    if (pointer == nullptr)
        return;
    is_reset_ = false;

    auto* chunk = reinterpret_cast<ChunkHeader*>(static_cast<char*>(pointer) - sizeof(ChunkHeader));
    AddToFreeList(chunk);
}

void* AllocSetContext::Realloc(void* pointer, std::size_t size) {
    if (pointer == nullptr)
        return Alloc(size);

    auto* chunk = reinterpret_cast<ChunkHeader*>(static_cast<char*>(pointer) - sizeof(ChunkHeader));
    std::size_t old_user_size = chunk->size - sizeof(ChunkHeader);
    if (size <= old_user_size) {
        return pointer;  // Fits in the existing chunk.
    }

    // Allocate a new chunk, copy, and free the old one.
    void* new_pointer = Alloc(size);
    std::memcpy(new_pointer, pointer, old_user_size);
    Free(pointer);
    return new_pointer;
}

void AllocSetContext::Reset() {
    // Free all blocks except the first (keep the init block).
    Block* block = blocks_;
    if (block != nullptr) {
        Block* next = block->next;
        while (next != nullptr) {
            Block* to_free = next;
            next = next->next;
            std::free(to_free);
        }
        block->next = nullptr;
        blocks_ = block;
        current_block_ = block;
        free_start_ = reinterpret_cast<char*>(block) + sizeof(Block);
        free_end_ = block->end;
    }

    // Clear free lists.
    for (int i = 0; i < kNumFreeLists; ++i) {
        free_lists_[i] = nullptr;
    }

    next_block_size_ = init_block_size_;
    is_reset_ = true;
}

void AllocSetContext::FreeBlocks() {
    // Free all blocks.
    Block* block = blocks_;
    while (block != nullptr) {
        Block* next = block->next;
        std::free(block);
        block = next;
    }
    blocks_ = nullptr;
    current_block_ = nullptr;
    free_start_ = nullptr;
    free_end_ = nullptr;

    // Clear free lists.
    for (int i = 0; i < kNumFreeLists; ++i) {
        free_lists_[i] = nullptr;
    }
}

void AllocSetContext::Delete() {
    CallRegisteredDestructors();
    FreeBlocks();
    delete this;
}

bool AllocSetContext::IsEmpty() const {
    return is_reset_;
}

AllocSetContext* AllocSetContext::Create(const char* name, MemoryContext* parent,
                                         std::size_t min_context_size, std::size_t init_block_size,
                                         std::size_t max_block_size) {
    auto* context = new AllocSetContext(name, min_context_size, init_block_size, max_block_size);
    context->parent_ = parent;
    return context;
}

}  // namespace mytoydb::memory
