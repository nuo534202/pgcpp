#pragma once

#include <cstddef>
#include <cstdint>

namespace mytoydb::memory {

// Forward declarations
class MemoryContext;
class MemoryContextMethods;

// Opaque handle kept for PostgreSQL API compatibility. Internally a
// MemoryContext is a pointer to a MemoryContextData-derived object.
using MemoryContextPointer = MemoryContext*;

// Severity / type constants preserved from PostgreSQL
constexpr std::size_t kStandardChunkSize = 8192;

// Function pointer type for registered C++ destructors.
using DestructorFn = void (*)(void*);

// DestructorEntry — a linked-list node tracking a C++ object allocated in
// this memory context via palloc + placement new. When the context is
// deleted, the destructor is called before the palloc'd blocks are freed,
// ensuring that std::vector/std::string internal buffers (allocated via
// operator new) are properly released.
struct DestructorEntry {
    void* obj;
    DestructorFn fn;
    DestructorEntry* next;
};

// The MemoryContext abstract base class.
// In PostgreSQL C, this is MemoryContextData with a MemoryContextMethods*
// virtual table. In C++ we use native virtual functions instead.
class MemoryContext {
public:
    virtual ~MemoryContext() = default;

    // Core allocation primitives (the "methods" table in PostgreSQL C).
    virtual void* Alloc(std::size_t size) = 0;
    virtual void Free(void* pointer) = 0;
    virtual void* Realloc(void* pointer, std::size_t size) = 0;
    virtual void Reset() = 0;
    virtual void Delete() = 0;
    virtual bool IsEmpty() const = 0;

    // Hierarchy accessors
    MemoryContext* GetParent() const { return parent_; }
    void SetParent(MemoryContext* parent) { parent_ = parent; }
    const char* GetName() const { return name_; }
    void SetName(const char* name) { name_ = name; }

    // Whether the context has been reset since last allocation
    bool IsReset() const { return is_reset_; }

    // Register a C++ destructor to be called when this context is deleted.
    // Used by makeNode<T>() to ensure std::vector/std::string members of
    // palloc'd nodes are properly cleaned up.
    void RegisterDestructor(void* obj, DestructorFn fn);

    // Remove the destructor entry for the given object. Call this before
    // explicitly destroying a palloc'd C++ object (e.g., via obj->~T() +
    // pfree) to prevent CallRegisteredDestructors from invoking the
    // destructor again on already-freed memory.
    void UnregisterDestructor(void* obj);

    // Call all registered destructors and clear the list.
    void CallRegisteredDestructors();

protected:
    MemoryContext() = default;
    explicit MemoryContext(const char* name) : name_(name) {}

    MemoryContext* parent_ = nullptr;
    const char* name_ = "unnamed";
    bool is_reset_ = true;
    DestructorEntry* destructors_ = nullptr;
};

// Global current-memory-context accessors (PostgreSQL CurrentMemoryContext).
MemoryContext* GetCurrentMemoryContext();
void SetCurrentMemoryContext(MemoryContext* context);

// RAII guard that switches to a context on construction and restores the
// previous context on destruction. Equivalent to:
//   MemoryContext old = MemoryContextSwitchTo(new_ctx);
//   ... work ...
//   MemoryContextSwitchTo(old);
class ContextSwitchGuard {
public:
    explicit ContextSwitchGuard(MemoryContext* new_context);
    ~ContextSwitchGuard();
    ContextSwitchGuard(const ContextSwitchGuard&) = delete;
    ContextSwitchGuard& operator=(const ContextSwitchGuard&) = delete;
    MemoryContext* GetOldContext() const { return old_context_; }

private:
    MemoryContext* old_context_;
};

// RAII scope that creates a context on construction and deletes it on
// destruction. Useful for scoped temporary allocations.
class MemoryContextScope {
public:
    MemoryContextScope(const char* name, MemoryContext* parent = nullptr);
    ~MemoryContextScope();
    MemoryContextScope(const MemoryContextScope&) = delete;
    MemoryContextScope& operator=(const MemoryContextScope&) = delete;
    MemoryContext* Get() const { return context_; }
    operator MemoryContext*() const { return context_; }

private:
    MemoryContext* context_;
    MemoryContext* old_context_;
};

// PostgreSQL-compatible allocation API. These operate on CurrentMemoryContext.
// The names are intentionally kept lowercase to match PostgreSQL's public API.
void* palloc(std::size_t size);
void* palloc0(std::size_t size);
void* repalloc(void* pointer, std::size_t size);
void pfree(void* pointer);

// Allocate within a specific context (PostgreSQL MemoryContextAlloc).
void* MemoryContextAlloc(MemoryContext* context, std::size_t size);
void* MemoryContextAllocZero(MemoryContext* context, std::size_t size);

// Typed allocation helper (C++ convenience template).
template<typename T>
T* ContextAlloc(MemoryContext* context) {
    return static_cast<T*>(MemoryContextAlloc(context, sizeof(T)));
}

template<typename T>
T* ContextAllocArray(MemoryContext* context, std::size_t count) {
    return static_cast<T*>(MemoryContextAlloc(context, sizeof(T) * count));
}

}  // namespace mytoydb::memory
