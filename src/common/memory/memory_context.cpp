#include "mytoydb/common/memory/memory_context.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mytoydb/common/memory/alloc_set.h"

namespace mytoydb::memory {

namespace {

// File-scope current memory context (PostgreSQL's CurrentMemoryContext).
MemoryContext* current_context_ = nullptr;

}  // namespace

MemoryContext* GetCurrentMemoryContext() {
    return current_context_;
}

void SetCurrentMemoryContext(MemoryContext* context) {
    current_context_ = context;
}

ContextSwitchGuard::ContextSwitchGuard(MemoryContext* new_context)
    : old_context_(GetCurrentMemoryContext()) {
    SetCurrentMemoryContext(new_context);
}

ContextSwitchGuard::~ContextSwitchGuard() {
    SetCurrentMemoryContext(old_context_);
}

MemoryContextScope::MemoryContextScope(const char* name, MemoryContext* parent)
    : old_context_(GetCurrentMemoryContext()) {
    context_ = AllocSetContext::Create(name, parent);
    SetCurrentMemoryContext(context_);
}

MemoryContextScope::~MemoryContextScope() {
    SetCurrentMemoryContext(old_context_);
    delete context_;
}

void* palloc(std::size_t size) {
    MemoryContext* ctx = GetCurrentMemoryContext();
    if (ctx == nullptr) {
        std::fprintf(stderr, "palloc: CurrentMemoryContext is null\n");
        std::abort();
    }
    return ctx->Alloc(size);
}

void* palloc0(std::size_t size) {
    MemoryContext* ctx = GetCurrentMemoryContext();
    if (ctx == nullptr) {
        std::fprintf(stderr, "palloc0: CurrentMemoryContext is null\n");
        std::abort();
    }
    void* p = ctx->Alloc(size);
    std::memset(p, 0, size);
    return p;
}

void* repalloc(void* pointer, std::size_t size) {
    MemoryContext* ctx = GetCurrentMemoryContext();
    if (ctx == nullptr) {
        std::fprintf(stderr, "repalloc: CurrentMemoryContext is null\n");
        std::abort();
    }
    return ctx->Realloc(pointer, size);
}

void pfree(void* pointer) {
    MemoryContext* ctx = GetCurrentMemoryContext();
    if (ctx == nullptr) {
        std::fprintf(stderr, "pfree: CurrentMemoryContext is null\n");
        std::abort();
    }
    ctx->Free(pointer);
}

void* MemoryContextAlloc(MemoryContext* context, std::size_t size) {
    if (context == nullptr) {
        std::fprintf(stderr, "MemoryContextAlloc: context is null\n");
        std::abort();
    }
    return context->Alloc(size);
}

void* MemoryContextAllocZero(MemoryContext* context, std::size_t size) {
    if (context == nullptr) {
        std::fprintf(stderr, "MemoryContextAllocZero: context is null\n");
        std::abort();
    }
    void* p = context->Alloc(size);
    std::memset(p, 0, size);
    return p;
}

}  // namespace mytoydb::memory
