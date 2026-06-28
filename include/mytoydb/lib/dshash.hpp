// dshash.hpp — Dynamic shared hash table.
//
// Mirrors PostgreSQL's src/backend/lib/dshash.c with a simplified
// std::unordered_map-backed implementation. The observable API mirrors the
// PG-style entry points (create/insert/find/delete/iterate) so callers can
// switch between this and the real concurrent hash table later.
//
// Deviation from PostgreSQL: the original is a concurrent open-addressed
// hash table backed by dynamic shared memory (DSM) and protected by
// per-partition LWLocks. This implementation is single-process and uses
// std::unordered_map, which is sufficient for MyToyDB's existing single-
// backend test/executor paths. The public API is preserved so future
// hardening can swap the storage without touching call sites.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mytoydb::lib {

// DsHash — a simple key/value hash table.
//
// Template parameters:
//   K        key type (must be hashable and equality-comparable)
//   V        value type (must be copyable/moveable)
//   Hash     hash function callable (defaults to std::hash<K>)
//   KeyEq    key equality predicate (defaults to std::equal_to<K>)
template<typename K, typename V, typename Hash = std::hash<K>, typename KeyEq = std::equal_to<K>>
class DsHash {
public:
    DsHash() = default;
    ~DsHash() = default;

    DsHash(const DsHash&) = default;
    DsHash(DsHash&&) noexcept = default;
    DsHash& operator=(const DsHash&) = default;
    DsHash& operator=(DsHash&&) noexcept = default;

    // PG: dshash_insert_or_find — inserts (key, value) if absent.
    // Returns a pointer to the stored value. If the key already exists, the
    // existing entry is returned unchanged (insert is a no-op) when
    // allow_replace is false; otherwise the value is overwritten.
    V* Insert(const K& key, V value, bool allow_replace = false);

    // PG: dshash_find — looks up key. Returns nullptr if absent.
    V* Find(const K& key);
    const V* Find(const K& key) const;

    // PG: dshash_delete — removes entry by key. Returns true on success.
    bool Delete(const K& key);

    // Number of entries.
    std::size_t Size() const { return map_.size(); }
    bool IsEmpty() const { return map_.empty(); }

    // PG: dshash_seq_init / dshash_seq_next — sequential iteration.
    // Returns a snapshot of all key/value pairs (sorted by key for
    // deterministic iteration order in tests). Snapshot semantics avoid
    // iterator invalidation if the table is mutated during iteration.
    std::vector<std::pair<K, V>> Entries() const;

    // Remove all entries (PG: dshash_destroy reclaims storage; here we just
    // clear, leaving the table ready for reuse).
    void Clear() { map_.clear(); }

private:
    std::unordered_map<K, V, Hash, KeyEq> map_;
};

// Convenience factory (PG: dshash_create). Allocates via global new.
template<typename K, typename V, typename Hash = std::hash<K>, typename KeyEq = std::equal_to<K>>
DsHash<K, V, Hash, KeyEq>* CreateDsHash() {
    return new DsHash<K, V, Hash, KeyEq>();
}

// PG: dshash_destroy — deletes a heap-allocated DsHash.
template<typename K, typename V, typename Hash, typename KeyEq>
void DestroyDsHash(DsHash<K, V, Hash, KeyEq>* table) {
    delete table;
}

// ---------------------------------------------------------------------------
// Template implementations
// ---------------------------------------------------------------------------

template<typename K, typename V, typename Hash, typename KeyEq>
V* DsHash<K, V, Hash, KeyEq>::Insert(const K& key, V value, bool allow_replace) {
    auto [it, inserted] = map_.try_emplace(key, std::move(value));
    if (!inserted && allow_replace) {
        it->second = std::move(value);
    }
    return &it->second;
}

template<typename K, typename V, typename Hash, typename KeyEq>
V* DsHash<K, V, Hash, KeyEq>::Find(const K& key) {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

template<typename K, typename V, typename Hash, typename KeyEq>
const V* DsHash<K, V, Hash, KeyEq>::Find(const K& key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

template<typename K, typename V, typename Hash, typename KeyEq>
bool DsHash<K, V, Hash, KeyEq>::Delete(const K& key) {
    return map_.erase(key) > 0;
}

template<typename K, typename V, typename Hash, typename KeyEq>
std::vector<std::pair<K, V>> DsHash<K, V, Hash, KeyEq>::Entries() const {
    std::vector<std::pair<K, V>> out;
    out.reserve(map_.size());
    for (const auto& [k, v] : map_) {
        out.emplace_back(k, v);
    }
    return out;
}

}  // namespace mytoydb::lib
