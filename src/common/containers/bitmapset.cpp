// bitmapset.cpp — Bitmapset member + free-function implementations.
//
// See bitmapset.hpp for design notes. Storage uses std::vector<uint64_t>;
// member x lives in words_[x / 64] bit (x % 64). The words_ vector is sized
// exactly to hold the highest set bit; trailing zero words are trimmed by
// DelMember so that Equals/IsEmpty can be implemented by simple vector
// comparison.

#include "pgcpp/common/containers/bitmapset.hpp"

#include <algorithm>
#include <cstdio>

namespace mytoydb::nodes {

namespace {

constexpr int kBitsPerWord = 64;

inline int WordIndex(int x) {
    return x / kBitsPerWord;
}
inline int BitIndex(int x) {
    return x % kBitsPerWord;
}
inline uint64_t BitMask(int x) {
    return uint64_t{1} << BitIndex(x);
}

}  // namespace

void Bitmapset::EnsureCapacity(int x) {
    if (x < 0) {
        return;
    }
    int needed = WordIndex(x) + 1;
    if (static_cast<int>(words_.size()) < needed) {
        words_.resize(static_cast<std::size_t>(needed), 0);
    }
}

// ---------------------------------------------------------------------------
// Single-object API
// ---------------------------------------------------------------------------

bool Bitmapset::IsEmpty() const {
    for (uint64_t w : words_) {
        if (w != 0) {
            return false;
        }
    }
    return true;
}

int Bitmapset::NumMembers() const {
    int count = 0;
    for (uint64_t w : words_) {
        // __builtin_popcountll is available on GCC/Clang.
        count += __builtin_popcountll(w);
    }
    return count;
}

int Bitmapset::MinMember() const {
    for (std::size_t i = 0; i < words_.size(); ++i) {
        if (words_[i] != 0) {
            return static_cast<int>(i) * kBitsPerWord + __builtin_ctzll(words_[i]);
        }
    }
    return -1;
}

int Bitmapset::MaxMember() const {
    for (std::size_t i = words_.size(); i > 0; --i) {
        uint64_t w = words_[i - 1];
        if (w != 0) {
            // 63 - clz(w) gives the index of the highest set bit.
            return static_cast<int>(i - 1) * kBitsPerWord + (kBitsPerWord - 1 - __builtin_clzll(w));
        }
    }
    return -1;
}

Bitmapset& Bitmapset::AddMember(int x) {
    if (x < 0) {
        return *this;  // negative members rejected (PG uses Assert)
    }
    EnsureCapacity(x);
    words_[WordIndex(x)] |= BitMask(x);
    return *this;
}

Bitmapset& Bitmapset::DelMember(int x) {
    if (x < 0) {
        return *this;
    }
    int idx = WordIndex(x);
    if (idx < static_cast<int>(words_.size())) {
        words_[idx] &= ~BitMask(x);
        // Trim trailing zero words so two sets with the same members compare
        // equal via vector equality (and IsEmpty works on size() == 0).
        while (!words_.empty() && words_.back() == 0) {
            words_.pop_back();
        }
    }
    return *this;
}

bool Bitmapset::IsMember(int x) const {
    if (x < 0) {
        return false;
    }
    int idx = WordIndex(x);
    if (idx >= static_cast<int>(words_.size())) {
        return false;
    }
    return (words_[idx] & BitMask(x)) != 0;
}

bool Bitmapset::Equals(const Bitmapset& other) const {
    // Two sets are equal iff their trimmed word vectors match. Trim a copy
    // if needed (we trim on DelMember, but a set built only via AddMember
    // may still have trailing zero words only if a higher bit was added then
    // removed indirectly — which cannot happen here). To be safe, compare
    // word-by-word treating missing high words as zero.
    int n = static_cast<int>(std::max(words_.size(), other.words_.size()));
    for (int i = 0; i < n; ++i) {
        uint64_t a = i < static_cast<int>(words_.size()) ? words_[i] : 0;
        uint64_t b = i < static_cast<int>(other.words_.size()) ? other.words_[i] : 0;
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool Bitmapset::Overlap(const Bitmapset& other) const {
    int n = static_cast<int>(std::min(words_.size(), other.words_.size()));
    for (int i = 0; i < n; ++i) {
        if ((words_[i] & other.words_[i]) != 0) {
            return true;
        }
    }
    return false;
}

bool Bitmapset::IsSubset(const Bitmapset& other) const {
    // a ⊆ b  iff  (a & ~b) == 0
    int na = static_cast<int>(words_.size());
    int nb = static_cast<int>(other.words_.size());
    for (int i = 0; i < na; ++i) {
        uint64_t b = i < nb ? other.words_[i] : 0;
        if ((words_[i] & ~b) != 0) {
            return false;
        }
    }
    return true;
}

bool Bitmapset::NonemptyDifference(const Bitmapset& other) const {
    // (a \ b) ≠ ∅  iff  (a & ~b) != 0
    int na = static_cast<int>(words_.size());
    int nb = static_cast<int>(other.words_.size());
    for (int i = 0; i < na; ++i) {
        uint64_t b = i < nb ? other.words_[i] : 0;
        if ((words_[i] & ~b) != 0) {
            return true;
        }
    }
    return false;
}

Bitmapset* Bitmapset::Union(const Bitmapset& other) const {
    auto* result = new Bitmapset(*this);
    int nb = static_cast<int>(other.words_.size());
    if (static_cast<int>(result->words_.size()) < nb) {
        result->words_.resize(static_cast<std::size_t>(nb), 0);
    }
    for (int i = 0; i < nb; ++i) {
        result->words_[i] |= other.words_[i];
    }
    return result;
}

Bitmapset* Bitmapset::Intersect(const Bitmapset& other) const {
    auto* result = new Bitmapset();
    int n = static_cast<int>(std::min(words_.size(), other.words_.size()));
    result->words_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        result->words_[i] = words_[i] & other.words_[i];
    }
    while (!result->words_.empty() && result->words_.back() == 0) {
        result->words_.pop_back();
    }
    return result;
}

Bitmapset* Bitmapset::Difference(const Bitmapset& other) const {
    auto* result = new Bitmapset(*this);
    int nb = static_cast<int>(other.words_.size());
    int n = static_cast<int>(std::min(result->words_.size(), other.words_.size()));
    for (int i = 0; i < n; ++i) {
        result->words_[i] &= ~other.words_[i];
    }
    (void)nb;
    while (!result->words_.empty() && result->words_.back() == 0) {
        result->words_.pop_back();
    }
    return result;
}

Bitmapset* Bitmapset::MakeSingleton(int x) {
    auto* bms = new Bitmapset();
    bms->AddMember(x);
    return bms;
}

int Bitmapset::NextMember(int prevbit) const {
    int start = prevbit + 1;
    if (start < 0) {
        start = 0;
    }
    int idx = WordIndex(start);
    int bit = BitIndex(start);
    if (idx >= static_cast<int>(words_.size())) {
        return -2;
    }
    // Mask off bits below `bit` in the first word we examine.
    uint64_t w = words_[idx] & (~uint64_t{0} << bit);
    while (true) {
        if (w != 0) {
            return idx * kBitsPerWord + __builtin_ctzll(w);
        }
        ++idx;
        if (idx >= static_cast<int>(words_.size())) {
            return -2;
        }
        w = words_[idx];
    }
}

std::string Bitmapset::ToString() const {
    std::string s = "{";
    bool first = true;
    int prev = -1;
    while (true) {
        int m = NextMember(prev);
        if (m == -2) {
            break;
        }
        if (!first) {
            s += ",";
        }
        first = false;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", m);
        s += buf;
        prev = m;
    }
    s += "}";
    return s;
}

// ---------------------------------------------------------------------------
// PG-style free functions (handle nullptr inputs)
// ---------------------------------------------------------------------------

Bitmapset* bms_make_singleton(int x) {
    return Bitmapset::MakeSingleton(x);
}

Bitmapset* bms_add_member(Bitmapset* a, int x) {
    if (a == nullptr) {
        a = new Bitmapset();
    }
    a->AddMember(x);
    return a;
}

Bitmapset* bms_del_member(Bitmapset* a, int x) {
    if (a != nullptr) {
        a->DelMember(x);
    }
    return a;
}

bool bms_is_member(int x, const Bitmapset* a) {
    if (a == nullptr) {
        return false;
    }
    return a->IsMember(x);
}

bool bms_is_empty(const Bitmapset* a) {
    if (a == nullptr) {
        return true;
    }
    return a->IsEmpty();
}

int bms_num_members(const Bitmapset* a) {
    if (a == nullptr) {
        return 0;
    }
    return a->NumMembers();
}

Bitmapset* bms_union(const Bitmapset* a, const Bitmapset* b) {
    if (a == nullptr && b == nullptr) {
        return nullptr;
    }
    if (a == nullptr) {
        return b->Copy();
    }
    if (b == nullptr) {
        return a->Copy();
    }
    return a->Union(*b);
}

Bitmapset* bms_intersect(const Bitmapset* a, const Bitmapset* b) {
    if (a == nullptr || b == nullptr) {
        return nullptr;
    }
    return a->Intersect(*b);
}

Bitmapset* bms_difference(const Bitmapset* a, const Bitmapset* b) {
    if (a == nullptr) {
        return nullptr;
    }
    if (b == nullptr) {
        return a->Copy();
    }
    return a->Difference(*b);
}

bool bms_equal(const Bitmapset* a, const Bitmapset* b) {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        // Two nullptrs handled above; one nullptr + one empty set is equal.
        const Bitmapset* nonnull = a != nullptr ? a : b;
        return nonnull->IsEmpty();
    }
    return a->Equals(*b);
}

bool bms_overlap(const Bitmapset* a, const Bitmapset* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return a->Overlap(*b);
}

bool bms_is_subset(const Bitmapset* a, const Bitmapset* b) {
    if (a == nullptr) {
        return true;  // empty set is a subset of anything
    }
    if (b == nullptr) {
        return a->IsEmpty();
    }
    return a->IsSubset(*b);
}

bool bms_nonempty_difference(const Bitmapset* a, const Bitmapset* b) {
    if (a == nullptr) {
        return false;
    }
    if (b == nullptr) {
        return !a->IsEmpty();
    }
    return a->NonemptyDifference(*b);
}

Bitmapset* bms_copy(const Bitmapset* a) {
    if (a == nullptr) {
        return nullptr;
    }
    return a->Copy();
}

void bms_free(Bitmapset* a) {
    delete a;  // delete nullptr is a no-op
}

int bms_next_member(const Bitmapset* a, int prevbit) {
    if (a == nullptr) {
        return -2;
    }
    return a->NextMember(prevbit);
}

int bms_minimum_member(const Bitmapset* a) {
    if (a == nullptr) {
        return -1;
    }
    return a->MinMember();
}

}  // namespace mytoydb::nodes
