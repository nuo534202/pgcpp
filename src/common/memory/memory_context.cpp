#include "pgcpp/common/memory/memory_context.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pgcpp/common/memory/alloc_set.hpp"

namespace pgcpp::memory {

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
    if (pointer == nullptr)
        return;
    // Look up the owning context from the chunk header rather than relying
    // on CurrentMemoryContext. This allows pfree to work correctly even when
    // the current context is null (e.g., during MemoryContext::Delete()
    // when registered destructors call pfree).
    MemoryContext* ctx = AllocSetContext::GetPointerContext(pointer);
    if (ctx == nullptr) {
        std::fprintf(stderr, "pfree: chunk context is null\n");
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

void MemoryContext::RegisterDestructor(void* obj, DestructorFn fn) {
    if (obj == nullptr || fn == nullptr)
        return;
    auto* entry = static_cast<DestructorEntry*>(this->Alloc(sizeof(DestructorEntry)));
    entry->obj = obj;
    entry->fn = fn;
    entry->next = destructors_;
    destructors_ = entry;
}

void MemoryContext::CallRegisteredDestructors() {
    DestructorEntry* entry = destructors_;
    while (entry != nullptr) {
        if (entry->fn != nullptr && entry->obj != nullptr) {
            entry->fn(entry->obj);
        }
        entry = entry->next;
    }
    destructors_ = nullptr;
}

void MemoryContext::UnregisterDestructor(void* obj) {
    if (obj == nullptr)
        return;
    DestructorEntry** pp = &destructors_;
    while (*pp != nullptr) {
        if ((*pp)->obj == obj) {
            DestructorEntry* to_remove = *pp;
            *pp = to_remove->next;
            // The entry itself is palloc'd in this context; it will be freed
            // when the context is deleted. Just unlink it.
            return;
        }
        pp = &((*pp)->next);
    }
}

}  // namespace pgcpp::memory
