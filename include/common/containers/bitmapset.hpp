// bitmapset.h — C++ version of PostgreSQL's bitmapset.c.
//
// Converted from PostgreSQL 15's src/include/nodes/bitmapset.h and
// src/backend/nodes/bitmapset.c. A Bitmapset is a set of non-negative
// integer members (typically relids / attnos), packed into an array of
// 64-bit words. Unlike most node types, Bitmapset is NOT a Node subclass;
// it is allocated via global `new`/`delete` (not palloc) and freed via
// `bms_free` / RAII.
//
// PostgreSQL uses BITS_PER_BITMAPWORD = 32; this implementation uses 64-bit
// words (simpler, matches the native word size on 64-bit targets). The
// observable behavior is identical.
//
// Deviation from PostgreSQL: negative member indices are rejected with an
// Assert in PG; here they are treated as a no-op (AddMember/DelMember do
// nothing, IsMember returns false) so that callers do not crash in release
// builds. This matches the project rule of not using ereport(ERROR) for
// programmer errors that would be UB via longjmp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::nodes {

class Bitmapset {
public:
    Bitmapset() = default;
    explicit Bitmapset(int member) { AddMember(member); }

    Bitmapset(const Bitmapset&) = default;
    Bitmapset(Bitmapset&&) noexcept = default;
    Bitmapset& operator=(const Bitmapset&) = default;
    Bitmapset& operator=(Bitmapset&&) noexcept = default;

    // PG: bms_copy — deep copy. Allocated via global `new` (not palloc).
    Bitmapset* Copy() const { return new Bitmapset(*this); }

    // PG: bms_free — for compatibility. C++ callers can also let RAII do it.
    // Deletes this object. Safe to call on nullptr via the free function.
    void Free() { delete this; }

    bool IsEmpty() const;
    int NumMembers() const;
    int MinMember() const;  // returns -1 for empty set (PG: bms_minimum_member)
    int MaxMember() const;  // returns -1 for empty set

    Bitmapset& AddMember(int x);  // bms_add_member (mutates self)
    Bitmapset& DelMember(int x);  // bms_del_member
    bool IsMember(int x) const;   // bms_is_member

    bool Equals(const Bitmapset& other) const;              // bms_equal
    bool Overlap(const Bitmapset& other) const;             // bms_overlap
    bool IsSubset(const Bitmapset& other) const;            // bms_is_subset (a ⊆ b)
    bool NonemptyDifference(const Bitmapset& other) const;  // bms_nonempty_difference (a \ b ≠ ∅)

    // Set operations returning a new Bitmapset (allocated via global `new`).
    Bitmapset* Union(const Bitmapset& other) const;       // bms_union
    Bitmapset* Intersect(const Bitmapset& other) const;   // bms_intersect
    Bitmapset* Difference(const Bitmapset& other) const;  // bms_difference

    static Bitmapset* MakeSingleton(int x);  // bms_make_singleton

    // PG: bms_next_member. Returns the next member >= (prevbit + 1), or -2
    // when there are no more members. Pass -1 to start iteration.
    int NextMember(int prevbit) const;

    std::string ToString() const;

private:
    std::vector<uint64_t> words_;  // words_[i] bit j  <=>  member (i*64 + j)
    static constexpr int kBitsPerWord = 64;

    void EnsureCapacity(int x);
};

// ---------------------------------------------------------------------------
// PostgreSQL-style free functions (operate on Bitmapset* pointers, accept
// nullptr inputs following PG semantics).
// ---------------------------------------------------------------------------

Bitmapset* bms_make_singleton(int x);
Bitmapset* bms_add_member(Bitmapset* a, int x);  // creates a if nullptr
Bitmapset* bms_del_member(Bitmapset* a, int x);
bool bms_is_member(int x, const Bitmapset* a);
bool bms_is_empty(const Bitmapset* a);
int bms_num_members(const Bitmapset* a);
Bitmapset* bms_union(const Bitmapset* a, const Bitmapset* b);
Bitmapset* bms_intersect(const Bitmapset* a, const Bitmapset* b);
Bitmapset* bms_difference(const Bitmapset* a, const Bitmapset* b);
bool bms_equal(const Bitmapset* a, const Bitmapset* b);
bool bms_overlap(const Bitmapset* a, const Bitmapset* b);
bool bms_is_subset(const Bitmapset* a, const Bitmapset* b);
bool bms_nonempty_difference(const Bitmapset* a, const Bitmapset* b);
Bitmapset* bms_copy(const Bitmapset* a);
void bms_free(Bitmapset* a);
int bms_next_member(const Bitmapset* a, int prevbit);
int bms_minimum_member(const Bitmapset* a);

}  // namespace pgcpp::nodes
