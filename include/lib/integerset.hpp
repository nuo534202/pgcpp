// integerset.hpp — Integer set with bitmap compression.
//
// Mirrors PostgreSQL's src/backend/lib/integerset.c. The original packs
// members into a tree of "batches" each holding a bitmap of bit-test ranges,
// giving O(1) membership test for clustered inputs. This C++ port keeps the
// observable API (Add/Contains/NumMembers/Iterate) but stores members in a
// std::set<uint64_t> for simplicity; the public interface is stable so a
// later port can swap in the compressed bitmap implementation without
// touching callers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace pgcpp::lib {

class IntegerSet {
public:
    IntegerSet() = default;
    ~IntegerSet() = default;

    IntegerSet(const IntegerSet&) = default;
    IntegerSet(IntegerSet&&) noexcept = default;
    IntegerSet& operator=(const IntegerSet&) = default;
    IntegerSet& operator=(IntegerSet&&) noexcept = default;

    // PG: intset_add_member — adds x if not already present. Returns true on
    // insertion, false if the value was already a member.
    bool Add(uint64_t x);

    // PG: intset_is_member — membership test.
    bool Contains(uint64_t x) const;

    // PG: intset_num_members — current cardinality.
    std::size_t NumMembers() const { return members_.size(); }

    bool IsEmpty() const { return members_.empty(); }

    // PG: intset_begin_iterate / intset_iterate_next — sequential iteration.
    // The returned values are in ascending order. Returns false when there
    // are no more members.
    void BeginIterate();
    bool IterateNext(uint64_t* out);

    // Snapshot the set in ascending order (helper for tests).
    std::vector<uint64_t> ToSortedVector() const;

    void Reset() { members_.clear(); }

private:
    std::set<uint64_t> members_;
    std::set<uint64_t>::const_iterator cursor_{};
    bool iterating_ = false;
};

}  // namespace pgcpp::lib
