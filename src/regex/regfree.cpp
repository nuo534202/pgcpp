// regfree.cpp — pg_regfree implementation.
//
// Releases a regex_t allocated by pg_regcomp. The destructor is invoked
// explicitly to drop the std::regex/std::string members, the destructor entry
// is removed from the owning MemoryContext (to avoid a double-destroy when the
// context is later reset), and the block is pfree'd.

#include "pgcpp/regex/regfree.hpp"

#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace mytoydb::regex {

using mytoydb::memory::AllocSetContext;
using mytoydb::memory::MemoryContext;
using mytoydb::memory::pfree;

void pg_regfree(regex_t* re) {
    if (re == nullptr) {
        return;
    }
    // Locate the owning context so we can remove the destructor entry before
    // destroying the object — otherwise CallRegisteredDestructors (run when the
    // context is reset/deleted) would invoke the destructor on already-freed
    // memory.
    MemoryContext* ctx = AllocSetContext::GetPointerContext(re);
    if (ctx != nullptr) {
        ctx->UnregisterDestructor(re);
    }
    re->~regex_t();
    pfree(re);
}

}  // namespace mytoydb::regex
