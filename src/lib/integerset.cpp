// integerset.cpp — Integer set implementation.
//
// Backs pgcpp/lib/integerset.hpp with a std::set<uint64_t> store. The
// implementation matches PostgreSQL's IntegerSet public API
// (intset_add_member / intset_is_member / intset_num_members /
// intset_begin_iterate / intset_iterate_next) but trades the original's
// compressed bitmap batching for the simpler ordered-set representation.

#include "pgcpp/lib/integerset.hpp"

#include <utility>

namespace pgcpp::lib {

bool IntegerSet::Add(uint64_t x) {
    auto [it, inserted] = members_.insert(x);
    return inserted;
}

bool IntegerSet::Contains(uint64_t x) const {
    return members_.find(x) != members_.end();
}

void IntegerSet::BeginIterate() {
    cursor_ = members_.begin();
    iterating_ = true;
}

bool IntegerSet::IterateNext(uint64_t* out) {
    if (!iterating_ || cursor_ == members_.end()) {
        iterating_ = false;
        return false;
    }
    if (out != nullptr) {
        *out = *cursor_;
    }
    ++cursor_;
    return true;
}

std::vector<uint64_t> IntegerSet::ToSortedVector() const {
    return std::vector<uint64_t>(members_.begin(), members_.end());
}

}  // namespace pgcpp::lib
