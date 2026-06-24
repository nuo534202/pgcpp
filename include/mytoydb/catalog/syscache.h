#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>

#include "mytoydb/catalog/catalog.h"

namespace mytoydb::catalog {

// SysCacheIdentifier — enumerates the cached lookups, mirroring PostgreSQL's
// SysCacheIdentifier enum (see utils/cache/syscache.h). Each entry names a
// (catalog, index) pair and the cache key arity.
enum class SysCacheIdentifier : int {
    kInvalid = 0,
    kClassName,            // pg_class by (name, namespace)
    kClassOid,             // pg_class by oid
    kAttributeRelidName,   // pg_attribute by (relid, name)
    kAttributeRelidNum,    // pg_attribute by (relid, attnum)
    kTypeName,             // pg_type by (name, namespace)
    kTypeOid,              // pg_type by oid
    kOperatorNameLrN,      // pg_operator by (name, left, right, namespace)
    kOperatorOid,          // pg_operator by oid
    kProcNameArgsNsp,      // pg_proc by (name, argtypes, namespace)
    kProcOid,              // pg_proc by oid
    kCastSourceTarget,     // pg_cast by (source, target)
    kCastOid,              // pg_cast by oid
    kAggregateFnoid,       // pg_aggregate by aggfnoid
    kCollationNameEncNsp,  // pg_collation by (name, encoding, namespace)
    kCollationOid,         // pg_collation by oid
};

// SysCache — the in-memory system cache.
//
// PostgreSQL's SysCache is a per-row cache backed by a hash table per
// (catalog, index) pair. In MyToyDB we replace the hand-written hash with
// std::unordered_map keyed by the appropriate tuple of columns. The public
// API (SearchSysCache / ReleaseSysCache) is preserved.
//
// The cache stores raw pointers to catalog rows owned by the Catalog; it does
// not copy rows. A reference count per entry mimics PostgreSQL's pin model so
// that callers can "pin" a row while inspecting it and "release" it after.
class SysCache {
public:
    SysCache() = default;
    ~SysCache() = default;

    SysCache(const SysCache&) = delete;
    SysCache& operator=(const SysCache&) = delete;

    // Initialize the cache from the given catalog. Walks the catalog rows and
    // builds the hash indexes. Safe to call once after bootstrap.
    void Init(const Catalog* catalog);

    // Invalidate all entries (PostgreSQL InvalidateSystemCaches equivalent).
    void Invalidate();

    // --- pg_class lookups ---

    // SearchSysCache1 for kClassName: lookup by (relname, relnamespace).
    // Returns the pinned row pointer, or nullptr on miss.
    const FormData_pg_class* SearchClassByName(const std::string& name, Oid namespace_oid) const;
    // SearchSysCache1 for kClassOid: lookup by oid.
    const FormData_pg_class* SearchClassByOid(Oid oid) const;

    // --- pg_attribute lookups ---
    const FormData_pg_attribute* SearchAttributeByName(Oid relid, const std::string& name) const;
    const FormData_pg_attribute* SearchAttributeByNum(Oid relid, int16_t attnum) const;

    // --- pg_type lookups ---
    const FormData_pg_type* SearchTypeByName(const std::string& name, Oid namespace_oid) const;
    const FormData_pg_type* SearchTypeByOid(Oid oid) const;

    // --- Pin management ---
    // PostgreSQL pins rows returned by SearchSysCache; ReleaseSysCache drops
    // the pin. In MyToyDB the rows are stable (owned by Catalog), so pins are
    // reference-counted for API compatibility and future invalidation safety.
    void Release(const void* entry) const;

    // Statistics (for testing/debugging)
    std::size_t ClassCacheSize() const { return class_by_oid_.size(); }
    std::size_t AttributeCacheSize() const { return attribute_by_relid_num_.size(); }
    std::size_t TypeCacheSize() const { return type_by_oid_.size(); }

private:
    // Hash indexes. Keys are tuples of the indexed columns.
    // class_by_oid_: oid -> row
    std::unordered_map<Oid, const FormData_pg_class*> class_by_oid_;
    // class_by_name_: (name, namespace) -> row
    std::unordered_map<std::uint64_t, const FormData_pg_class*> class_by_name_;
    // attribute_by_relid_num_: (relid, attnum) -> row
    std::unordered_map<std::uint64_t, const FormData_pg_attribute*> attribute_by_relid_num_;
    // attribute_by_relid_name_: (relid, name) -> row
    std::unordered_map<std::uint64_t, const FormData_pg_attribute*> attribute_by_relid_name_;
    // type_by_oid_: oid -> row
    std::unordered_map<Oid, const FormData_pg_type*> type_by_oid_;
    // type_by_name_: (name, namespace) -> row
    std::unordered_map<std::uint64_t, const FormData_pg_type*> type_by_name_;

    // Mutable reference counts (mutable so Release can be const, matching
    // PostgreSQL's SearchSysCache/ReleaseSysCache const-ish semantics).
    mutable std::unordered_map<const void*, int> refcounts_;

    // Pin/unpin helpers.
    void Pin(const void* entry) const;
    void Unpin(const void* entry) const;

    // Composite key helpers (pack two 32-bit values into one 64-bit key).
    static std::uint64_t MakeKey(Oid a, Oid b) {
        return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint32_t>(b);
    }
    static std::uint64_t MakeKey(Oid a, int16_t b) {
        return (static_cast<std::uint64_t>(a) << 32) |
               static_cast<std::uint32_t>(static_cast<uint16_t>(b));
    }
    // For string keys, hash the string and combine with the Oid.
    static std::uint64_t MakeKey(const std::string& s, Oid oid);
};

// Global syscache accessor.
SysCache* GetSysCache();
void SetSysCache(SysCache* cache);

// --- PostgreSQL-compatible SysCache API ---
//
// These mirror the SearchSysCache1 / ReleaseSysCache interface. The cache id
// selects the (catalog, index) pair. Key count and types depend on the cache
// id; we provide typed wrappers above (SearchClassByName, etc.) for safety,
// and the generic SearchSysCache1 for API compatibility.

// SearchSysCache1: lookup with a single key. The key is interpreted based on
// the cache id:
//   kClassOid        -> key is Oid
//   kTypeOid         -> key is Oid
//   kAttributeRelidNum -> key is (relid << 16 | attnum) packed into uintptr_t
// Returns the pinned row pointer, or nullptr on miss.
const void* SearchSysCache1(SysCacheIdentifier cache_id, uintptr_t key1);

// SearchSysCache2: lookup with two keys. Used for:
//   kClassName           -> (name as uintptr_t pointer to std::string, namespace_oid)
//   kAttributeRelidName  -> (relid, name as uintptr_t pointer to std::string)
//   kTypeName            -> (name as uintptr_t pointer to std::string, namespace_oid)
const void* SearchSysCache2(SysCacheIdentifier cache_id, uintptr_t key1, uintptr_t key2);

// ReleaseSysCache: drop the pin on a row returned by SearchSysCache*.
void ReleaseSysCache(const void* entry);

}  // namespace mytoydb::catalog
